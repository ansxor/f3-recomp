// Netplay versus hook for builds whose game provides no games/<game>/netplay/.
#include "f3rt/game_netplay.hpp"

namespace f3rt {

const GameNetplay *game_netplay() { return nullptr; }

} // namespace f3rt
