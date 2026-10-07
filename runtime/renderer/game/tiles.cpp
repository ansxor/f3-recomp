#include "renderer/game/tiles.hpp"

namespace f3rt {

void GameTiles::reset() {
    for (auto &map : maps_) map.fill(0);
}

namespace {

constexpr uint32_t pf_begin = 0x10000; // PF0 data

} // namespace

void GameTiles::decode(const VideoRam &vram) {
    for (unsigned layer = 0; layer < maps_.size(); ++layer) {
        const uint32_t base = pf_begin + layer * 0x2000;
        for (unsigned i = 0; i < maps_[layer].size(); ++i) {
            const uint32_t at = base + i * 4;
            maps_[layer][i] = uint32_t(vram.u16(at)) << 16 | vram.u16(at + 2);
        }
    }
}

} // namespace f3rt
