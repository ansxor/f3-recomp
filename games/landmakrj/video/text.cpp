// Land Maker text-layer VRAM snapshot.
//
// The text tile map lives at 0x61c000 (4096 cells, one big-endian word each:
// tile 0..7, palette 9..14, flip_x bit 8, flip_y bit 15) and the 8x8 4bpp glyph
// RAM at 0x61e000 (256 tiles x 32 bytes). Both are copied verbatim here at
// VBSTART from the video RAM the FDP renderer reads; GameText::pixel() and the
// GPU shader decode the raw words.
#include "game_text.hpp"
#ifdef F3RT_VIDEO_WRITE_LOG
#include "game_video_log.hpp"
#endif

#include <cstring>

namespace f3rt {

void GameText::decode(const VideoRam &vram) {
    for (unsigned i = 0; i < map_.size(); ++i)
        map_[i] = vram.u16(0x1c000 + i * 2);
    std::memcpy(glyph_ram_.data(), &vram.graphics[0x1e000], glyph_ram_.size());
}

#ifdef F3RT_VIDEO_WRITE_LOG
void GameText::observe_write(uint32_t pc, uint32_t address, uint64_t frame) {
    if (address < 0x61c000 || address >= 0x620000) return;
    switch (pc) {
    case 0x570e: case 0x5712: case 0x5756: case 0x5758: case 0x578e:
    case 0x57bc: case 0x57be: case 0x580c: case 0x580e:
    case 0x583a: case 0x5840: case 0x5846: case 0x58c2:
    case 0x59d0: case 0x5a08: case 0x5bf8: case 0x5c20:
    case 0x5b8a: case 0x5b9a: case 0x5bb6: case 0x5bc6: case 0x5bd8:
    case 0x8de60: case 0x8e0b2: case 0x8e0c2: case 0x8e0e6: case 0x8e0f4:
    case 0x8e9d2: case 0x9b53a: case 0x9b562: case 0x9b564: case 0x9b586:
    case 0xa1176: case 0xa117c: case 0xa1184: case 0xa118c:
    case 0xa1194: case 0xa119c: case 0xa11a4: case 0xa11ac:
        return;
    default:
        log_unknown_video_write("text", pc, address, frame);
    }
}
#endif

} // namespace f3rt
