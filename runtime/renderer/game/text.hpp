#pragma once
#include "renderer/game/ram_shadow.hpp"
#include "renderer/game/scene.hpp"
#include <array>
#include <cstdint>
#include <span>

namespace f3rt {

// Text layer, snapshotted raw from FDP video RAM at VBSTART.
//
// Three regions are copied because emulation keeps writing VRAM while the frame
// is composited or materialized lazily:
//   m_map       4096 big-endian 16-bit words at graphics offset 0x1c000 (64x64
//              cells, 8x8 pixels each);
//   m_glyph_ram 0x2000 bytes at graphics offset 0x1e000 (256 glyphs x 32 bytes,
//              8x8 4bpp);
//   m_pivot_ram 0x10000 bytes of column-major bitmap tiles (2048 x 32 bytes).
// pixel() decodes them on demand, bit-identical to the FDP renderer
// (runtime/renderer/fdp/video.cpp generate_text_line + decode_charram_tile).
class GameText {
public:
    GameText() { reset(); }
    void reset();
    // Copy the raw text map, glyph RAM and pivot RAM from VRAM at VBSTART.
    void decode(const VideoRam &vram);
    uint64_t version() const { return m_version; }

    // Raw text-map word bit layout (shared with the GPU decode in
    // runtime/renderer/shaders/scene_body.glsl):
    //   bits 0-7   glyph number (0..255)
    //   bit  8     flip X
    //   bits 9-14  palette code (palette base = code * 16)
    //   bit  15    flip Y
    // Raw glyph RAM: pixel (x, y) is nibble (x & 1) of byte y*4 + (3 - x/2),
    // i.e. decode_charram_tile() in runtime/renderer/decode.hpp.
    unsigned bitmap_map_row(unsigned y, bool flipped) const {
        const unsigned tile_row = (y & 255) / 8;
        // FDP uses the global text Y register to select the attribute bank.
        return tile_row + (((tile_row * 8 + m_scroll_y + (flipped ? 256 : 0)) & 256) ? 32 : 0);
    }

    ScenePixel pixel(int x, int y, bool flipped, bool bitmap = false) const {
        x &= 511;
        y &= bitmap ? 255 : 511;
        const unsigned bitmap_row = bitmap ? bitmap_map_row(unsigned(y), flipped) : 0;
        if (flipped) {
            x = 511 - x;
            y = (bitmap ? 255 : 511) - y;
        }
        const uint16_t word = m_map[(bitmap ? bitmap_row : unsigned(y / 8)) * 64 + x / 8];
        const unsigned tile = bitmap ? unsigned(x / 8 * 32 + y / 8) : word & 0xff;
        const uint8_t *ram = bitmap ? m_pivot_ram.data() : m_glyph_ram.data();
        const unsigned tx = (x & 7) ^ ((word & 0x0100) ? 7 : 0);
        const unsigned ty = (y & 7) ^ ((word & 0x8000) ? 7 : 0);
        const uint8_t byte = ram[tile * 32 + ty * 4 + (3 - tx / 2)];
        const uint8_t pen = (byte >> ((tx & 1) * 4)) & 0x0f;
        return {uint16_t(((word >> 9) & 0x3f) * 16 + pen), uint8_t(pen ? 0x10 : 0)};
    }

    // Hardware row-usage cull (FDP textram_row_usage): a text row whose 64 tile codes are
    // all zero is not drawn at all, even when attributes select visible pens.
    bool row_used(unsigned y) const {
        return (m_row_used >> ((y >> 3) & 63)) & 1;
    }

    // Raw snapshot regions (see above), for snapshot consumers.
    std::span<const uint16_t, 4096> map() const { return m_map; }
    std::span<uint16_t, 4096> map() {
        touch();
        return m_map;
    }
    std::span<const uint8_t, 0x2000> glyphs() const { return m_glyph_ram; }
    std::span<uint8_t, 0x2000> glyphs() {
        touch();
        return m_glyph_ram;
    }

    std::span<const uint8_t, 0x10000> pivot() const { return m_pivot_ram; }

private:
    std::array<uint16_t, 4096> m_map{};
    std::array<uint8_t, 0x2000> m_glyph_ram{};
    std::array<uint8_t, 0x10000> m_pivot_ram{};
    uint16_t m_scroll_y = 0;
    uint64_t m_row_used = 0; // bit r: text row r has a nonzero tile code
    RamShadow<sizeof(uint16_t) * 4096> m_shadow_map;
    RamShadow<0x2000> m_shadow_glyphs;
    RamShadow<0x10000> m_shadow_pivot;
    uint64_t m_version = fresh_content_version();
    void touch() {
        m_shadow_map.invalidate();
        m_shadow_glyphs.invalidate();
        m_shadow_pivot.invalidate();
        m_version = fresh_content_version();
    }
};
static_assert(SceneSource<GameText>);

} // namespace f3rt
