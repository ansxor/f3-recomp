#include "game_sprites.hpp"
#include "state_io.hpp"
#include <algorithm>

namespace f3rt {
namespace {

inline int16_t sext12(uint16_t v) {
    v &= 0x0fff;
    return (v & 0x0800) ? int16_t(v | 0xf000) : int16_t(v);
}

} // namespace

void GameSprites::reset() {
    staging_count_ = 0;
    submitted_count_ = 0;
    current_count_ = 0;
    current_flipped_ = false;
    current_pen_mask_ = 15;
    current_trails_ = false;

    reg_scroll_x_ = 0;
    reg_scroll_y_ = 0;
    reg_flipped_ = false;
    reg_pen_mask_ = 15;
    reg_trails_ = false;

    supported_ = false;
    unsupported_pc_ = 0;
}

void GameSprites::latch() {
    current_flipped_ = reg_flipped_;
    current_pen_mask_ = reg_pen_mask_;
    current_trails_ = reg_trails_;
    current_count_ = submitted_count_;

    // Slot 1 origin: (46, 24) normal, (146, 0) flipped
    const int16_t origin_x = current_flipped_ ? 146 : 46;
    const int16_t origin_y = current_flipped_ ? 0 : 24;
    const int32_t scroll_x_24_8 = (int32_t(origin_x) + reg_scroll_x_) << 8;
    const int32_t scroll_y_24_8 = (int32_t(origin_y) + reg_scroll_y_) << 8;

    for (size_t i = 0; i < submitted_count_; ++i) {
        const auto &src = submitted_sprites_[i];
        auto &dst = current_sprites_[i];
        dst = src;

        const int32_t total_x = src.x + scroll_x_24_8;
        const int32_t total_y = src.y + scroll_y_24_8;

        if (current_flipped_) {
            dst.x = (512 << 8) - int32_t(src.scale_x) * 16 - total_x;
            dst.y = (256 << 8) - int32_t(src.scale_y) * 16 - total_y;
            dst.flip_x = !src.flip_x;
            dst.flip_y = !src.flip_y;
        } else {
            dst.x = total_x;
            dst.y = total_y;
            dst.flip_x = src.flip_x;
            dst.flip_y = src.flip_y;
        }
    }
}

std::span<const SceneSprite> GameSprites::sprites() const {
    return std::span<const SceneSprite>(current_sprites_.data(), current_count_);
}

bool GameSprites::flipped() const {
    return current_flipped_;
}

uint8_t GameSprites::pen_mask() const {
    return current_pen_mask_;
}

bool GameSprites::trails() const {
    return current_trails_;
}

bool GameSprites::supported() const {
    return supported_;
}

uint32_t GameSprites::unsupported_pc() const {
    return unsupported_pc_;
}

bool GameSprites::is_covered_write(uint32_t pc) {
    // 0x41d0..0x4380 covers init_sprites (41d0..4318, including slot 1023 init at 429c..4306)
    // and clear_spriteram (431a..4380)
    if (pc >= 0x41d0 && pc <= 0x4380) return true;
    if (pc >= 0x43b0 && pc <= 0x43de) return true; // sprite_scroll
    if (pc >= 0x43e0 && pc <= 0x43fe) return true; // sprite_command
    if (pc >= 0x4422 && pc <= 0x447e) return true; // list terminator
    if (pc >= 0x4688 && pc <= 0x46be) return true; // single sprite compiler
    if (pc >= 0x46c0 && pc <= 0x480a) return true; // grid sprite compiler
    if (pc >= 0x480c && pc <= 0x4a36) return true; // scaled grid compiler
    // Whitelist only actually modeled object helper ranges:
    if (pc >= 0xa8f38 && pc <= 0xa8f82) return true; // obj_grid
    if (pc >= 0xa8f84 && pc <= 0xa8fce) return true; // obj_3tile
    if (pc >= 0xa9036 && pc <= 0xa9076) return true; // obj_grid flip_x
    if (pc >= 0xa90f4 && pc <= 0xa913a) return true; // obj_4tile
    if (pc >= 0xa913c && pc <= 0xa93a2) return true; // obj_scaled
    return false;
}

void GameSprites::observe_write(uint32_t pc, uint32_t address) {
    if (address < 0x600000 || address >= 0x610000) return;
    if (is_covered_write(pc)) return;
    supported_ = false;
    if (!unsupported_pc_) unsupported_pc_ = pc;
}

void GameSprites::emit_sprite(uint32_t tile, int32_t x_24_8, int32_t y_24_8,
                              uint16_t scale_x, uint16_t scale_y,
                              uint8_t palette, bool flip_x, bool flip_y,
                              uint32_t caller_pc) {
    if (!tile) return;
    if (staging_count_ >= kMaxSprites) {
        supported_ = false;
        if (!unsupported_pc_) unsupported_pc_ = caller_pc;
        return;
    }

    auto &s = staging_sprites_[staging_count_++];
    s.tile = tile;
    s.scale_x = scale_x;
    s.scale_y = scale_y;
    s.x = x_24_8;
    s.y = y_24_8;
    s.palette = palette;
    s.flip_x = flip_x;
    s.flip_y = flip_y;
}

void GameSprites::parse_single(GameMemory &memory, const f3_cpu &cpu) {
    const uint32_t a0 = cpu.a[0];
    const uint32_t a4 = cpu.a[4];

    const uint16_t attr_xor = memory.u16(a0);
    const uint16_t tile = memory.u16(a0 + 2);
    if (!memory.supported) {
        supported_ = false;
        if (!unsupported_pc_) unsupported_pc_ = cpu.pc;
        return;
    }
    if (!tile) return;

    const uint8_t y_zoom = memory.u8(a4 + 1);
    const uint8_t x_zoom = memory.u8(a4 + 3);
    const uint16_t scale_x = uint16_t(256 - x_zoom);
    const uint16_t scale_y = uint16_t(256 - y_zoom);

    const int16_t x = sext12(memory.u16(a4 + 4));
    const int16_t y = sext12(memory.u16(a4 + 6));

    const uint16_t palette = memory.u16(a4 + 8) & 0xff;
    const uint16_t fx = memory.u16(a4 + 10) & 1;
    const uint16_t fy = memory.u16(a4 + 12) & 1;

    const uint16_t attr = (palette | (fx << 8) | (fy << 9)) ^ attr_xor;
    const bool flip_x = (attr & 0x100) != 0;
    const bool flip_y = (attr & 0x200) != 0;
    const uint8_t pal = uint8_t(attr & 0xff);

    emit_sprite(tile, int32_t(x) << 8, int32_t(y) << 8, scale_x, scale_y, pal, flip_x, flip_y, cpu.pc);

    if (!memory.supported) {
        supported_ = false;
        if (!unsupported_pc_) unsupported_pc_ = cpu.pc;
    }
}

void GameSprites::parse_grid(GameMemory &memory, const f3_cpu &cpu) {
    uint32_t a0 = cpu.a[0];
    const uint32_t a4 = cpu.a[4];
    const uint32_t d7 = cpu.d[7];

    const unsigned rows = ((d7 >> 16) & 0xffff) + 1;
    const unsigned cols = (d7 & 0xffff) + 1;
    if (rows == 0 || rows > 32 || cols == 0 || cols > 32) {
        supported_ = false;
        if (!unsupported_pc_) unsupported_pc_ = cpu.pc;
        return;
    }

    const uint32_t zoom_param = memory.u32(a4);
    const uint32_t coords = memory.u32(a4 + 4);
    const int16_t x0 = sext12(uint16_t(coords >> 16));
    const int16_t y0 = sext12(uint16_t(coords));

    const uint16_t palette = memory.u16(a4 + 8) & 0xff;
    // ROM 46d4 branches 4720 (neg X) when A4+10 != 0 -> HORIZONTAL flip
    const bool flip_x = memory.u16(a4 + 10) != 0;
    // ROM 46dc branches 4774 (neg Y) when A4+12 != 0 -> VERTICAL flip
    const bool flip_y = memory.u16(a4 + 12) != 0;

    if (zoom_param == 0) {
        for (unsigned c = 0; c < cols; ++c) {
            const int32_t cur_x_24_8 = (int32_t(x0) << 8) +
                (flip_x ? int32_t(cols - 1 - c) : int32_t(c)) * (16 << 8);
            for (unsigned r = 0; r < rows; ++r) {
                const int32_t cur_y_24_8 = (int32_t(y0) << 8) +
                    (flip_y ? int32_t(rows - 1 - r) : int32_t(r)) * (16 << 8);
                const uint32_t tile_entry = memory.u32(a0);
                a0 += 4;
                const uint16_t tile = uint16_t(tile_entry);
                if (tile) {
                    const uint16_t attr_xor = uint16_t(tile_entry >> 16);
                    const uint16_t attr = (palette | (flip_x ? 0x100 : 0) | (flip_y ? 0x200 : 0)) ^ attr_xor;
                    emit_sprite(tile, cur_x_24_8, cur_y_24_8, 256, 256,
                                uint8_t(attr & 0xff),
                                (attr & 0x100) != 0,
                                (attr & 0x200) != 0,
                                cpu.pc);
                }
            }
        }
    } else {
        // Scaled grid: bytes from blocks_0002.c:9346 (0xff00ff)
        // x_zoom = zoom_param & 0xff (bits 0..7)
        // y_zoom = (zoom_param >> 16) & 0xff (bits 16..23)
        const uint8_t zoom_x = uint8_t(zoom_param & 0xff);
        const uint8_t zoom_y = uint8_t((zoom_param >> 16) & 0xff);
        const uint16_t scale_x = uint16_t(256 - (zoom_x & 0xf0));
        const uint16_t scale_y = uint16_t(256 - zoom_y);
        // ROM $4830 seeds a half-pixel accumulator, but $4882/$4886
        // upload only integer words. These are NOT FDP chained tiles.
        const int origin_x = x0 + (flip_x ? ((cols - 1) * (256 - zoom_x) + 8) / 16 : 0);
        const int origin_y = y0 + (flip_y ? ((rows - 1) * scale_y + 8) / 16 : 0);
        for (unsigned c = 0; c < cols; ++c) {
            const int px = (origin_x * 256 + 128 + (flip_x ? -1 : 1) * int(c) * (256 - zoom_x) * 16) >> 8;
            const int32_t cur_x_24_8 = int32_t(sext12(uint16_t(px))) * 256;
            for (unsigned r = 0; r < rows; ++r) {
                const int py = (origin_y * 256 + 128 + (flip_y ? -1 : 1) * int(r) * scale_y * 16) >> 8;
                const int32_t cur_y_24_8 = int32_t(sext12(uint16_t(py))) * 256;
                const uint32_t tile_entry = memory.u32(a0);
                a0 += 4;
                const uint16_t tile = uint16_t(tile_entry);
                if (tile) {
                    const uint16_t attr_xor = uint16_t(tile_entry >> 16);
                    const uint16_t attr = (palette | (flip_x ? 0x100 : 0) | (flip_y ? 0x200 : 0)) ^ attr_xor;
                    emit_sprite(tile, cur_x_24_8, cur_y_24_8, scale_x, scale_y,
                                uint8_t(attr & 0xff),
                                (attr & 0x100) != 0,
                                (attr & 0x200) != 0,
                                cpu.pc);
                }
            }
        }
    }

    if (!memory.supported) {
        supported_ = false;
        if (!unsupported_pc_) unsupported_pc_ = cpu.pc;
    }
}

void GameSprites::parse_obj_grid(GameMemory &memory, const f3_cpu &cpu) {
    uint32_t a0 = cpu.a[0];
    const int16_t x0 = sext12(uint16_t(cpu.d[1]));
    const int16_t y0 = sext12(uint16_t(cpu.d[2]));
    const uint16_t d3 = uint16_t(cpu.d[3]);

    const unsigned rows = memory.u16(a0) + 1; a0 += 2;
    const unsigned cols = memory.u16(a0) + 1; a0 += 2;
    if (rows == 0 || rows > 32 || cols == 0 || cols > 32) {
        supported_ = false;
        if (!unsupported_pc_) unsupported_pc_ = cpu.pc;
        return;
    }

    const bool flip_x = (d3 & 0x100) != 0;

    for (unsigned c = 0; c < cols; ++c) {
        const int32_t cur_x_24_8 = (int32_t(x0) << 8) +
            (flip_x ? int32_t(cols - 1 - c) : int32_t(c)) * (16 << 8);
        for (unsigned r = 0; r < rows; ++r) {
            const int32_t cur_y_24_8 = (int32_t(y0) << 8) + int32_t(r) * (16 << 8);
            const uint32_t tile_entry = memory.u32(a0);
            a0 += 4;
            const uint16_t tile = uint16_t(tile_entry);
            if (tile) {
                const uint16_t attr = uint16_t(tile_entry >> 16) ^ d3;
                emit_sprite(tile, cur_x_24_8, cur_y_24_8, 256, 256,
                            uint8_t(attr & 0xff),
                            (attr & 0x100) != 0,
                            (attr & 0x200) != 0,
                            cpu.pc);
            }
        }
    }

    if (!memory.supported) {
        supported_ = false;
        if (!unsupported_pc_) unsupported_pc_ = cpu.pc;
    }
}

void GameSprites::parse_obj_3tile(GameMemory &memory, const f3_cpu &cpu) {
    const uint32_t a0 = cpu.a[0];
    const int16_t x0 = sext12(uint16_t(cpu.d[1]));
    const int16_t y0 = sext12(uint16_t(cpu.d[2]));
    const uint16_t d3 = uint16_t(cpu.d[3]);

    const uint16_t attr_xor = memory.u16(a0 + 4);
    const uint16_t attr = d3 ^ attr_xor;
    const uint16_t t0 = memory.u16(a0 + 6);
    if (t0) {
        emit_sprite(t0, (int32_t(x0) + 5) << 8, int32_t(y0) << 8, 256, 256,
                    uint8_t(attr & 0xff), (attr & 0x100) != 0, (attr & 0x200) != 0, cpu.pc);
        emit_sprite(memory.u16(a0 + 10), int32_t(x0) << 8, (int32_t(y0) + 16) << 8, 256, 256,
                    uint8_t(attr & 0xff), (attr & 0x100) != 0, (attr & 0x200) != 0, cpu.pc);
        emit_sprite(memory.u16(a0 + 14), (int32_t(x0) + 16) << 8, (int32_t(y0) + 16) << 8, 256, 256,
                    uint8_t(attr & 0xff), (attr & 0x100) != 0, (attr & 0x200) != 0, cpu.pc);
    }

    if (!memory.supported) {
        supported_ = false;
        if (!unsupported_pc_) unsupported_pc_ = cpu.pc;
    }
}

void GameSprites::parse_obj_4tile(GameMemory &memory, const f3_cpu &cpu) {
    const uint32_t a0 = cpu.a[0];
    const int16_t x0 = sext12(uint16_t(cpu.d[1]));
    const int16_t y0 = sext12(uint16_t(cpu.d[2]));
    const uint16_t d3 = uint16_t(cpu.d[3]);

    const uint16_t t0 = memory.u16(a0 + 6);
    if (t0) {
        const uint8_t pal = uint8_t(d3 & 0xff);
        const bool fx = (d3 & 0x100) != 0;
        const bool fy = (d3 & 0x200) != 0;
        emit_sprite(t0, int32_t(x0) << 8, int32_t(y0) << 8, 256, 256, pal, fx, fy, cpu.pc);
        emit_sprite(memory.u16(a0 + 10), int32_t(x0) << 8, (int32_t(y0) + 16) << 8, 256, 256, pal, fx, fy, cpu.pc);
        emit_sprite(memory.u16(a0 + 14), (int32_t(x0) + 16) << 8, int32_t(y0) << 8, 256, 256, pal, fx, fy, cpu.pc);
        emit_sprite(memory.u16(a0 + 18), (int32_t(x0) + 16) << 8, (int32_t(y0) + 16) << 8, 256, 256, pal, fx, fy, cpu.pc);
    }

    if (!memory.supported) {
        supported_ = false;
        if (!unsupported_pc_) unsupported_pc_ = cpu.pc;
    }
}

void GameSprites::parse_obj_scaled(GameMemory &memory, const f3_cpu &cpu) {
    uint32_t a0 = cpu.a[0];
    const uint32_t a3 = cpu.a[3];
    const uint16_t d3 = uint16_t((cpu.d[3] & 255) | ((memory.u16(a3 + 0x12) & 3) << 8));

    // Native C (blocks_0080.c:1314-1412):
    // zoom_x = memory.u8(a3 + 0x10); (byte at a3 + 16)
    // zoom_y = memory.u8(a3 + 0x12); (high byte of word at a3 + 18)
    const uint8_t zoom_x = memory.u8(a3 + 0x10);
    const uint8_t zoom_y = memory.u8(a3 + 0x12);
    const uint16_t scale_x = uint16_t(256 - (zoom_x & 0xf0));
    const uint16_t scale_y = uint16_t(256 - zoom_y);

    // Dimensions from ROM header at a0 (line 1529: read32 at a0):
    // high 16 bits = rows count - 1; low 16 bits = cols count - 1
    const uint32_t dims = memory.u32(a0);
    a0 += 4;
    const unsigned rows = ((dims >> 16) & 0xffff) + 1;
    const unsigned cols = (dims & 0xffff) + 1;
    if (rows == 0 || rows > 32 || cols == 0 || cols > 32) {
        supported_ = false;
        if (!unsupported_pc_) unsupported_pc_ = cpu.pc;
        return;
    }

    // Offset calculations from native C:
    // line 1638-1668: d1 += (cols * zoom_x + 8) / 32
    // line 1723-1743: d2 += (rows * zoom_y + 8) / 16
    const int16_t x0 = sext12(uint16_t(cpu.d[1])) + int16_t((cols * zoom_x + 8) / 32);
    const int16_t y0 = sext12(uint16_t(cpu.d[2])) + int16_t((rows * zoom_y + 8) / 16);

    const bool flip_x = (d3 & 0x100) != 0;
    const bool flip_y = (d3 & 0x200) != 0;
    const int origin_x = x0 + (flip_x ? ((cols - 1) * (256 - zoom_x) + 8) / 16 : 0);
    const int origin_y = y0 + (flip_y ? ((rows - 1) * scale_y + 8) / 16 : 0);
    const int fraction = uint16_t(cpu.a[5]) >> 8;
    for (unsigned c = 0; c < cols; ++c) {
        const int px = (origin_x * 256 + fraction + (flip_x ? -1 : 1) * int(c) * (256 - zoom_x) * 16) >> 8;
        const int32_t cur_x_24_8 = int32_t(sext12(uint16_t(px))) * 256;
        for (unsigned r = 0; r < rows; ++r) {
            const int py = (origin_y * 256 + fraction + (flip_y ? -1 : 1) * int(r) * scale_y * 16) >> 8;
            const int32_t cur_y_24_8 = int32_t(sext12(uint16_t(py))) * 256;
            const uint32_t tile_entry = memory.u32(a0);
            a0 += 4;
            const uint16_t tile = uint16_t(tile_entry);
            if (tile) {
                const uint16_t attr = uint16_t(tile_entry >> 16) ^ d3;
                emit_sprite(tile, cur_x_24_8, cur_y_24_8, scale_x, scale_y,
                            uint8_t(attr & 0xff),
                            (attr & 0x100) != 0,
                            (attr & 0x200) != 0,
                            cpu.pc);
            }
        }
    }

    if (!memory.supported) {
        supported_ = false;
        if (!unsupported_pc_) unsupported_pc_ = cpu.pc;
    }
}

void GameSprites::submit(GameMemory &memory, const f3_cpu &cpu) {
    if (!memory.supported) {
        supported_ = false;
        if (!unsupported_pc_) unsupported_pc_ = cpu.pc;
        return;
    }

    std::copy_n(staging_sprites_.begin(), staging_count_, submitted_sprites_.begin());
    submitted_count_ = staging_count_;
    staging_count_ = 0;

    // Completed valid batch restores support if not invalidated by unhandled write.
    // If unsupported_pc_ was set by an unhandled write, support conservatively remains
    // false until proven hardware reinitialization at 0x41d0.
    if (unsupported_pc_ == 0) {
        supported_ = true;
    }
}

void GameSprites::observe(GameMemory &memory, const f3_cpu &cpu) {
    switch (cpu.pc) {
    case 0x41d0:
        // Hardware sprite initialization is a proven reset point
        supported_ = true;
        unsupported_pc_ = 0;
        staging_count_ = 0;
        submitted_count_ = 0;
        current_count_ = 0;
        return;

    case 0x43b0:
        reg_scroll_x_ = sext12(memory.u16(0x407a16));
        reg_scroll_y_ = sext12(memory.u16(0x407a1a));
        if (!memory.supported) {
            supported_ = false;
            if (!unsupported_pc_) unsupported_pc_ = cpu.pc;
        }
        return;

    case 0x43e0: {
        const uint16_t cmd = memory.u16(0x407a1e);
        reg_flipped_ = (cmd & 0x2000) != 0;
        reg_pen_mask_ = uint8_t((((cmd >> 8) & 3) << 4) | 0x0f);
        reg_trails_ = (cmd & 0x0002) != 0;
        if (!memory.supported) {
            supported_ = false;
            if (!unsupported_pc_) unsupported_pc_ = cpu.pc;
        }
        return;
    }

    case 0x4528:
        staging_count_ = 0;
        return;

    case 0x4688:
        parse_single(memory, cpu);
        return;

    case 0x46c0:
        parse_grid(memory, cpu);
        return;

    case 0xa8f38:
        parse_obj_grid(memory, cpu);
        return;

    case 0xa8f84:
        parse_obj_3tile(memory, cpu);
        return;

    case 0xa90f4:
        parse_obj_4tile(memory, cpu);
        return;

    case 0xa913c:
        parse_obj_scaled(memory, cpu);
        return;

    case 0x4480:
        submit(memory, cpu);
        return;

    default:
        return;
    }
}
void GameSprites::raster(std::span<const uint8_t> assets, std::span<uint16_t> output,
                         GameVideoOptions options) const {
    if (!trails()) std::fill(output.begin(), output.end(), 0);
    if (assets.empty()) return;
    const bool expanded = options.expanded();
    const int scale = int(options.scale);
    const int width = expanded ? int(options.width()) : 432;
    const int origin_x = expanded ? 46 - int(options.border) : 0;
    const int origin_y = expanded ? 24 : 0;
    const int left = expanded ? 0 : 46, right = expanded ? width : 366;
    const int top = expanded ? 0 : 24, bottom = expanded ? int(options.height()) : 256;
    const int y_bias = flipped() ? 0 : 255;
    const uint8_t mask = pen_mask();
    const auto sprites = this->sprites();
    for (size_t i = sprites.size(); i; --i) {
        const auto &sprite = sprites[i - 1];
        // Cull the nominal fixed-point rectangle BEFORE rounding texel rows.
        // Otherwise a sprite ending exactly at Y=24 leaks its last row into
        // the active picture through the +255 native raster phase.
        if (sprite.x + sprite.scale_x * 16 <= (46 - int(options.border)) * 256 ||
            sprite.x > (365 + int(options.border)) * 256 ||
            sprite.y + sprite.scale_y * 16 <= 24 * 256 || sprite.y > 255 * 256)
            continue;
        const auto *pixels = assets.data() + (sprite.tile & 32767) * 256;
        struct Column { int left, right, source; };
        std::array<Column, 16> columns;
        unsigned column_count = 0;
        // Column geometry and horizontal flip do not depend on the texel row.
        for (int x = 0; x < 16; ++x) {
            const int position_x = (sprite.x + x * sprite.scale_x) * scale + 128;
            const int start_x = (position_x >> 8) - origin_x * scale;
            const int end_x = ((position_x + sprite.scale_x * scale) >> 8) - origin_x * scale;
            if (start_x == end_x || end_x <= left || start_x >= right) continue;
            columns[column_count++] = {std::max(left, start_x), std::min(right, end_x),
                                       x ^ (sprite.flip_x ? 15 : 0)};
        }
        if (!column_count) continue;
        for (int y = 0; y < 16; ++y) {
            const int position_y = (sprite.y + y * sprite.scale_y) * scale + y_bias;
            const int start_y = (position_y >> 8) - origin_y * scale;
            const int end_y = std::max(start_y + 1, ((position_y + sprite.scale_y * scale) >> 8) - origin_y * scale);
            if (end_y <= top || start_y >= bottom) continue;
            const auto *row = pixels + (y ^ (sprite.flip_y ? 15 : 0)) * 16;
            const int first_y = std::max(top, start_y), last_y = std::min(bottom, end_y);
            for (unsigned x = 0; x < column_count; ++x) {
                const auto &column = columns[x];
                const uint8_t pen = row[column.source] & mask;
                if (!pen) continue;
                const uint16_t color = uint16_t(0x1000 + (unsigned(sprite.palette) << 4) + pen);
                for (int dy = first_y; dy < last_y; ++dy)
                    for (int dx = column.left; dx < column.right; ++dx) {
                        auto &destination = output[dy * width + dx];
                        if (!destination) destination = color;
                    }
            }
        }
    }
}

size_t GameSprites::state_size() const {
    return sizeof(CanonicalSceneSprite) * kMaxSprites * 3 +
           sizeof(uint32_t) * 3 +
           sizeof(uint8_t) * 3 +
           sizeof(int16_t) * 2 +
           sizeof(uint8_t) * 3 +
           sizeof(uint8_t) +
           sizeof(uint32_t);
}

void GameSprites::save_state(StateWriter &writer) const {
    writer.write(uint32_t(staging_count_));
    for (size_t i = 0; i < kMaxSprites; ++i) {
        const auto &s = staging_sprites_[i];
        CanonicalSceneSprite ss{s.x, s.y, s.scale_x, s.scale_y, s.tile, s.palette,
                               uint8_t(s.flip_x ? 1 : 0), uint8_t(s.flip_y ? 1 : 0)};
        writer.write(ss);
    }
    writer.write(uint32_t(submitted_count_));
    for (size_t i = 0; i < kMaxSprites; ++i) {
        const auto &s = submitted_sprites_[i];
        CanonicalSceneSprite ss{s.x, s.y, s.scale_x, s.scale_y, s.tile, s.palette,
                               uint8_t(s.flip_x ? 1 : 0), uint8_t(s.flip_y ? 1 : 0)};
        writer.write(ss);
    }
    writer.write(uint32_t(current_count_));
    for (size_t i = 0; i < kMaxSprites; ++i) {
        const auto &s = current_sprites_[i];
        CanonicalSceneSprite ss{s.x, s.y, s.scale_x, s.scale_y, s.tile, s.palette,
                               uint8_t(s.flip_x ? 1 : 0), uint8_t(s.flip_y ? 1 : 0)};
        writer.write(ss);
    }
    writer.write(uint8_t(current_flipped_ ? 1 : 0));
    writer.write(current_pen_mask_);
    writer.write(uint8_t(current_trails_ ? 1 : 0));
    writer.write(reg_scroll_x_);
    writer.write(reg_scroll_y_);
    writer.write(uint8_t(reg_flipped_ ? 1 : 0));
    writer.write(reg_pen_mask_);
    writer.write(uint8_t(reg_trails_ ? 1 : 0));
    writer.write(uint8_t(supported_ ? 1 : 0));
    writer.write(unsupported_pc_);
}

void GameSprites::load_state(StateReader &reader) {
    uint32_t c;
    reader.read(c);
    if (c > kMaxSprites) throw std::invalid_argument("Snapshot staging sprite count out of range");
    staging_count_ = c;
    for (size_t i = 0; i < kMaxSprites; ++i) {
        CanonicalSceneSprite ss;
        reader.read(ss);
        auto &s = staging_sprites_[i];
        s.x = ss.x; s.y = ss.y;
        s.scale_x = ss.scale_x; s.scale_y = ss.scale_y;
        s.tile = ss.tile; s.palette = ss.palette;
        s.flip_x = ss.flip_x != 0; s.flip_y = ss.flip_y != 0;
    }
    reader.read(c);
    if (c > kMaxSprites) throw std::invalid_argument("Snapshot submitted sprite count out of range");
    submitted_count_ = c;
    for (size_t i = 0; i < kMaxSprites; ++i) {
        CanonicalSceneSprite ss;
        reader.read(ss);
        auto &s = submitted_sprites_[i];
        s.x = ss.x; s.y = ss.y;
        s.scale_x = ss.scale_x; s.scale_y = ss.scale_y;
        s.tile = ss.tile; s.palette = ss.palette;
        s.flip_x = ss.flip_x != 0; s.flip_y = ss.flip_y != 0;
    }
    reader.read(c);
    if (c > kMaxSprites) throw std::invalid_argument("Snapshot current sprite count out of range");
    current_count_ = c;
    for (size_t i = 0; i < kMaxSprites; ++i) {
        CanonicalSceneSprite ss;
        reader.read(ss);
        auto &s = current_sprites_[i];
        s.x = ss.x; s.y = ss.y;
        s.scale_x = ss.scale_x; s.scale_y = ss.scale_y;
        s.tile = ss.tile; s.palette = ss.palette;
        s.flip_x = ss.flip_x != 0; s.flip_y = ss.flip_y != 0;
    }
    uint8_t u8;
    reader.read(u8); current_flipped_ = u8 != 0;
    reader.read(current_pen_mask_);
    reader.read(u8); current_trails_ = u8 != 0;
    reader.read(reg_scroll_x_);
    reader.read(reg_scroll_y_);
    reader.read(u8); reg_flipped_ = u8 != 0;
    reader.read(reg_pen_mask_);
    reader.read(u8); reg_trails_ = u8 != 0;
    reader.read(u8); supported_ = u8 != 0;
    reader.read(unsupported_pc_);
}

} // namespace f3rt
