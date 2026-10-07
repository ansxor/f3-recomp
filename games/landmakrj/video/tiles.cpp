// Land Maker (landmakrj) playfield producer: raw PF0..PF3 snapshot of FDP
// video RAM. The four layers live at graphics offset 0x10000 + layer * 0x2000,
// 2048 cells of 4 bytes each. See runtime/game_tiles.hpp for the cell bit layout.
#include "game_tiles.hpp"
#ifdef F3RT_VIDEO_WRITE_LOG
#include "game_video_log.hpp"
#endif

namespace f3rt {
namespace {
constexpr uint32_t pf_begin = 0x10000;                   // PF0 data
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

#ifdef F3RT_VIDEO_WRITE_LOG
void GameTiles::observe_write(uint32_t pc, uint32_t address, uint64_t frame) {
    constexpr uint32_t graphics_base = 0x600000, pf_end = pf_begin + 4 * 0x2000;
    constexpr std::array<const char *, 4> layer_names{"pf0", "pf1", "pf2", "pf3"};
    if (address < graphics_base + pf_begin || address >= graphics_base + pf_end) return;
    // Land Maker's tile-block copy/erase loops and the per-layer clear stores.
    switch (pc) {
    case 0x55fc: case 0x5646: case 0x56d6:
    case 0x5a2e: case 0x5a6a: case 0x5aa6: case 0x5b00:
    case 0x9bd08: case 0x9bd0a: case 0x9bd0c: case 0x9bd0e:
    case 0x9ec66: case 0x9ec6a: case 0x9ec6e: case 0x9ec72:
        return;
    default:
        log_unknown_video_write(layer_names[(address - graphics_base - pf_begin) / 0x2000], pc, address, frame);
    }
}
#endif

} // namespace f3rt
