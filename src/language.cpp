// Text languages. string.pac has every table in eight language slots (0, 1 English; 2 Italian;
// 4 German; 5 Spanish; 6 Japanese, partly), and the game maps the console's language to a slot,
// but this disc's build lists only English as available, so any other language fell back to it.
// Spanish, German and Italian are complete on the disc: when the game asks for one of them, add it
// to the list first. (There's no French text on this disc.)

#include <cstdint>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/types.h>

namespace {

uint32_t LoadBe32(const uint8_t* p) {
  return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
void StoreBe32(uint8_t* p, uint32_t v) {
  p[0] = uint8_t(v >> 24);
  p[1] = uint8_t(v >> 16);
  p[2] = uint8_t(v >> 8);
  p[3] = uint8_t(v);
}

constexpr uint32_t kItalian = 2, kGerman = 4, kSpanish = 5;  // the game's slot numbers
constexpr uint32_t kListCapacity = 8;  // slots at +0..+28, count at +32, chosen index at +36

}  // namespace

// Chooses the text language (r3: the language object, r4: the slot wanted); succeeds only for a
// slot in the object's list.
REX_EXTERN(__imp__sub_82443E48);
REX_HOOK_RAW(sub_82443E48) {
  const uint32_t want = ctx.r4.u32;
  if (ctx.r3.u32 && (want == kItalian || want == kGerman || want == kSpanish)) {
    uint8_t* object = base + ctx.r3.u32;
    const uint32_t count = LoadBe32(object + 32);
    bool listed = false;
    for (uint32_t i = 0; i < count && i < kListCapacity; ++i)
      listed |= LoadBe32(object + 4 * i) == want;
    if (!listed && count < kListCapacity) {
      StoreBe32(object + 4 * count, want);
      StoreBe32(object + 32, count + 1);
      REXLOG_INFO("Text language: slot {} made available", want);
    }
  }
  __imp__sub_82443E48(ctx, base);
}
