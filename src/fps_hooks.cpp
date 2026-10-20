// Frame-rate hooks for WWE SmackDown vs. Raw 2008.
//
// The game's statically linked Xbox 360 D3D Swap (sub_8223A4B8) reads the device's present
// interval (D3DPRESENT_INTERVAL_*, device+13572) at 0x8223A56C and turns it into the number of
// vblanks each frame stays on screen: ONE (1) -> every vblank (60 fps), TWO (2) -> every second
// vblank (30 fps). The flip itself is scheduled by sub_8223A320 at "last flip + interval".
//
// SvrPresentIntervalHook runs right after that load (config/default.toml [[midasm_hook]]).

#include "frame_stats.h"

#include "core/memory_helpers.h"

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>

REXCVAR_DEFINE_BOOL(svr_60fps, false, "Game",
                    "Keep the game's frame-rate mode at 60 (50 PAL) instead of dropping to 30 (25) "
                    "in matches and cutscenes; game timing follows the mode");

// The game keeps a frame-rate mode in a global struct at 0x82E6E458: +8 target fps (60/50/30/25),
// +28/+32/+56/+60 per-frame time-step floats and +36 ms per frame, all set together by
// SetFrameRate (sub_824707A8), "drop to 30" (sub_82470A18) and "back to 60" (sub_82470AC0). About
// 140 functions read the target fps, and sub_82471190 turns 30/25 into D3D present interval 2.
// Overriding only the present interval made the game draw at 60 but still step its logic as if
// at 30 (entrances at double speed); keeping the mode at 60 changes both together.

void SvrPresentIntervalHook(PPCRegister& r10) {
  // Log each change of the interval D3D Swap uses (follows the frame-rate mode).
  static std::atomic<uint32_t> last_logged{0xFFFFFFFFu};
  uint32_t requested = r10.u32;
  if (last_logged.exchange(requested, std::memory_order_relaxed) != requested) {
    REXLOG_INFO("[fps] game present interval: {} ({})", requested,
                requested == 2 ? "30 fps" : requested <= 1 ? "60 fps" : "other");
  }
}

// SetFrameRate(fps), r3 = requested rate (sub_824707A8 entry).
void SvrSetFrameRateHook(PPCRegister& r3) {
  uint32_t requested = r3.u32;
  if (REXCVAR_GET(svr_60fps) && (requested == 30 || requested == 25)) {
    r3.u64 = requested == 30 ? 60 : 50;
    REXLOG_INFO("[fps] game set frame rate {}; using {}", requested, r3.u32);
  } else {
    REXLOG_INFO("[fps] game set frame rate {}", requested);
  }
}

// "Drop to 30 (25) fps mode" (sub_82470A18 entry); returning true skips the function.
bool SvrSkip30ModeHook() {
  bool skip = REXCVAR_GET(svr_60fps);
  static std::atomic<uint32_t> calls{0};
  uint32_t n = calls.fetch_add(1, std::memory_order_relaxed);
  if (n < 20 || n % 100 == 0) {
    REXLOG_INFO("[fps] game switched to 30 fps mode{}", skip ? "; kept at 60" : "");
  }
  return skip;
}

// Cutscene captions: sub_8226BC80 hands the caption display the cutscene's time (f1, at the bctrl
// 0x8226BDD8), counted from the scene clock (sub_82293150: scene object +192), which goes up by one
// every frame. The captions' start and end times are in 30 fps frames, so in the 60 fps mode every
// caption came at half its time and stayed half as long. Hand it 30 fps frames.
constexpr uint32_t kFrameRateMode = 0x82E6E458;  // +8: target fps (60/50/30/25)
void SvrCaptionTimeHook(PPCRegister& f1) {
  if (f1.f64 > 0 && bd::mem::load<uint32_t>(kFrameRateMode + 8) >= 50)
    f1.f64 *= 0.5;
}

// Frame counter: SvrFrameSwapHook runs in the game's swap function (sub_8223A960) right before it
// calls VdSwap at 0x8223ABAC, once per presented frame.
namespace {
using Clock = std::chrono::steady_clock;
constexpr size_t kFrameHistory = 512;  // > 1 s of frames at any realistic rate
std::mutex frame_mutex;
Clock::time_point frame_times[kFrameHistory];
uint64_t frame_count = 0;
}  // namespace

void SvrFrameSwapHook() {
  std::lock_guard lock(frame_mutex);
  frame_times[frame_count % kFrameHistory] = Clock::now();
  ++frame_count;
}

SvrFrameStats GetSvrFrameStats() {
  std::lock_guard lock(frame_mutex);
  SvrFrameStats stats;
  stats.frames = frame_count;
  if (frame_count < 2) {
    return stats;
  }
  auto now = Clock::now();
  auto newest = frame_times[(frame_count - 1) % kFrameHistory];
  // Count frames presented within the last second.
  uint64_t available = std::min<uint64_t>(frame_count, kFrameHistory);
  uint64_t in_window = 0;
  Clock::time_point oldest = newest;
  for (uint64_t i = 1; i <= available; ++i) {
    auto t = frame_times[(frame_count - i) % kFrameHistory];
    if (now - t > std::chrono::seconds(1)) {
      break;
    }
    oldest = t;
    ++in_window;
  }
  stats.fps = double(in_window);
  if (in_window >= 2) {
    stats.frame_time_ms =
        std::chrono::duration<double, std::milli>(newest - oldest).count() / double(in_window - 1);
  }
  return stats;
}
