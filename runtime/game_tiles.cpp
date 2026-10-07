#include "game_tiles.hpp"

namespace f3rt {

void GameTiles::reset() {
    for (auto &map : maps_) map.fill(0);
}

// decode() and observe_write() are game-specific and live in
// games/<id>/video/tiles.cpp, linked per configured F3_GAME.

} // namespace f3rt
