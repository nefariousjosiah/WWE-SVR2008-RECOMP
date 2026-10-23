// Wide and tall screens (settings menu > Screen shape: 16:10, 21:9, 32:9).
//
// Matches, entrances and cutscenes fill the shape: the camera itself is given the screen's aspect,
// so the game frames and culls a wider (or taller) view and nothing is stretched. The native
// renderer draws those scenes into correspondingly wider (taller) targets and keeps the 2D HUD in
// the 16:9 middle (svr_resources.h SvrFrameShape). Menus stay 16:9 in the middle of the screen.
//
// The camera: sub_8247D120 builds every scene camera's projection with sub_8247CA20(matrix,
// f1 = field of view, f2 = aspect, near, far), the aspect being 16:9 (0x8204B178) or 4:3 for the
// console's display mode. The field of view is vertical (m11 = cot(0.5 * f1), m00 = m11 / f2):
// - wider than 16:9: f2 = the screen's aspect; the vertical view stays, the sides widen;
// - narrower (16:10): f2 = the screen's aspect and the vertical view widened by as much, so the
//   width stays what 16:9 shows and the extra height shows more above and below.
//
// Which scenes: those that set up a scene camera. Matches, entrances and cutscenes do it every
// frame; the menus and loading screens never do, so a frame is drawn in the screen's shape while a
// scene camera was set up within the last half second.

#include "widescreen.h"

#include <rex/cvar.h>
#include <rex/ppc.h>

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>

namespace svr {
namespace {

constexpr double kAspect16x9 = 16.0 / 9.0;
// The projection's angle scale (sub_8247CA20: lfs 0x82000D1C).
constexpr double kProjectionAngleScale = 0.5;
// How long after the last scene camera a frame still counts as the scene's.
constexpr int64_t kSceneHoldMs = 500;

std::atomic<int64_t> g_last_camera_ms{-1000000};

int64_t NowMs() {
  return std::chrono::duration_cast<std::chrono::milliseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

}  // namespace

double WideAspect() {
  // bd_aspect_ratio (gpu/settings.cpp): 2 = 16:10, 3 = 21:9, 4 = 32:9; the rest draw 16:9.
  switch (std::atoi(rex::cvar::GetFlagByName("bd_aspect_ratio").c_str())) {
  case 2:
    return 16.0 / 10.0;
  case 3:
    return 64.0 / 27.0;
  case 4:
    return 32.0 / 9.0;
  default:
    return 0.0;
  }
}

bool WideScene() {
  return WideAspect() != 0.0 &&
         NowMs() - g_last_camera_ms.load(std::memory_order_relaxed) < kSceneHoldMs;
}

float WideFactor() {
  const double aspect = WideScene() ? WideAspect() : 0.0;
  return aspect > kAspect16x9 ? float(aspect / kAspect16x9) : 1.0f;
}

float TallFactor() {
  const double aspect = WideScene() ? WideAspect() : 0.0;
  return aspect != 0.0 && aspect < kAspect16x9 ? float(kAspect16x9 / aspect) : 1.0f;
}

}  // namespace svr

// sub_8247D120 about to build the camera's projection (bl sub_8247CA20): f1 = field of view,
// f2 = aspect.
void SvrCameraAspectHook(PPCRegister& f1, PPCRegister& f2) {
  svr::g_last_camera_ms.store(svr::NowMs(), std::memory_order_relaxed);
  if (!svr::WideScene() || std::fabs(f2.f64 - svr::kAspect16x9) > 0.01)
    return;  // 16:9, or the console's 4:3 display mode
  const double wide = svr::WideFactor();
  const double tall = svr::TallFactor();
  if (wide > 1.0) {
    f2.f64 = svr::kAspect16x9 * wide;
  } else if (tall > 1.0) {
    f2.f64 = svr::kAspect16x9 / tall;
    const double k = svr::kProjectionAngleScale;
    f1.f64 = std::atan(std::tan(f1.f64 * k) * tall) / k;
  }
}
