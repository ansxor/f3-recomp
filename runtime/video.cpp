// license:BSD-3-Clause
// copyright-holders:Bryan McPhail, ywy, 12Me21, f3rt authors
#include "f3rt/video.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <memory>
#include <span>
#include <vector>

namespace f3rt {

namespace {

constexpr int H_TOTAL = 432;
constexpr int H_VIS   = 320;
constexpr int H_START = 46;
constexpr int V_VIS   = 232;
constexpr int V_START = 24;

constexpr int NUM_PLAYFIELDS   = 4;
constexpr int NUM_SPRITEGROUPS = 4;
constexpr int NUM_CLIPPLANES   = 4;

constexpr uint32_t OFFS_SPRITERAM = 0x00000; // 0x10000 bytes
constexpr uint32_t OFFS_PF_RAM    = 0x10000; // 0x0c000 bytes
constexpr uint32_t OFFS_TEXTRAM   = 0x1c000; // 0x02000 bytes
constexpr uint32_t OFFS_CHARRAM   = 0x1e000; // 0x02000 bytes
constexpr uint32_t OFFS_LINERAM   = 0x20000; // 0x10000 bytes
constexpr uint32_t OFFS_PIVOT_RAM = 0x30000; // 0x10000 bytes
constexpr uint32_t GRAPHICS_RAM_SIZE = 0x40000;

inline uint16_t read_be16(const uint8_t *p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

inline uint32_t read_be32(const uint8_t *p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
}

inline int16_t sext12(uint16_t v) {
    return (v & 0x800) ? int16_t(v | 0xf000) : int16_t(v & 0xfff);
}

inline int mosaic(int x, uint8_t sample) {
    if (sample <= 1)
        return x;
    int x_count = x - 46 + 114;
    x_count = (x_count >= 432) ? (x_count - 432) : x_count;
    return x - (x_count % sample);
}

struct clip_plane_inf {
    int16_t l = 0;
    int16_t r = 0;

    clip_plane_inf() = default;
    clip_plane_inf(int16_t left, int16_t right) : l(left), r(right) {}

    clip_plane_inf &set_upper(int8_t left, int8_t right) {
        l = (l & 0xff) | (int16_t(left) << 8);
        r = (r & 0xff) | (int16_t(right) << 8);
        return *this;
    }
    clip_plane_inf &set_lower(uint8_t left, uint8_t right) {
        l = (l & 0x100) | left;
        r = (r & 0x100) | right;
        return *this;
    }
};

struct pri_mode {
    uint8_t src_prio[H_TOTAL]{};
    uint8_t dst_prio[H_TOTAL]{};
    uint8_t src_blendmode[H_TOTAL]{};
    uint8_t dst_blendmode[H_TOTAL]{};
};

struct mix_pix {
    uint16_t src_pal[H_TOTAL]{};
    uint16_t dst_pal[H_TOTAL]{};
    uint8_t  src_blend[H_TOTAL]{};
    uint8_t  dst_blend[H_TOTAL]{};
};

struct mixable {
    bool x_sample_enable = false;
    uint16_t mix_value = 0;
    uint8_t prio = 0;
    uint8_t blend_mode = 0;
    uint8_t index = 0;

    void set_mix(uint16_t v) {
        mix_value = v;
        prio = v & 0x0f;
        blend_mode = (v >> 14) & 3;
    }
    void set_prio(uint8_t p) {
        mix_value = (mix_value & 0xfff0) | (p & 0x0f);
        prio = p & 0x0f;
    }
    void set_blend(uint8_t b) {
        mix_value = (mix_value & 0x3fff) | (uint16_t(b & 3) << 14);
        blend_mode = b & 3;
    }

    uint8_t clip_inv() const { return (mix_value >> 4) & 0x0f; }
    uint8_t clip_enable() const { return (mix_value >> 8) & 0x0f; }
    bool clip_inv_mode() const { return (mix_value & 0x1000) != 0; }
    int x_index(int x) const { return x; }
    uint16_t palette_adjust(uint16_t pal) const { return pal; }
    virtual bool layer_enable() const {
        return (mix_value & 0x2000) != 0 && blend_mode != 0b11;
    }
    virtual ~mixable() = default;
};

struct sprite_inf : public mixable {
    bool blend_select_v = false;

    bool layer_enable() const override {
        return (mix_value & 0x2000) != 0 && blend_mode != 0b00;
    }
    bool inactive_group(uint16_t color) const {
        return ((color >> 10) & 3) != index;
    }
    bool blend_select(const uint8_t * /*flags*/, int /*x*/) const {
        return blend_select_v;
    }
};

struct pivot_inf : public mixable {
    uint8_t pivot_control = 0;
    bool blend_select_v = false;
    uint16_t pivot_enable = 0;
    int16_t reg_sx = 0;
    int16_t reg_sy = 0;

    bool blend_select(const uint8_t * /*flags*/, int /*x*/) const {
        return blend_select_v;
    }
    bool use_pix() const {
        return (pivot_control & 0xa0) != 0;
    }
    int x_index(int x) const {
        return (x + reg_sx) & 0x1ff;
    }
    int y_index(int y) const {
        return (reg_sy + y) & (use_pix() ? 0xff : 0x1ff);
    }
};

struct playfield_inf : public mixable {
    uint16_t colscroll = 0;
    bool alt_tilemap = false;
    int32_t x_scale = 0x80;
    int32_t y_scale = 0;
    uint16_t pal_add = 0;
    int32_t rowscroll = 0;

    int32_t reg_sx = 0;
    int32_t reg_sy = 0;
    int32_t reg_fx_y = 0;
    int32_t reg_fx_x = 0;

    uint16_t width_mask = 0x3ff;

    uint16_t palette_adjust(uint16_t pal) const {
        return pal + pal_add;
    }
    int x_index(int x) const {
        return (((reg_fx_x + (x - H_START) * x_scale) >> 8) + H_START) & width_mask;
    }
    int y_index(int /*y*/) const {
        return ((reg_fx_y >> 8) + colscroll) & 0x1ff;
    }
    bool blend_select(const uint8_t *line_flags, int x) const {
        return (line_flags[x] & 1) != 0;
    }
};

struct f3_line_inf {
    int y = 0;
    clip_plane_inf clip[NUM_CLIPPLANES]{};
    uint8_t blend[4]{};
    uint8_t x_sample = 16;
    uint8_t fx_6400 = 0;
    uint16_t bg_palette = 0;
    pivot_inf pivot;
    sprite_inf sp[NUM_SPRITEGROUPS];
    playfield_inf pf[NUM_PLAYFIELDS];
};

struct tempsprite {
    int code = 0;
    uint8_t color = 0;
    bool flip_x = false;
    bool flip_y = false;
    int32_t x = 0;
    int32_t y = 0;
    int32_t scale_x = 0;
    int32_t scale_y = 0;
    uint8_t pri = 0;
};

struct clip_ranges {
    // Each of four planes can at most split every preceding interval in two.
    std::array<clip_plane_inf, 1 << NUM_CLIPPLANES> values;
    size_t count = 0;
    const clip_plane_inf *begin() const { return values.data(); }
    const clip_plane_inf *end() const { return values.data() + count; }
};

clip_ranges calc_clip(const clip_plane_inf clip[NUM_CLIPPLANES], const mixable &layer) {
    constexpr int16_t INF_L = H_START;
    constexpr int16_t INF_R = H_START + H_VIS;
    uint8_t normal_planes = layer.clip_enable() & ~layer.clip_inv();
    uint8_t invert_planes = layer.clip_enable() & layer.clip_inv();
    if (!layer.clip_inv_mode()) std::swap(normal_planes, invert_planes);

    clip_ranges ranges;
    ranges.values[ranges.count++] = {INF_L, INF_R};
    for (int plane = 0; plane < NUM_CLIPPLANES; ++plane) {
        const int16_t clip_l = clip[plane].l - 1;
        const int16_t clip_r = clip[plane].r - 2;
        if (normal_planes & (1 << plane)) {
            size_t kept = 0;
            for (auto range : ranges) {
                if (clip_l <= clip_r && range.r >= clip_l && range.l <= clip_r)
                    ranges.values[kept++] = {std::max(range.l, clip_l), std::min(range.r, clip_r)};
            }
            ranges.count = kept;
        } else if ((invert_planes & (1 << plane)) && clip_l <= clip_r) {
            clip_ranges next;
            for (size_t i = 0; i < 2 * ranges.count; ++i) {
                clip_plane_inf candidate = i < ranges.count
                    ? clip_plane_inf{INF_L, clip_l} : clip_plane_inf{clip_r, INF_R};
                bool keep = true;
                for (const auto &range : ranges) {
                    // Preserve the observed baseline's inverted-plane combining
                    // rule; the hardware notes still investigate this case.
                    candidate.l = std::max(range.l, candidate.l);
                    candidate.r = std::max(range.l, candidate.r);
                    if (candidate.l >= candidate.r) { keep = false; break; }
                }
                if (keep) next.values[next.count++] = candidate;
            }
            std::copy_n(next.values.begin(), next.count, ranges.values.begin());
            ranges.count = next.count;
        }
    }
    return ranges;
}

} // namespace

struct Video::Impl {
    std::vector<uint8_t> decoded_sprites; // 32768 * 256 bytes
    std::vector<uint8_t> decoded_tiles;   // 32768 * 256 bytes
    std::array<uint8_t, 256 * 64> decoded_chars{};   // 256 tiles x 64 pixels (4bpp)
    std::array<uint8_t, 2048 * 64> decoded_pivot{}; // 2048 tiles x 64 pixels (4bpp)

    std::array<tempsprite, 1024> spritelist{};
    size_t sprite_count = 0;
    std::array<uint16_t, 432 * 256> sprite_framebuffer{};
    std::array<uint8_t, 256> sprite_pri_row_usage{};
    std::array<uint8_t, 0x10000> buffered_spriteram{};
    bool has_buffered_spriteram = false;

    bool flipscreen = false;
    bool sprite_bank = false;
    bool sprite_trails = false;
    uint8_t sprite_extra_planes = 0;
    uint8_t sprite_pen_mask = 0x0f;

    uint16_t control_0[8]{};
    uint16_t control_1[8]{};
    uint8_t tilemap_row_usage[32][8]{};
    uint8_t textram_row_usage[64]{};

    // Pre-allocated line buffers for fast scanline rendering
    struct LineBuffer {
        int last_y = -1;
        std::array<uint16_t, 1024> pix{};
        std::array<uint8_t, 1024> flags{};
    };
    std::array<LineBuffer, NUM_PLAYFIELDS> pf_lines{};
    LineBuffer text_line{};
    LineBuffer pivot_line{};

    void reset() {
        sprite_count = 0;
        sprite_framebuffer.fill(0);
        sprite_pri_row_usage.fill(0);
        buffered_spriteram.fill(0);
        has_buffered_spriteram = false;
        flipscreen = false;
        sprite_bank = false;
        sprite_trails = false;
        sprite_extra_planes = 0;
        sprite_pen_mask = 0x0f;
        std::memset(control_0, 0, sizeof(control_0));
        std::memset(control_1, 0, sizeof(control_1));
        std::memset(tilemap_row_usage, 0, sizeof(tilemap_row_usage));
        std::memset(textram_row_usage, 0, sizeof(textram_row_usage));
        for (auto &pf : pf_lines) pf.last_y = -1;
        text_line.last_y = -1;
        pivot_line.last_y = -1;
    }

    bool decode_roms(std::span<const uint8_t> sprites,
                     std::span<const uint8_t> sprites_hi,
                     std::span<const uint8_t> tilemap,
                     std::span<const uint8_t> tilemap_hi) {
        if (sprites.size() < 0x400000 || sprites_hi.size() < 0x200000 ||
            tilemap.size() < 0x400000 || tilemap_hi.size() < 0x200000) {
            return false;
        }

        constexpr size_t TOTAL_TILES = 32768;
        decoded_sprites.resize(TOTAL_TILES * 256);
        decoded_tiles.resize(TOTAL_TILES * 256);

        // Decode 16x16 6bpp sprites: low 4 bpp packed LSB, high 2 bpp
        for (size_t c = 0; c < TOTAL_TILES; ++c) {
            const uint8_t *lo_tile = &sprites[c * 128];
            const uint8_t *hi_tile = &sprites_hi[c * 64];
            uint8_t *dest = &decoded_sprites[c * 256];

            for (int y = 0; y < 16; ++y) {
                const uint8_t *lo_row = &lo_tile[y * 8];
                const uint8_t *hi_row = &hi_tile[y * 4];
                uint8_t *dest_row = &dest[y * 16];

                for (int x = 0; x < 16; ++x) {
                    uint8_t lo = (x & 1) ? (lo_row[x >> 1] >> 4) : (lo_row[x >> 1] & 0x0f);
                    uint8_t hi = ((hi_row[x >> 2] >> (2 * (x & 3))) & 0x03) << 4;
                    dest_row[x] = lo | hi;
                }
            }
        }

        // Decode 16x16 6bpp tilemaps: low 4 bpp packed LSB, high 2 bpp
        for (size_t c = 0; c < TOTAL_TILES; ++c) {
            const uint8_t *lo_tile = &tilemap[c * 128];
            const uint8_t *hi_tile = &tilemap_hi[c * 64];
            uint8_t *dest = &decoded_tiles[c * 256];

            for (int y = 0; y < 16; ++y) {
                const uint8_t *lo_row = &lo_tile[y * 8];
                const uint8_t *hi_row = &hi_tile[y * 4];
                uint8_t *dest_row = &dest[y * 16];

                for (int x = 0; x < 16; ++x) {
                    uint8_t lo = (x & 1) ? (lo_row[x >> 1] >> 4) : (lo_row[x >> 1] & 0x0f);
                    uint8_t hi;
                    if (x < 8) {
                        hi = (((hi_row[1] >> x) & 1) << 5) | (((hi_row[0] >> x) & 1) << 4);
                    } else {
                        hi = (((hi_row[3] >> (x - 8)) & 1) << 5) | (((hi_row[2] >> (x - 8)) & 1) << 4);
                    }
                    dest_row[x] = lo | hi;
                }
            }
        }

        return true;
    }

    void decode_charram(const uint8_t *charram) {
        for (int c = 0; c < 256; ++c) {
            const uint8_t *tile_src = &charram[c * 32];
            uint8_t *dest = &decoded_chars[c * 64];
            for (int y = 0; y < 8; ++y) {
                const uint8_t *row = &tile_src[y * 4];
                for (int x = 0; x < 8; ++x)
                    dest[y * 8 + x] = (row[3 - x / 2] >> ((x & 1) * 4)) & 0x0f;
            }
        }
    }

    void decode_pivot_ram(const uint8_t *pivot_ram) {
        for (int c = 0; c < 2048; ++c) {
            const uint8_t *tile_src = &pivot_ram[c * 32];
            uint8_t *dest = &decoded_pivot[c * 64];
            for (int y = 0; y < 8; ++y) {
                const uint8_t *row = &tile_src[y * 4];
                for (int x = 0; x < 8; ++x)
                    dest[y * 8 + x] = (row[3 - x / 2] >> ((x & 1) * 4)) & 0x0f;
            }
        }
    }

    void update_row_usages(const uint8_t *pf_ram, const uint8_t *textram) {
        std::memset(tilemap_row_usage, 0, sizeof(tilemap_row_usage));
        std::memset(textram_row_usage, 0, sizeof(textram_row_usage));

        // Extend mode: 4 playfields of 64x32 tiles (2 words per tile)
        for (int offset = 1; offset < 0x4000; offset += 2) {
            uint16_t tile = read_be16(&pf_ram[offset * 2]);
            if (tile != 0) {
                int row  = (offset >> 7) & 0x1f;
                int tmap = offset >> 12;
                if (tmap < 8)
                    tilemap_row_usage[row][tmap]++;
            }
        }

        // Textram: 64x64 tiles (1 word per tile)
        for (int offset = 0; offset < 0x1000; ++offset) {
            uint16_t vram_tile = read_be16(&textram[offset * 2]);
            if ((vram_tile & 0xff) != 0) {
                int row = (offset >> 6) & 0x3f;
                textram_row_usage[row]++;
            }
        }
    }

    void get_sprite_info(const uint8_t *spriteram_base) {
        struct sprite_axis {
            int32_t block_scale = 1 << 8;
            int32_t pos = 0, block_pos = 0;
            int16_t global = 0, subglobal = 0;

            void update(uint8_t scroll, uint16_t posw, bool multi, uint8_t block_ctrl, uint8_t new_zoom) {
                int16_t new_pos = sext12(posw);
                if (scroll & 0x01) subglobal = new_pos;
                if (scroll & 0x02) global = new_pos;
                if (!(scroll & 0x08)) {
                    new_pos += global;
                    if (!(scroll & 0x04))
                        new_pos += subglobal;
                }

                switch (block_ctrl) {
                case 0b00:
                    if (!multi) {
                        block_pos = int32_t(new_pos) << 8;
                        block_scale = 0x100 - new_zoom;
                    }
                    [[fallthrough]];
                case 0b10:
                    pos = block_pos;
                    break;
                case 0b11:
                    pos += block_scale * 16;
                    break;
                }
            }
        };

        sprite_axis x, y;
        uint8_t color = 0;
        bool multi = false;
        sprite_count = 0;

        int total_sprites = 0;
        for (int offs = 0; offs < 0x400 && total_sprites < 0x400; ++offs) {
            total_sprites++;
            const uint32_t bank_offset = sprite_bank ? 0x8000 : 0x0000;
            const uint8_t *spr = &spriteram_base[bank_offset + offs * 16];

            uint16_t w0 = read_be16(&spr[0]);
            uint16_t w1 = read_be16(&spr[2]);
            uint16_t w2 = read_be16(&spr[4]);
            uint16_t w3 = read_be16(&spr[6]);
            uint16_t w4 = read_be16(&spr[8]);
            uint16_t w5 = read_be16(&spr[10]);
            uint16_t w6 = read_be16(&spr[12]);

            // Special command bit in word 3
            if (w3 & 0x8000) {
                flipscreen = (w5 & 0x2000) != 0;
                sprite_extra_planes = (w5 >> 8) & 3;
                sprite_pen_mask = (sprite_extra_planes << 4) | 0x0f;
                sprite_trails = (w5 & 0x0002) != 0;
                sprite_bank = (w5 & 0x0001) != 0;
            }

            // Sprite list jump bit in word 6
            if (w6 & 0x8000) {
                int new_offs = w6 & 0x03ff;
                if (new_offs == offs)
                    break;
                offs = new_offs - 1;
            }

            uint8_t spritecont = w4 >> 8;
            bool lock = (spritecont & 0x04) != 0;
            if (!lock)
                color = w4 & 0xff;

            uint8_t scroll_mode = (w2 >> 12) & 0x0f;
            x.update(scroll_mode, w2 & 0x0fff, multi, (spritecont >> 6) & 3, w1 & 0xff);
            y.update(scroll_mode, w3 & 0x0fff, multi, (spritecont >> 4) & 3, w1 >> 8);
            multi = (spritecont & 0x08) != 0;

            int tile = w0 | ((w5 & 0x0001) << 16);
            if (!tile)
                continue;

            int32_t tx = flipscreen ? ((512 << 8) - x.block_scale * 16 - x.pos) : x.pos;
            int32_t ty = flipscreen ? ((256 << 8) - y.block_scale * 16 - y.pos) : y.pos;

            // Visibility culling: screen active rect is [46..365] x [24..255]
            if (tx + x.block_scale * 16 <= (46 << 8) || tx > (365 << 8) ||
                ty + y.block_scale * 16 <= (24 << 8) || ty > (255 << 8))
                continue;

            bool flip_x = (spritecont & 0x01) != 0;
            bool flip_y = (spritecont & 0x02) != 0;

            if (sprite_count < spritelist.size()) {
                auto &s = spritelist[sprite_count++];
                s.x = tx;
                s.y = ty;
                s.flip_x = flipscreen ? !flip_x : flip_x;
                s.flip_y = flipscreen ? !flip_y : flip_y;
                s.code = tile;
                s.color = color;
                s.scale_x = x.block_scale;
                s.scale_y = y.block_scale;
                s.pri = (color >> 6) & 3;
            }
        }
    }

    void draw_gfx_sprite(const tempsprite &sprite) {
        if (decoded_sprites.empty())
            return;

        const uint8_t *code_base = &decoded_sprites[(sprite.code % 32768) * 256];
        const uint8_t flipx = sprite.flip_x ? 0x0f : 0;
        const uint8_t flipy = sprite.flip_y ? 0x0f : 0;

        int32_t dy8 = sprite.y;
        if (!flipscreen)
            dy8 += 255;

        for (uint8_t y = 0; y < 16; ++y) {
            const int dy = dy8 >> 8;
            dy8 += sprite.scale_y;
            if (dy < V_START || dy >= (V_START + V_VIS))
                continue;

            uint16_t *dest = &sprite_framebuffer[dy * H_TOTAL];
            uint8_t &usage = sprite_pri_row_usage[dy];
            const uint8_t *src = &code_base[(y ^ flipy) * 16];

            int32_t dx8 = sprite.x + 128;
            for (uint8_t x = 0; x < 16; ++x) {
                const int dx = dx8 >> 8;
                dx8 += sprite.scale_x;
                if (dx < H_START || dx >= (H_START + H_VIS))
                    continue;
                if (dx == (dx8 >> 8))
                    continue; // Skip double sampling in zoom

                const uint8_t c = src[x ^ flipx] & sprite_pen_mask;
                if (c && !dest[dx]) {
                    dest[dx] = 0x1000 + (uint16_t(sprite.color) << 4 | c);
                    usage |= 1 << sprite.pri;
                }
            }
        }
    }

    void draw_sprites() {
        if (!sprite_trails) {
            sprite_pri_row_usage.fill(0);
            sprite_framebuffer.fill(0);
        }

        // Draw in reverse order so earlier sprites have priority over later sprites
        for (size_t i = sprite_count; i > 0; --i) {
            draw_gfx_sprite(spritelist[i - 1]);
        }
    }

    void read_line_ram(f3_line_inf &line, int y, const uint8_t *lineram) {
        auto latched_addr = [lineram, y](uint8_t section, uint8_t subsection) -> uint32_t {
            uint16_t latches = read_be16(&lineram[section * 0x200 + y * 2]);
            uint32_t base = 0x4000 + 0x1000 * section + 0x200 * subsection;
            if (latches & (1 << (subsection + 4)))
                return base + 0x800 + y * 2;
            else if (latches & (1 << subsection))
                return base + y * 2;
            return 0;
        };

        // 4000 (Column scroll & clip plane high bits for PF 2 and 3)
        for (int i : { 2, 3 }) {
            if (uint32_t where = latched_addr(0, i)) {
                uint16_t colscroll = read_be16(&lineram[where]);
                line.pf[i].colscroll = colscroll & 0x1ff;
                line.pf[i].alt_tilemap = false; // extend mode always false
                line.clip[2 * (i - 2) + 0].set_upper((colscroll >> 12) & 1, (colscroll >> 13) & 1);
                line.clip[2 * (i - 2) + 1].set_upper((colscroll >> 14) & 1, (colscroll >> 15) & 1);
            }
        }

        // 5000 (Clip planes low 8 bits)
        for (int i = 0; i < 4; ++i) {
            if (uint32_t where = latched_addr(1, i)) {
                uint16_t clip_lows = read_be16(&lineram[where]);
                line.clip[i].set_lower(clip_lows & 0xff, (clip_lows >> 8) & 0xff);
            }
        }

        // 6000 (Sprite blend modes, pivot control, alpha values, mosaic, bg palette)
        if (uint32_t where = latched_addr(2, 0)) {
            uint16_t line_6000 = read_be16(&lineram[where]);
            line.pivot.blend_select_v = (line_6000 & 0x0200) != 0;
            line.pivot.pivot_control = (line_6000 >> 8) & 0xff;
            for (int sp_group = 0; sp_group < 4; ++sp_group) {
                line.sp[sp_group].set_blend((line_6000 >> (sp_group * 2)) & 3);
            }
        }
        if (uint32_t where = latched_addr(2, 1)) {
            uint16_t blend_vals = read_be16(&lineram[where]);
            for (int idx = 0; idx < 4; ++idx) {
                uint8_t alpha = (blend_vals >> (4 * idx)) & 0x0f;
                line.blend[idx] = std::min<uint8_t>(8, 0x0f - alpha);
            }
        }
        if (uint32_t where = latched_addr(2, 2)) {
            uint16_t x_mosaic = read_be16(&lineram[where]);
            line.x_sample = 16 - ((x_mosaic >> 4) & 0x0f);
            for (int pf_num = 0; pf_num < 4; ++pf_num) {
                line.pf[pf_num].x_sample_enable = (x_mosaic & (1 << pf_num)) != 0;
            }
            for (auto &sp : line.sp) {
                sp.x_sample_enable = (x_mosaic & 0x100) != 0;
            }
            line.pivot.x_sample_enable = (x_mosaic & 0x200) != 0;
            line.fx_6400 = (x_mosaic >> 8) & 0xfc;
        }
        if (uint32_t where = latched_addr(2, 3)) {
            line.bg_palette = read_be16(&lineram[where]);
        }

        // 7000 (Pivot & sprite mixing / priority)
        if (uint32_t where = latched_addr(3, 0)) {
            line.pivot.pivot_enable = read_be16(&lineram[where]);
        }
        if (uint32_t where = latched_addr(3, 1)) {
            line.pivot.set_mix(read_be16(&lineram[where]));
        }
        if (uint32_t where = latched_addr(3, 2)) {
            uint16_t sprite_mix = read_be16(&lineram[where]);
            for (int group = 0; group < 4; ++group) {
                line.sp[group].set_mix((line.sp[group].mix_value & 0xc00f)
                                       | ((sprite_mix & 0x3ff) << 4));
                line.sp[group].blend_select_v = ((sprite_mix >> (12 + group)) & 1) != 0;
            }
        }
        if (uint32_t where = latched_addr(3, 3)) {
            uint16_t sprite_prio = read_be16(&lineram[where]);
            for (int group = 0; group < 4; ++group) {
                line.sp[group].set_prio((sprite_prio >> (group * 4)) & 0x0f);
            }
        }

        // 8000 (Playfield zoom)
        for (int i = 0; i < 4; ++i) {
            if (uint32_t where = latched_addr(4, i)) {
                uint16_t pf_scale = read_be16(&lineram[where]);
                const int FIX_Y[] = { 0, 3, 2, 1 };
                line.pf[i].x_scale = 256 - ((pf_scale >> 8) & 0xff);
                line.pf[FIX_Y[i]].y_scale = (pf_scale & 0xff) << 1;
            }
        }

        // 9000 (Playfield palette add)
        for (int i = 0; i < 4; ++i) {
            if (uint32_t where = latched_addr(5, i)) {
                uint16_t pf_pal_add = read_be16(&lineram[where]);
                line.pf[i].pal_add = pf_pal_add * 16;
            }
        }

        // A000 (Playfield rowscroll)
        for (int i = 0; i < 4; ++i) {
            if (uint32_t where = latched_addr(6, i)) {
                int32_t rowscroll = int32_t(read_be16(&lineram[where])) << (8 - 6);
                line.pf[i].rowscroll = (rowscroll & 0xffffff00) - (rowscroll & 0x000000ff);
            }
        }

        // B000 (Playfield mixing info)
        for (int i = 0; i < 4; ++i) {
            if (uint32_t where = latched_addr(7, i)) {
                line.pf[i].set_mix(read_be16(&lineram[where]));
            }
        }
    }

    void get_pf_scroll(int pf_num, int32_t &reg_sx, int32_t &reg_sy) {
        int16_t sx_raw = int16_t(control_0[pf_num]);
        int16_t sy_raw = int16_t(control_0[pf_num + 4]);

        sy_raw += (1 << 7); // 9.7 fixed point

        if (flipscreen) {
            sx_raw += (320 << 6);
            sx_raw += ((512 + 192) << 6);
            sy_raw = -sy_raw;
        }

        sx_raw += ((40 - 4 * pf_num) << 6);

        int32_t sx = int32_t(sx_raw) << (8 - 6);
        int32_t sy = int32_t(sy_raw) << (8 - 7);
        sx ^= 0b1111'1100;
        if (flipscreen) {
            sx = sx - (H_START << 8);
            sy = -sy;
        } else {
            sx = sx - (H_START << 8);
        }

        reg_sx = sx;
        reg_sy = sy;
    }

    void generate_playfield_line(int pf_num, int y, const uint8_t *pf_ram) {
        auto &cache = pf_lines[pf_num];
        if (cache.last_y == y)
            return;
        cache.last_y = y;

        const uint32_t layer_base = pf_num * 0x2000;
        const int tile_row = (y / 16) & 31;
        const int in_y = y % 16;

        for (int tile_col = 0; tile_col < 64; ++tile_col) {
            int effective_col = flipscreen ? (63 - tile_col) : tile_col;
            int effective_row = flipscreen ? (31 - tile_row) : tile_row;
            uint32_t tile_idx = effective_row * 64 + effective_col;

            uint16_t attr = read_be16(&pf_ram[layer_base + tile_idx * 4]);
            uint16_t code = read_be16(&pf_ram[layer_base + tile_idx * 4 + 2]);

            uint16_t palette_code = attr & 0x1ff;
            uint8_t blend_sel     = (attr >> 9) & 1;
            uint8_t extra_planes  = (attr >> 10) & 3;
            bool flip_x = (attr & 0x4000) != 0;
            bool flip_y = (attr & 0x8000) != 0;
            if (flipscreen) {
                flip_x = !flip_x;
                flip_y = !flip_y;
            }

            uint8_t pen_mask = ((extra_planes & ~palette_code) << 4) | 0x0f;
            uint16_t palette_base = palette_code * 16;

            const uint8_t *tile_gfx = &decoded_tiles[(code % 32768) * 256];
            int src_y = flip_y ? (15 - in_y) : in_y;

            for (int in_x = 0; in_x < 16; ++in_x) {
                int src_x = flip_x ? (15 - in_x) : in_x;
                uint8_t raw_pen = tile_gfx[src_y * 16 + src_x];
                uint8_t pen = raw_pen & pen_mask;

                cache.pix[tile_col * 16 + in_x] = palette_base + pen;
                cache.flags[tile_col * 16 + in_x] = (pen != 0 ? 0x10 : 0x00) | blend_sel;
            }
        }
    }

    void generate_text_line(int y, const uint8_t *textram) {
        if (text_line.last_y == y)
            return;
        text_line.last_y = y;

        const int tile_row = (y / 8) & 63;
        const int in_y = y % 8;
        int effective_row = flipscreen ? (63 - tile_row) : tile_row;

        for (int tile_col = 0; tile_col < 64; ++tile_col) {
            int effective_col = flipscreen ? (63 - tile_col) : tile_col;
            uint32_t tile_idx = effective_row * 64 + effective_col;

            uint16_t vram_tile = read_be16(&textram[tile_idx * 2]);
            uint8_t tile_code = vram_tile & 0xff;
            uint8_t palette = (vram_tile >> 9) & 0x3f;
            bool flip_x = (vram_tile & 0x0100) != 0;
            bool flip_y = (vram_tile & 0x8000) != 0;
            if (flipscreen) {
                flip_x = !flip_x;
                flip_y = !flip_y;
            }

            const uint8_t *tile_gfx = &decoded_chars[tile_code * 64];
            int src_y = flip_y ? (7 - in_y) : in_y;

            for (int in_x = 0; in_x < 8; ++in_x) {
                int src_x = flip_x ? (7 - in_x) : in_x;
                uint8_t pen = tile_gfx[src_y * 8 + src_x];

                text_line.pix[tile_col * 8 + in_x] = (palette * 16) + pen;
                text_line.flags[tile_col * 8 + in_x] = (pen != 0) ? 0x10 : 0x00;
            }
        }
    }

    void generate_pixel_line(int y, const uint8_t *textram) {
        if (pivot_line.last_y == y)
            return;
        pivot_line.last_y = y;


        const int tile_row = (y / 8) & 31;
        const int in_y = y % 8;

        int y_offs = tile_row * 8 + control_1[5];
        if (flipscreen)
            y_offs += 0x100;
        int text_row = tile_row;
        if ((y_offs & 0x1ff) >= 256)
            text_row += 32;

        for (int tile_col = 0; tile_col < 64; ++tile_col) {
            int effective_col = flipscreen ? (63 - tile_col) : tile_col;
            int effective_row = flipscreen ? (31 - tile_row) : tile_row;
            uint32_t tile = effective_col * 32 + effective_row;

            int text_col = flipscreen ? (63 - tile_col) : tile_col;
            uint16_t vram_tile = read_be16(&textram[(text_row * 64 + text_col) * 2]);
            uint8_t palette = (vram_tile >> 9) & 0x3f;
            bool flip_x = (vram_tile & 0x0100) != 0;
            bool flip_y = (vram_tile & 0x8000) != 0;
            if (flipscreen) {
                flip_x = !flip_x;
                flip_y = !flip_y;
            }

            const uint8_t *tile_gfx = &decoded_pivot[tile * 64];
            int src_y = flip_y ? (7 - in_y) : in_y;

            for (int in_x = 0; in_x < 8; ++in_x) {
                int src_x = flip_x ? (7 - in_x) : in_x;
                uint8_t pen = tile_gfx[src_y * 8 + src_x];

                pivot_line.pix[tile_col * 8 + in_x] = (palette * 16) + pen;
                pivot_line.flags[tile_col * 8 + in_x] = (pen != 0) ? 0x10 : 0x00;
            }
        }
    }

    bool is_used(const pivot_inf &layer, int y) const {
        const int y_adj = flipscreen ? 0x1ff - layer.y_index(y) : layer.y_index(y);
        return layer.use_pix() || (textram_row_usage[y_adj >> 3] > 0);
    }
    bool is_used(const sprite_inf &layer, int y) const {
        return (sprite_pri_row_usage[y] & (1 << layer.index)) != 0;
    }
    bool is_used(const playfield_inf &layer, int y) const {
        const int y_adj = flipscreen ? 0x1ff - layer.y_index(y) : layer.y_index(y);
        return tilemap_row_usage[y_adj >> 4][layer.index] > 0;
    }

    template<typename LayerType>
    void mix_line_layer(const LayerType &gfx,
                        mix_pix &z,
                        pri_mode &pri,
                        const f3_line_inf &line,
                        const clip_plane_inf &range,
                        const uint16_t *src,
                        const uint8_t *flags) {
        const int left = std::max<int>(range.l, H_START);
        const int right = std::min<int>(range.r, H_START + H_VIS);
        for (int x = left; x < right; ++x) {
            if (gfx.blend_mode == pri.src_blendmode[x])
                continue;

            const int real_x = gfx.x_sample_enable ? mosaic(x, line.x_sample) : x;
            const int gfx_x = gfx.x_index(real_x);

            const uint16_t color = src[gfx_x];
            if constexpr (std::is_same_v<LayerType, sprite_inf>) {
                if (gfx.inactive_group(color))
                    continue;
            }

            if (flags && !(flags[gfx_x] & 0xf0))
                continue;

            if (gfx.prio > pri.src_prio[x]) {
                if (color) {
                    const uint16_t pal = gfx.palette_adjust(color);
                    uint8_t sel = gfx.blend_select(flags, gfx_x);

                    switch (gfx.blend_mode) {
                    case 0b01: // Normal blend
                        sel = 2 + sel;
                        [[fallthrough]];
                    case 0b10: // Reverse blend
                        if (line.blend[sel] == 0)
                            continue;
                        z.src_blend[x] = line.blend[sel];
                        break;
                    case 0b00: case 0b11: default: // Opaque layer
                        if (line.blend[sel] + line.blend[2 + sel] == 0)
                            continue;
                        z.src_blend[x] = line.blend[2 + sel];
                        z.dst_blend[x] = line.blend[sel];
                        pri.dst_prio[x] = gfx.prio;
                        z.dst_pal[x] = pal;
                        break;
                    }

                    z.src_pal[x] = pal;
                    pri.src_blendmode[x] = gfx.blend_mode;
                    pri.src_prio[x] = gfx.prio;
                }
            } else if (gfx.prio >= pri.dst_prio[x]) {
                if (color) {
                    const uint16_t pal = gfx.palette_adjust(color);
                    if (gfx.prio != pri.dst_prio[x])
                        z.dst_pal[x] = pal;
                    else
                        z.dst_pal[x] = 0; // Prio conflict
                    pri.dst_prio[x] = gfx.prio;
                    const bool sel = gfx.blend_select(flags, gfx_x);
                    switch (pri.src_blendmode[x]) {
                    case 0b01:
                        z.dst_blend[x] = line.blend[sel ? 1 : 0];
                        break;
                    case 0b10: case 0b00: case 0b11: default:
                        z.dst_blend[x] = line.blend[2 + (sel ? 1 : 0)];
                        break;
                    }
                }
            }
        }
    }

    void render_line(uint32_t *dst, const mix_pix &z, const uint8_t *palette_ram) {
        for (unsigned int x = H_START; x < H_START + H_VIS; ++x) {
            uint32_t s_pal = z.src_pal[x] & 0x1fff;
            uint32_t d_pal = z.dst_pal[x] & 0x1fff;

            uint32_t s_col = read_be32(&palette_ram[s_pal * 4]);
            uint32_t d_col = read_be32(&palette_ram[d_pal * 4]);

            uint16_t r1 = (s_col >> 16) & 0xff;
            uint16_t g1 = (s_col >> 8) & 0xff;
            uint16_t b1 = s_col & 0xff;

            uint16_t r2 = (d_col >> 16) & 0xff;
            uint16_t g2 = (d_col >> 8) & 0xff;
            uint16_t b2 = d_col & 0xff;

            r1 *= z.src_blend[x];
            g1 *= z.src_blend[x];
            b1 *= z.src_blend[x];

            r2 *= z.dst_blend[x];
            g2 *= z.dst_blend[x];
            b2 *= z.dst_blend[x];

            r1 += r2;
            g1 += g2;
            b1 += b2;

            r1 >>= 3;
            g1 >>= 3;
            b1 >>= 3;

            r1 = std::min<uint16_t>(r1, 255);
            g1 = std::min<uint16_t>(g1, 255);
            b1 = std::min<uint16_t>(b1, 255);

            dst[x - H_START] = 0xff000000 | (uint32_t(r1) << 16) | (uint32_t(g1) << 8) | uint32_t(b1);
        }
    }

    void scanline_draw(std::span<const uint8_t> palette_ram,
                       std::span<const uint8_t> graphics_ram,
                       std::span<uint32_t> output_argb) {
        const uint8_t *lineram = &graphics_ram[OFFS_LINERAM];
        const uint8_t *pf_ram  = &graphics_ram[OFFS_PF_RAM];
        const uint8_t *textram = &graphics_ram[OFFS_TEXTRAM];
        const uint8_t *pivot_ram = &graphics_ram[OFFS_PIVOT_RAM];

        decode_charram(&graphics_ram[OFFS_CHARRAM]);
        decode_pivot_ram(pivot_ram);
        update_row_usages(pf_ram, textram);

        for (auto &pf : pf_lines) pf.last_y = -1;
        text_line.last_y = -1;
        pivot_line.last_y = -1;

        f3_line_inf line_data{};
        for (int i = 0; i < NUM_SPRITEGROUPS; ++i) {
            line_data.sp[i].index = i;
        }
        for (int pf = 0; pf < NUM_PLAYFIELDS; ++pf) {
            get_pf_scroll(pf, line_data.pf[pf].reg_sx, line_data.pf[pf].reg_sy);
            line_data.pf[pf].reg_fx_y = line_data.pf[pf].reg_sy;
            line_data.pf[pf].width_mask = 0x3ff; // extend = 1
            line_data.pf[pf].index = pf;
        }

        if (flipscreen) {
            line_data.pivot.reg_sx = int16_t(control_1[4]) - 12;
            line_data.pivot.reg_sy = int16_t(control_1[5]);
        } else {
            line_data.pivot.reg_sx = -int16_t(control_1[4]) - 5;
            line_data.pivot.reg_sy = -int16_t(control_1[5]);
        }

        for (unsigned int screen_y = 0; screen_y != 256; ++screen_y) {
            const int y = flipscreen ? (255 - screen_y) : screen_y;
            read_line_ram(line_data, y, lineram);
            line_data.y = screen_y;

            for (int pf_num = 0; pf_num < NUM_PLAYFIELDS; ++pf_num) {
                auto &pf = line_data.pf[pf_num];
                pf.reg_fx_x = pf.reg_sx + pf.rowscroll;
                pf.reg_fx_x += 10 * (pf.x_scale - (1 << 8));
            }

            mix_pix line_buf{};
            pri_mode line_pri{};
            std::fill_n(line_buf.dst_pal, H_TOTAL, line_data.bg_palette);
            std::fill_n(line_buf.dst_blend, H_TOTAL, 8); // 100%
            std::fill_n(line_pri.src_blendmode, H_TOTAL, 0xff);
            std::fill_n(line_pri.dst_blendmode, H_TOTAL, 0xff);

            std::array<mixable*, 9> layers = {
                &line_data.pivot,
                &line_data.sp[0], &line_data.pf[0],
                &line_data.sp[3], &line_data.pf[3],
                &line_data.sp[2], &line_data.pf[2],
                &line_data.sp[1], &line_data.pf[1]
            };

            // Nine elements: stable insertion sort avoids stable_sort's
            // temporary heap allocation on every scanline.
            for (size_t i = 1; i < layers.size(); ++i) {
                auto *layer = layers[i];
                size_t j = i;
                while (j && layers[j - 1]->prio < layer->prio) {
                    layers[j] = layers[j - 1];
                    --j;
                }
                layers[j] = layer;
            }

            if (screen_y >= V_START && screen_y < (V_START + V_VIS)) {
                for (auto *layer : layers) {
                    if (layer == &line_data.pivot) {
                        if (line_data.pivot.layer_enable() && is_used(line_data.pivot, screen_y)) {
                            auto clip_ranges = calc_clip(line_data.clip, line_data.pivot);
                            int line_y = line_data.pivot.y_index(line_data.y);
                            if (line_data.pivot.use_pix()) {
                                generate_pixel_line(line_y, textram);
                                for (const auto &clip : clip_ranges) {
                                    mix_line_layer(line_data.pivot, line_buf, line_pri, line_data, clip,
                                                   pivot_line.pix.data(), pivot_line.flags.data());
                                }
                            } else {
                                generate_text_line(line_y, textram);
                                for (const auto &clip : clip_ranges) {
                                    mix_line_layer(line_data.pivot, line_buf, line_pri, line_data, clip,
                                                   text_line.pix.data(), text_line.flags.data());
                                }
                            }
                        }
                    } else if (layer == &line_data.sp[0] || layer == &line_data.sp[1] ||
                               layer == &line_data.sp[2] || layer == &line_data.sp[3]) {
                        auto &sp = static_cast<sprite_inf&>(*layer);
                        if (sp.layer_enable() && is_used(sp, screen_y)) {
                            auto clip_ranges = calc_clip(line_data.clip, sp);
                            const uint16_t *src = &sprite_framebuffer[line_data.y * H_TOTAL];
                            for (const auto &clip : clip_ranges) {
                                mix_line_layer(sp, line_buf, line_pri, line_data, clip, src, nullptr);
                            }
                        }
                    } else {
                        auto &pf = static_cast<playfield_inf&>(*layer);
                        if (pf.layer_enable() && is_used(pf, screen_y)) {
                            auto clip_ranges = calc_clip(line_data.clip, pf);
                            int line_y = pf.y_index(line_data.y);
                            generate_playfield_line(pf.index, line_y, pf_ram);
                            for (const auto &clip : clip_ranges) {
                                mix_line_layer(pf, line_buf, line_pri, line_data, clip,
                                               pf_lines[pf.index].pix.data(), pf_lines[pf.index].flags.data());
                            }
                        }
                    }
                }

                uint32_t *dst = &output_argb[(screen_y - V_START) * SCREEN_WIDTH];
                render_line(dst, line_buf, palette_ram.data());
            }

            if (screen_y != 0) {
                for (auto &pf : line_data.pf) {
                    pf.reg_fx_y += pf.y_scale;
                }
            }
        }
    }
};

Video::Video() : m_impl(std::make_unique<Impl>()) {}
Video::~Video() = default;
Video::Video(Video &&) noexcept = default;
Video &Video::operator=(Video &&) noexcept = default;

void Video::reset() {
    m_impl->reset();
}

bool Video::load_roms(std::span<const uint8_t> sprites,
                      std::span<const uint8_t> sprites_hi,
                      std::span<const uint8_t> tilemap,
                      std::span<const uint8_t> tilemap_hi) {
    return m_impl->decode_roms(sprites, sprites_hi, tilemap, tilemap_hi);
}

void Video::set_active_spriteram(std::span<const uint8_t> spriteram) {
    if (spriteram.size() >= 0x10000) {
        std::memcpy(m_impl->buffered_spriteram.data(), spriteram.data(), 0x10000);
        m_impl->has_buffered_spriteram = true;
        // Parse and pre-render into sprite framebuffer
        m_impl->get_sprite_info(m_impl->buffered_spriteram.data());
        m_impl->draw_sprites();
    }
}

void Video::vblank(std::span<const uint8_t> graphics_ram) {
    if (graphics_ram.size() >= GRAPHICS_RAM_SIZE) {
        m_impl->get_sprite_info(&graphics_ram[OFFS_SPRITERAM]);
        m_impl->draw_sprites();
        std::memcpy(m_impl->buffered_spriteram.data(), &graphics_ram[OFFS_SPRITERAM], 0x10000);
        m_impl->has_buffered_spriteram = true;
    }
}

void Video::render_frame(std::span<const uint8_t> palette_ram,
                         std::span<const uint8_t> graphics_ram,
                         std::span<const uint8_t> control_regs,
                         std::span<uint32_t> output_argb) {
    if (palette_ram.size() < 0x8000 || graphics_ram.size() < GRAPHICS_RAM_SIZE ||
        control_regs.size() < 0x20 || output_argb.size() < (SCREEN_WIDTH * SCREEN_HEIGHT)) {
        return;
    }

    // Load control registers:
    // control_regs[0..15] = control_0[0..7] (pf0..pf3 X, pf0..pf3 Y)
    // control_regs[16..31] = control_1[0..7] (pivot regs etc.)
    for (int i = 0; i < 8; ++i) {
        m_impl->control_0[i] = read_be16(&control_regs[i * 2]);
        m_impl->control_1[i] = read_be16(&control_regs[16 + i * 2]);
    }

    // Land Maker has sprite_lag = 1:
    // 1. scanline_draw() renders screen using current sprite_framebuffer
    //    (which holds sprites buffered from prior frame)
    m_impl->scanline_draw(palette_ram, graphics_ram, output_argb);

    // 2. Unless active spriteram was explicitly injected for a static single-frame test,
    //    buffer current spriteram and draw sprites into sprite_framebuffer for the next frame
    m_impl->get_sprite_info(&graphics_ram[OFFS_SPRITERAM]);
    m_impl->draw_sprites();
    std::memcpy(m_impl->buffered_spriteram.data(), &graphics_ram[OFFS_SPRITERAM], 0x10000);
    m_impl->has_buffered_spriteram = true;
}

std::span<const uint8_t> Video::sprite_tiles() const {
    return m_impl->decoded_sprites;
}

std::span<const uint8_t> Video::playfield_tiles() const {
    return m_impl->decoded_tiles;
}

VideoLine Video::inspect_playfield_line(unsigned layer, int y,
                                      std::span<const uint8_t> graphics_ram) {
    if (layer >= NUM_PLAYFIELDS || graphics_ram.size() < GRAPHICS_RAM_SIZE ||
        y < 0 || y >= 512 || m_impl->decoded_tiles.empty()) return {};
    auto &line = m_impl->pf_lines[layer];
    line.last_y = -1;
    m_impl->generate_playfield_line(int(layer), y, &graphics_ram[OFFS_PF_RAM]);
    return {line.pix, line.flags};
}

std::span<const uint16_t> Video::sprite_plane() const {
    return m_impl->sprite_framebuffer;
}

bool Video::roms_loaded() const {
    return !m_impl->decoded_sprites.empty();
}

bool Video::flipscreen() const {
    return m_impl->flipscreen;
}

} // namespace f3rt
