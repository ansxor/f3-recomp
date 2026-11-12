// Boot predicate for builds whose game provides no games/<game>/boot/.
//
// Such games have no known boot-complete signal, so the frontend never turbo-boots
// them. The entry point must still resolve because runtime/frontend/frontend.cpp calls it.
#include "f3rt/game_boot.hpp"

namespace f3rt {

const GameBoot *game_boot() { return nullptr; }

} // namespace f3rt
