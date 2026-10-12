#include "renderer/game/text.hpp"
#include "f3rt/memory_map.hpp"
#include <cstring>

namespace f3rt {

void GameText::reset() {
    map_.fill(0);
    glyph_ram_.fill(0);
}

void GameText::decode(const VideoRam &vram) {
    for (unsigned i = 0; i < map_.size(); ++i)
        map_[i] = vram.u16(OFFS_TEXTRAM_MAP + i * 2);
    std::memcpy(glyph_ram_.data(), &vram.graphics[OFFS_TEXTRAM_GLYPHS], glyph_ram_.size());
}

} // namespace f3rt
