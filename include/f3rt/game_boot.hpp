#pragma once
#include <cstdint>
namespace f3rt {
class Machine;
struct GameBoot {
    // True at a frame boundary once power-on self-test and boot waits are over.
    bool (*complete)(const Machine &);
    // Fail-safe: turbo stops after this many frames even if complete never fires.
    uint32_t frame_limit;
};
const GameBoot *game_boot(); // nullptr when the game provides none
}
