#pragma once
#include "renderer/game/scene.hpp"
#include <array>
#include <cstdint>
#include <span>

namespace f3rt {

// Text layer, snapshotted raw from FDP video RAM at VBSTART.
//
// Two regions are copied because emulation keeps writing VRAM while the frame
// is composited or materialized lazily:
//   m_map       4096 big-endian 16-bit words at graphics offset 0x1c000 (64x64
//              cells, 8x8 pixels each);
//   m_glyph_ram 0x2000 bytes at graphics offset 0x1e000 (256 glyphs x 32 bytes,
//              8x8 4bpp).
// pixel() decodes them on demand, bit-identical to the FDP renderer
// (runtime/renderer/fdp/video.cpp generate_text_line + decode_charram_tile).
class GameText {
public:
    GameText() { reset(); }
    void reset();
    // Copy the raw text map and glyph RAM from VRAM at VBSTART. Shared; defined
    // in runtime/renderer/game/text.cpp.
    void decode(const VideoRam &vram);

    // Raw text-map word bit layout (shared with the GPU decode in
    // runtime/renderer/shaders/scene_body.glsl):
    //   bits 0-7   glyph number (0..255)
    //   bit  8     flip X
    //   bits 9-14  palette code (palette base = code * 16)
    //   bit  15    flip Y
    // Raw glyph RAM: pixel (x, y) is nibble (x & 1) of byte y*4 + (3 - x/2),
    // i.e. decode_charram_tile() in runtime/renderer/decode.hpp.
    ScenePixel pixel(int x, int y, bool flipped) const {
        x &= 511;
        y &= 511;
        if (flipped) {
            x = 511 - x;
            y = 511 - y;
        }
        const uint16_t word = m_map[(y / 8) * 64 + x / 8];
        const unsigned tx = (x & 7) ^ ((word & 0x0100) ? 7 : 0);
        const unsigned ty = (y & 7) ^ ((word & 0x8000) ? 7 : 0);
        const uint8_t byte = m_glyph_ram[(word & 0xff) * 32 + ty * 4 + (3 - tx / 2)];
        const uint8_t pen = (byte >> ((tx & 1) * 4)) & 0x0f;
        return {uint16_t(((word >> 9) & 0x3f) * 16 + pen), uint8_t(pen ? 0x10 : 0)};
    }

    // Raw snapshot regions (see above), for snapshot consumers.
    std::span<const uint16_t, 4096> map() const { return m_map; }
    std::span<uint16_t, 4096> map() { return m_map; }
    std::span<const uint8_t, 0x2000> glyphs() const { return m_glyph_ram; }
    std::span<uint8_t, 0x2000> glyphs() { return m_glyph_ram; }

private:
    friend class GameVideo;
    std::array<uint16_t, 4096> m_map{};
    std::array<uint8_t, 0x2000> m_glyph_ram{};
};
static_assert(SceneSource<GameText>);

} // namespace f3rt
