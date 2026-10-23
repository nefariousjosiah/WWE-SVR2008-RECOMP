// Unlock everything (settings menu > Unlock everything, cvar svr_unlock_all).
//
// SvR 2008 keeps what is unlocked (shop purchases and rewards: superstars, arenas, attires, moves
// and the rest) as a 96-bit field in the profile (profile +0x19D6C), and every menu asks one
// function whether item N is unlocked: sub_82579F80 (r3 = the item, its answer in r3). Unlocking
// an item (sub_82579FD8) only sets bits for items whose entry in the runtime item table at
// 0x82E78458 (12 bytes each, +4 non-zero) says they can be unlocked.
//
// With the setting on, the check answers "unlocked" for every such item; the hook writes nothing to
// the profile. But the game's reward code only unlocks an item when the check says it is locked, so
// rewards earned with the setting on aren't recorded (the settings menu warns about it).

#include <rex/cvar.h>
#include <rex/ppc.h>
#include <rex/runtime.h>
#include <rex/system/xmemory.h>

#include <cstdint>

REXCVAR_DEFINE_BOOL(svr_unlock_all, false, "SvR",
                    "Every superstar, arena, attire, move and other unlockable is available; the "
                    "save itself is not changed");

namespace {
constexpr uint32_t kItemTable = 0x82E78458;
constexpr uint32_t kItemCount = 96;  // the profile's bit field is 12 bytes

bool CanUnlock(uint32_t item) {
  if (item >= kItemCount) {
    return false;
  }
  auto* rt = rex::Runtime::instance();
  if (!rt || !rt->memory()) {
    return false;
  }
  const auto* entry = rt->memory()->TranslateVirtual<const uint8_t*>(kItemTable + item * 12);
  return entry && entry[4] != 0;
}
}  // namespace

// sub_82579F80 just computed its answer (r3 = 1 when unlocked; r31 = the item).
void SvrUnlockAllHook(PPCRegister& r3, PPCRegister& r31) {
  if (REXCVAR_GET(svr_unlock_all) && r3.u32 == 0 && CanUnlock(r31.u32)) {
    r3.u64 = 1;
  }
}
