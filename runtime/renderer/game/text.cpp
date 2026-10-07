#include "renderer/game/text.hpp"
#include <cstring>

namespace f3rt {

void GameText::reset() {
    map_.fill(0);
    glyph_ram_.fill(0);
}

void GameText::decode(const VideoRam &vram) {
    for (unsigned i = 0; i < map_.size(); ++i)
        map_[i] = vram.u16(0x1c000 + i * 2);
    std::memcpy(glyph_ram_.data(), &vram.graphics[0x1e000], glyph_ram_.size());
}

} // namespace f3rt
