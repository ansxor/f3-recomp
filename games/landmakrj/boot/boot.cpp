// Land Maker (landmakrj) power-on boot-complete signal.
//
// After the RAM/VRAM self-test, the EEPROM signature check and both 180-frame
// `trap #4` boot waits, the boot task at $8dde8 executes `moveq #1,d0;
// movec d0,cacr`. The pre-watchdog error path ($10104/$1014c) instead writes
// cacr 0, so this flag is never set by a failed boot.
//
// Verified read-only from a throwaway harness (native main+sound, strict native):
//   blank EEPROM:        cacr == 0 on every boundary 1..780, first == 1 at frame 781
//   initialised EEPROM:  cacr == 0 on every boundary 1..477, first == 1 at frame 478
// No regression after the first 1. The predicate therefore fires exactly once, at
// the end of the WAIT A MOMENT phase, and always after attract begins.
#include "f3rt/game_boot.hpp"
#include "f3rt/machine.hpp"

namespace f3rt {
namespace {

bool boot_complete(const Machine &m) { return m.cpu.cacr == 1; }

const GameBoot boot{&boot_complete, 1200};

} // namespace

const GameBoot *game_boot() { return &boot; }

} // namespace f3rt
