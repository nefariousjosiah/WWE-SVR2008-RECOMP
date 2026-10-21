// The white veil at the start of some entrances (GitHub issue #9).
//
// Entrance scripts can put timed full-screen colour overlays over the scene (update sub_8233DB60,
// draw sub_8233DD98). An overlay's script data (overlay +84) holds its blend mode (+0: 0 alpha,
// 1 additive, 3 multiply...), start cue and frame (+4, +8), end frame (+16), colour (+20..+28),
// intensity (+32) and, for script items newer than version 10 (item +4, the item at overlay +80),
// a fade curve (+36: 0 rise and fall, 1 fall and rise, 2 rise, 3 fall, 4 constant).
//
// Entrances that open with the stage lights at full strength (Cena's and Orton's among them)
// start with an additive white overlay at full intensity on the constant curve until frame 9999,
// so for the whole opening shot the game adds 50% white to every pixel before bloom: the white-out
// players see on PC, which console footage of the same entrances doesn't show. Overlays like that
// (additive, constant) are skipped; flashes that rise and fall still play.

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <atomic>
#include <cstdint>
#include <cstring>

REXCVAR_DEFINE_BOOL(svr_entrance_white_veil, false, "SvR",
                    "Draw the constant additive white overlay some entrance scripts open with (it "
                    "washes the whole picture out for the opening shot)");

namespace {
const uint8_t* Guest(uint32_t va) {
  auto* rt = rex::Runtime::instance();
  return rt && rt->memory() && va ? rt->memory()->TranslateVirtual<const uint8_t*>(va) : nullptr;
}

uint32_t GuestU32(uint32_t va) {
  const uint8_t* p = Guest(va);
  return p ? (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3] : 0;
}

float GuestF32(uint32_t va) {
  const uint32_t bits = GuestU32(va);
  float v;
  std::memcpy(&v, &bits, 4);
  return v;
}
}  // namespace

// sub_8233DD98 entry (r3 = the overlay); returning true skips drawing it this frame.
bool SvrEntranceVeilHook(PPCRegister& r3) {
  if (REXCVAR_GET(svr_entrance_white_veil))
    return false;
  const uint32_t data = GuestU32(r3.u32 + 84);
  const uint32_t item = GuestU32(r3.u32 + 80);
  if (!data || !item)
    return false;
  const int16_t version = int16_t(GuestU32(item + 4) >> 16);
  const bool veil = GuestF32(data + 0) == 1.0f && version > 10 && GuestF32(data + 36) == 4.0f;
  if (veil) {
    static std::atomic<uint32_t> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 3)
      REXLOG_INFO("[entrance] skipping the constant additive overlay 0x{:08X} (colour {} {} {}, "
                  "intensity {})",
                  r3.u32, GuestF32(data + 20), GuestF32(data + 24), GuestF32(data + 28),
                  GuestF32(data + 32));
  }
  return veil;
}
