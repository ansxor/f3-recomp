#include "game_text.hpp"

namespace f3rt {

void GameText::reset() {
    map_.fill(0);
    glyph_ram_.fill(0);
}

} // namespace f3rt
