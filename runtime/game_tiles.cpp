#include "game_tiles.hpp"

namespace f3rt {

void GameTiles::reset() {
    for (auto &map : maps_) map.fill(0);
}

// decode() is game-specific and lives in games/<id>/video/, linked per
// configured F3_GAME.

} // namespace f3rt
