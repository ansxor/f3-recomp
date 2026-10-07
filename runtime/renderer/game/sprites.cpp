#include "renderer/game/sprites.hpp"
#include "state_io.hpp"
#include <algorithm>

namespace f3rt {

void GameSprites::reset() {
    staging_count_ = 0;
    submitted_count_ = 0;
    current_count_ = 0;
    current_flipped_ = false;
    current_pen_mask_ = 15;
    current_trails_ = false;

    reg_flipped_ = false;
    reg_pen_mask_ = 15;
    reg_trails_ = false;
    reg_bank_ = false;
}

void GameSprites::latch() {
    current_flipped_ = reg_flipped_;
    current_pen_mask_ = reg_pen_mask_;
    current_trails_ = reg_trails_;
    current_count_ = submitted_count_;

    // Decoded positions are already in scanout space (Video::get_sprite_info);
    // the latch only mirrors them for flipscreen.
    for (size_t i = 0; i < submitted_count_; ++i) {
        const auto &src = submitted_sprites_[i];
        auto &dst = current_sprites_[i];
        dst = src;
        if (current_flipped_) {
            dst.x = (512 << 8) - int32_t(src.scale_x) * 16 - src.x;
            dst.y = (256 << 8) - int32_t(src.scale_y) * 16 - src.y;
            dst.flip_x = !src.flip_x;
            dst.flip_y = !src.flip_y;
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

void GameSprites::raster(std::span<const uint8_t> assets, std::span<uint16_t> output,
                         GameVideoOptions options) const {
    raster_sprites(sprites(), pen_mask(), assets, output, {options, flipped(), trails()});
}

void raster_sprites(std::span<const SceneSprite> sprites, uint8_t mask, std::span<const uint8_t> assets,
                    std::span<uint16_t> output, SpriteRasterOptions raster) {
    const GameVideoOptions options = raster.geometry;
    if (!raster.accumulate) std::fill(output.begin(), output.end(), 0);
    if (assets.empty()) return;
    const bool expanded = options.expanded();
    const int scale = int(options.scale);
    const int width = expanded ? int(options.width()) : 432;
    const int origin_x = expanded ? 46 - int(options.border) : 0;
    const int origin_y = expanded ? 24 : 0;
    const int left = expanded ? 0 : 46, right = expanded ? width : 366;
    const int top = expanded ? 0 : 24, bottom = expanded ? int(options.height()) : 256;
    const int y_bias = raster.flipped ? 0 : 255;
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
           sizeof(uint8_t) * 4;
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
    writer.write(uint8_t(reg_flipped_ ? 1 : 0));
    writer.write(reg_pen_mask_);
    writer.write(uint8_t(reg_trails_ ? 1 : 0));
    writer.write(uint8_t(reg_bank_ ? 1 : 0));
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
    reader.read(u8); reg_flipped_ = u8 != 0;
    reader.read(reg_pen_mask_);
    reader.read(u8); reg_trails_ = u8 != 0;
    reader.read(u8); reg_bank_ = u8 != 0;
}

} // namespace f3rt
