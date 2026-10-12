#include "renderer/game/sprites.hpp"
#include "renderer/decode.hpp"
#include "state_io.hpp"
#include <algorithm>
#include <array>

namespace f3rt {

void GameSprites::reset() {
    m_staging_count = 0;
    m_submitted_count = 0;
    m_current_count = 0;
    m_current_flipped = false;
    m_current_pen_mask = 15;
    m_current_trails = false;

    m_reg_flipped = false;
    m_reg_pen_mask = 15;
    m_reg_trails = false;
    m_reg_bank = false;
    m_presented_submitted_valid = m_presented_current_valid = false;
}

void GameSprites::decode(const VideoRam &vram) {
    SpriteRamState state;
    state.flipscreen = m_reg_flipped;
    state.bank = m_reg_bank;
    state.trails = m_reg_trails;
    state.pen_mask = m_reg_pen_mask;
    state.extra_planes = uint8_t(m_reg_pen_mask >> 4);
    const SpriteRamState initial = state;

    // Canonical list (serialized state): identities only, never splices.
    SpritePresentation identities;
    if (m_presentation) {
        identities.identity = m_presentation->identity;
        identities.object = m_presentation->object;
    }
    std::array<DecodedSpriteEntry, MAX_SPRITES> decoded;
    const size_t count = decode_sprite_list(vram.graphics.data(), int(geometry::FIRST_LINE),
                                            int(geometry::HEIGHT), decoded, state, false,
                                            identities.identity.empty() ? nullptr : &identities);

    auto convert = [](const DecodedSpriteEntry &d, SceneSprite &dst) {
        dst.x = d.x;
        dst.y = d.y;
        dst.scale_x = uint16_t(d.scale_x);
        dst.scale_y = uint16_t(d.scale_y);
        dst.tile = d.tile;
        dst.palette = d.color;
        dst.flip_x = d.flip_x;
        dst.flip_y = d.flip_y;
        dst.identity = d.identity;
        dst.object = d.object;
        dst.shadow = (d.flags & SPRITE_FLAG_SHADOW) != 0;
    };
    for (size_t i = 0; i < count; ++i) convert(decoded[i], m_staging_sprites[i]);
    // The decoded list is this frame's submission; GameVideo::latch_sprites()
    // applies the scanout origin and the one-frame lag.
    std::copy_n(m_staging_sprites.begin(), count, m_submitted_sprites.begin());
    m_submitted_count = count;
    m_staging_count = 0;

    // Render-only presented list with splices; capacity-truncated like the canonical one.
    m_presented_submitted_valid =
        m_presentation && (!m_presentation->splices.empty() || !m_presentation->flags.empty());
    if (m_presented_submitted_valid) {
        SpriteRamState presented_state = initial;
        std::array<DecodedSpriteEntry, MAX_PRESENTED_SPRITES> presented;
        const size_t n = decode_sprite_list(vram.graphics.data(), int(geometry::FIRST_LINE),
                                            int(geometry::HEIGHT), presented, presented_state,
                                            false, m_presentation);
        m_presented_submitted.resize(MAX_PRESENTED_SPRITES);
        for (size_t i = 0; i < n; ++i) convert(presented[i], m_presented_submitted[i]);
        m_presented_submitted_count = n;
        m_presented_overflow = n == MAX_PRESENTED_SPRITES;
    } else {
        m_presented_overflow = false;
    }

    m_reg_flipped = state.flipscreen;
    m_reg_pen_mask = state.pen_mask;
    m_reg_trails = state.trails;
    m_reg_bank = state.bank;
}

void GameSprites::latch() {
    m_current_flipped = m_reg_flipped;
    m_current_pen_mask = m_reg_pen_mask;
    m_current_trails = m_reg_trails;
    m_current_count = m_submitted_count;

    // Decoded positions are already in scanout space (Video::get_sprite_info);
    // the latch only mirrors them for flipscreen.
    auto mirror = [&](const SceneSprite &src, SceneSprite &dst) {
        dst = src;
        if (m_current_flipped) {
            dst.x = (512 << 8) - int32_t(src.scale_x) * 16 - src.x;
            dst.y = (256 << 8) - int32_t(src.scale_y) * 16 - src.y;
            dst.flip_x = !src.flip_x;
            dst.flip_y = !src.flip_y;
        }
    };
    for (size_t i = 0; i < m_submitted_count; ++i)
        mirror(m_submitted_sprites[i], m_current_sprites[i]);

    m_presented_current_valid = m_presented_submitted_valid;
    if (m_presented_current_valid) {
        m_presented_current.resize(MAX_PRESENTED_SPRITES);
        m_presented_current_count = m_presented_submitted_count;
        for (size_t i = 0; i < m_presented_current_count; ++i)
            mirror(m_presented_submitted[i], m_presented_current[i]);
    }
}

std::span<const SceneSprite> GameSprites::sprites() const {
    return std::span<const SceneSprite>(m_current_sprites.data(), m_current_count);
}

std::span<const SceneSprite> GameSprites::presented_sprites() const {
    if (!m_presented_current_valid) return sprites();
    return std::span<const SceneSprite>(m_presented_current.data(), m_presented_current_count);
}

bool GameSprites::flipped() const { return m_current_flipped; }

uint8_t GameSprites::pen_mask() const { return m_current_pen_mask; }

bool GameSprites::trails() const { return m_current_trails; }

void GameSprites::raster(std::span<const uint8_t> assets, std::span<uint16_t> output,
                         GameVideoOptions options) const {
    raster_sprites(sprites(), pen_mask(), assets, output, {options, flipped(), trails()});
}

void raster_sprites(std::span<const SceneSprite> sprites, uint8_t mask,
                    std::span<const uint8_t> assets, std::span<uint16_t> output,
                    SpriteRasterOptions raster) {
    const GameVideoOptions options = raster.geometry;
    if (!raster.accumulate) std::fill(output.begin(), output.end(), 0);
    if (assets.empty()) return;
    const uint32_t tile_count = uint32_t(assets.size() / 256);
    const bool expanded = options.expanded();
    const int scale = int(options.scale);
    const int width = expanded ? int(options.width()) : 432;
    const int origin_x = expanded ? 46 - int(options.border) : 0;
    const int origin_y = expanded ? int(geometry::FIRST_LINE) : 0;
    const int left = expanded ? 0 : 46, right = expanded ? width : 366;
    const int top = expanded ? 0 : int(geometry::FIRST_LINE),
              bottom = expanded ? int(options.height()) : int(geometry::END_LINE);
    const int y_bias = raster.flipped ? 0 : 255;
    for (size_t i = sprites.size(); i; --i) {
        const auto &sprite = sprites[i - 1];
        // Cull the nominal fixed-point rectangle BEFORE rounding texel rows.
        // Otherwise a sprite ending exactly at the first visible line leaks its last row into
        // the active picture through the +255 native raster phase.
        if (sprite.x + sprite.scale_x * 16 <= (46 - int(options.border)) * 256 ||
            sprite.x > (365 + int(options.border)) * 256 ||
            sprite.y + sprite.scale_y * 16 <= int(geometry::FIRST_LINE) * 256 ||
            sprite.y > 255 * 256)
            continue;
        const auto *pixels = assets.data() + size_t(wrap_tile_index(sprite.tile, tile_count)) * 256;
        struct Column {
            int left, right, source;
        };
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
            const int end_y = std::max(start_y + 1, ((position_y + sprite.scale_y * scale) >> 8) -
                                                        origin_y * scale);
            if (end_y <= top || start_y >= bottom) continue;
            const auto *row = pixels + size_t(y ^ (sprite.flip_y ? 15 : 0)) * 16;
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
    return sizeof(CanonicalSceneSprite) * MAX_SPRITES * 3 + sizeof(uint32_t) * 3 +
           sizeof(uint8_t) * 3 + sizeof(uint8_t) * 4;
}

void GameSprites::save_state(StateWriter &writer) const {
    writer.write(uint32_t(m_staging_count));
    for (size_t i = 0; i < MAX_SPRITES; ++i) {
        const auto &s = m_staging_sprites[i];
        CanonicalSceneSprite ss{s.x,
                                s.y,
                                s.scale_x,
                                s.scale_y,
                                s.tile,
                                s.palette,
                                uint8_t(s.flip_x ? 1 : 0),
                                uint8_t(s.flip_y ? 1 : 0)};
        writer.write(ss);
    }
    writer.write(uint32_t(m_submitted_count));
    for (size_t i = 0; i < MAX_SPRITES; ++i) {
        const auto &s = m_submitted_sprites[i];
        CanonicalSceneSprite ss{s.x,
                                s.y,
                                s.scale_x,
                                s.scale_y,
                                s.tile,
                                s.palette,
                                uint8_t(s.flip_x ? 1 : 0),
                                uint8_t(s.flip_y ? 1 : 0)};
        writer.write(ss);
    }
    writer.write(uint32_t(m_current_count));
    for (size_t i = 0; i < MAX_SPRITES; ++i) {
        const auto &s = m_current_sprites[i];
        CanonicalSceneSprite ss{s.x,
                                s.y,
                                s.scale_x,
                                s.scale_y,
                                s.tile,
                                s.palette,
                                uint8_t(s.flip_x ? 1 : 0),
                                uint8_t(s.flip_y ? 1 : 0)};
        writer.write(ss);
    }
    writer.write(uint8_t(m_current_flipped ? 1 : 0));
    writer.write(m_current_pen_mask);
    writer.write(uint8_t(m_current_trails ? 1 : 0));
    writer.write(uint8_t(m_reg_flipped ? 1 : 0));
    writer.write(m_reg_pen_mask);
    writer.write(uint8_t(m_reg_trails ? 1 : 0));
    writer.write(uint8_t(m_reg_bank ? 1 : 0));
}

void GameSprites::load_state(StateReader &reader) {
    uint32_t c;
    reader.read(c);
    if (c > MAX_SPRITES) throw std::invalid_argument("Snapshot staging sprite count out of range");
    m_staging_count = c;
    for (size_t i = 0; i < MAX_SPRITES; ++i) {
        CanonicalSceneSprite ss;
        reader.read(ss);
        auto &s = m_staging_sprites[i];
        s.x = ss.x;
        s.y = ss.y;
        s.scale_x = ss.scale_x;
        s.scale_y = ss.scale_y;
        s.tile = ss.tile;
        s.palette = ss.palette;
        s.flip_x = ss.flip_x != 0;
        s.flip_y = ss.flip_y != 0;
        s.identity = 0;
        s.object = 0;
    }
    reader.read(c);
    if (c > MAX_SPRITES)
        throw std::invalid_argument("Snapshot submitted sprite count out of range");
    m_submitted_count = c;
    for (size_t i = 0; i < MAX_SPRITES; ++i) {
        CanonicalSceneSprite ss;
        reader.read(ss);
        auto &s = m_submitted_sprites[i];
        s.x = ss.x;
        s.y = ss.y;
        s.scale_x = ss.scale_x;
        s.scale_y = ss.scale_y;
        s.tile = ss.tile;
        s.palette = ss.palette;
        s.flip_x = ss.flip_x != 0;
        s.flip_y = ss.flip_y != 0;
        s.identity = 0;
        s.object = 0;
    }
    reader.read(c);
    if (c > MAX_SPRITES) throw std::invalid_argument("Snapshot current sprite count out of range");
    m_current_count = c;
    for (size_t i = 0; i < MAX_SPRITES; ++i) {
        CanonicalSceneSprite ss;
        reader.read(ss);
        auto &s = m_current_sprites[i];
        s.x = ss.x;
        s.y = ss.y;
        s.scale_x = ss.scale_x;
        s.scale_y = ss.scale_y;
        s.tile = ss.tile;
        s.palette = ss.palette;
        s.flip_x = ss.flip_x != 0;
        s.flip_y = ss.flip_y != 0;
        s.identity = 0;
        s.object = 0;
    }
    uint8_t u8;
    reader.read(u8);
    m_current_flipped = u8 != 0;
    reader.read(m_current_pen_mask);
    reader.read(u8);
    m_current_trails = u8 != 0;
    reader.read(u8);
    m_reg_flipped = u8 != 0;
    reader.read(m_reg_pen_mask);
    reader.read(u8);
    m_reg_trails = u8 != 0;
    reader.read(u8);
    m_reg_bank = u8 != 0;
    m_presented_submitted_valid = m_presented_current_valid = false;
}

static_assert(Snapshotable<GameSprites>);

} // namespace f3rt
