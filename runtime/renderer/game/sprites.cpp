#include "renderer/game/sprites.hpp"
#include "renderer/decode.hpp"
#include "renderer/game/row_bits.hpp"
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

namespace {
// One raster geometry for both entry points; the row window is folded into top/bottom.
struct RasterGeometry {
    GameVideoOptions options;
    uint32_t tile_count;
    int scale, width, origin_x, origin_y, left, right, top, bottom, y_bias;
    bool expanded;
};

RasterGeometry make_geometry(std::span<const uint8_t> assets, SpriteRasterOptions raster) {
    const GameVideoOptions options = raster.geometry;
    RasterGeometry g{options,           uint32_t(assets.size() / 256), 0, 0, 0, 0, 0, 0, 0, 0, 0,
                     options.expanded()};
    g.scale = int(options.scale);
    g.width = g.expanded ? int(options.width()) : 432;
    g.origin_x = g.expanded ? 46 - int(options.border) : 0;
    g.origin_y = g.expanded ? int(geometry::FIRST_LINE) : 0;
    g.left = g.expanded ? 0 : 46;
    g.right = g.expanded ? g.width : 366;
    g.top = g.expanded ? 0 : int(geometry::FIRST_LINE);
    g.bottom = g.expanded ? int(options.height()) : int(geometry::END_LINE);
    g.y_bias = raster.flipped ? 0 : 255;
    return g;
}

// Mark one picture column of `plane_row` as covered by sprite layer `layer` (0..3).
void mark_coverage(SpriteCoverage &coverage, unsigned plane_row, unsigned layer, unsigned column) {
    coverage.layers[plane_row] |= uint8_t(1u << layer);
    set_columns(coverage.row(plane_row, layer).data(), int(column), 1);
}

// Bits [column, column + len) (len <= 16) of the union of every layer's coverage of `plane_row`:
// the columns an earlier sprite already holds.
[[gnu::always_inline]] inline uint32_t
covered_columns(const SpriteCoverage &coverage, unsigned plane_row, unsigned column, unsigned len) {
    uint32_t held = 0;
    for (unsigned layers = coverage.layers[plane_row]; layers; layers &= layers - 1)
        held |= extract_columns(coverage.row(plane_row, unsigned(__builtin_ctz(layers))).data(),
                                int(column), len);
    return held;
}

// A sprite at exactly 256/256 zoom on a scale-1 plane: every texel column is one plane column and
// every texel row one plane row, so the per-column geometry and the dy/dx loops collapse. Writes the
// same plane pixels as the general path below: the opaque texels of a texel row come from one mask,
// and the columns an earlier sprite already holds (first write wins) from the coverage bits, or
// from the plane when coverage is not tracked.
void draw_unscaled_sprite(const SceneSprite &sprite, const uint8_t *pixels,
                          std::span<uint16_t> plane, const RasterGeometry &g, uint8_t mask,
                          SpriteCoverage *coverage) {
    // Column x lands at base_x + x; the +128 rounds the fixed-point position like the general path.
    const int base_x = ((sprite.x + 128) >> 8) - g.origin_x;
    const int first_x = std::max(0, g.left - base_x), last_x = std::min(16, g.right - base_x);
    if (first_x >= last_x) return;
    const int base_y = ((sprite.y + g.y_bias) >> 8) - g.origin_y;
    const int first_y = std::max(0, g.top - base_y), last_y = std::min(16, g.bottom - base_y);
    const uint16_t color_base = uint16_t(0x1000 + (unsigned(sprite.palette) << 4));
    // A pen can carry into the layer bits 10-11, so the layer is constant only when the largest
    // pen leaves them alone; otherwise the layer comes from each written plane value.
    const bool one_layer = sprite_layer(uint16_t(color_base + mask)) == sprite_layer(color_base);
    const unsigned layer = unsigned(sprite_layer(color_base)) - unsigned(LayerId::Sp0);
    const unsigned window_width = unsigned(last_x - first_x);
    const uint32_t window = (1u << window_width) - 1;
    // Picture column of the window's first column.
    const unsigned column = unsigned(base_x + first_x - g.left);
    for (int y = first_y; y < last_y; ++y) {
        const unsigned plane_row = unsigned(base_y + y);
        uint16_t *line = plane.data() + size_t(plane_row) * g.width + base_x + first_x;
        uint32_t claimed = 0;
        if (coverage) {
            claimed = covered_columns(*coverage, plane_row, column, window_width);
        } else {
            for (unsigned i = 0; i < window_width; ++i) claimed |= uint32_t(line[i] != 0) << i;
        }
        if (claimed == window) continue; // every column already holds an earlier sprite
        const PenRow pens(pixels + size_t(y ^ (sprite.flip_y ? 15 : 0)) * 16, mask);
        if (pens.blank()) continue;
        const uint32_t opaque = (pens.opaque(sprite.flip_x) >> first_x) & window;
        const uint32_t fresh = opaque & ~claimed;
        if (!fresh) continue;
        if (!coverage || one_layer) {
            if (coverage) {
                coverage->layers[plane_row] |= uint8_t(1u << layer);
                set_columns(coverage->row(plane_row, layer).data(), int(column), fresh);
            }
            // Write four columns at a time from the pens of their group; `columns` is the fresh
            // set in sprite-column space.
            const uint32_t columns = fresh << first_x;
            for (unsigned group = 0; group < 4; ++group) {
                const uint32_t nibble = columns >> (4 * group) & 0xf;
                if (!nibble) continue;
                const uint32_t four = pens.group(group, sprite.flip_x);
                // Offset of the group's first column from the window's first column (negative when
                // the window starts inside the group; those columns are not fresh).
                const int offset = int(4 * group) - first_x;
                if (nibble == 0xf) {
                    line[offset] = uint16_t(color_base + (four & 0xff));
                    line[offset + 1] = uint16_t(color_base + ((four >> 8) & 0xff));
                    line[offset + 2] = uint16_t(color_base + ((four >> 16) & 0xff));
                    line[offset + 3] = uint16_t(color_base + (four >> 24));
                } else {
                    for (uint32_t rest = nibble; rest; rest &= rest - 1) {
                        const unsigned i = unsigned(__builtin_ctz(rest));
                        line[offset + int(i)] = uint16_t(color_base + ((four >> (8 * i)) & 0xff));
                    }
                }
            }
        } else {
            for (uint32_t rest = fresh; rest; rest &= rest - 1) {
                const unsigned i = unsigned(__builtin_ctz(rest));
                const uint16_t color =
                    uint16_t(color_base + ((pens.group((first_x + i) / 4, sprite.flip_x) >>
                                            (8 * ((first_x + i) % 4))) &
                                           0xff));
                line[i] = color;
                mark_coverage(*coverage, plane_row,
                              unsigned(sprite_layer(color)) - unsigned(LayerId::Sp0), column + i);
            }
        }
    }
}

void draw_sprite(const SceneSprite &sprite, std::span<const uint8_t> assets,
                 std::span<uint16_t> plane, const RasterGeometry &g, uint8_t mask,
                 SpriteCoverage *coverage) {
    // Cull the nominal fixed-point rectangle BEFORE rounding texel rows.
    // Otherwise a sprite ending exactly at the first visible line leaks its last row into
    // the active picture through the +255 native raster phase.
    if (sprite.x + sprite.scale_x * 16 <= (46 - int(g.options.border)) * 256 ||
        sprite.x > (365 + int(g.options.border)) * 256 ||
        sprite.y + sprite.scale_y * 16 <= int(geometry::FIRST_LINE) * 256 || sprite.y > 255 * 256)
        return;
    const auto *pixels = assets.data() + size_t(wrap_tile_index(sprite.tile, g.tile_count)) * 256;
    if (g.scale == 1 && sprite.scale_x == 256 && sprite.scale_y == 256) {
        draw_unscaled_sprite(sprite, pixels, plane, g, mask, coverage);
        return;
    }
    struct Column {
        int left, right, source;
    };
    std::array<Column, 16> columns;
    unsigned column_count = 0;
    // Column geometry and horizontal flip do not depend on the texel row.
    for (int x = 0; x < 16; ++x) {
        const int position_x = (sprite.x + x * sprite.scale_x) * g.scale + 128;
        const int start_x = (position_x >> 8) - g.origin_x * g.scale;
        const int end_x = ((position_x + sprite.scale_x * g.scale) >> 8) - g.origin_x * g.scale;
        if (start_x == end_x || end_x <= g.left || start_x >= g.right) continue;
        columns[column_count++] = {std::max(g.left, start_x), std::min(g.right, end_x),
                                   x ^ (sprite.flip_x ? 15 : 0)};
    }
    if (!column_count) return;
    for (int y = 0; y < 16; ++y) {
        const int position_y = (sprite.y + y * sprite.scale_y) * g.scale + g.y_bias;
        const int start_y = (position_y >> 8) - g.origin_y * g.scale;
        const int end_y = std::max(start_y + 1, ((position_y + sprite.scale_y * g.scale) >> 8) -
                                                    g.origin_y * g.scale);
        if (end_y <= g.top || start_y >= g.bottom) continue;
        const auto *row = pixels + size_t(y ^ (sprite.flip_y ? 15 : 0)) * 16;
        const int first_y = std::max(g.top, start_y), last_y = std::min(g.bottom, end_y);
        for (unsigned x = 0; x < column_count; ++x) {
            const auto &column = columns[x];
            const uint8_t pen = row[column.source] & mask;
            if (!pen) continue;
            const uint16_t color = uint16_t(0x1000 + (unsigned(sprite.palette) << 4) + pen);
            // The layer comes from each written plane value: a pen can carry into bits 10-11, so
            // one sprite may reach more than one layer.
            const unsigned layer = unsigned(sprite_layer(color)) - unsigned(LayerId::Sp0);
            for (int dy = first_y; dy < last_y; ++dy)
                for (int dx = column.left; dx < column.right; ++dx) {
                    auto &destination = plane[size_t(dy) * g.width + size_t(dx)];
                    if (coverage) {
                        if (covered_columns(*coverage, unsigned(dy), unsigned(dx - g.left), 1))
                            continue;
                        mark_coverage(*coverage, unsigned(dy), layer, unsigned(dx - g.left));
                    } else if (destination) {
                        continue;
                    }
                    destination = color;
                }
        }
    }
}
} // namespace

void raster_sprites(std::span<const SceneSprite> sprites, uint8_t mask,
                    std::span<const uint8_t> assets, std::span<uint16_t> output,
                    SpriteRasterOptions raster) {
    if (!raster.accumulate) std::fill(output.begin(), output.end(), 0);
    if (assets.empty()) return;
    const RasterGeometry g = make_geometry(assets, raster);
    for (size_t i = sprites.size(); i; --i)
        draw_sprite(sprites[i - 1], assets, output, g, mask, nullptr);
}

void SpriteBins::bin(std::span<const SceneSprite> sprites, SpriteRasterOptions raster) {
    const RasterGeometry g = make_geometry({}, raster);
    const unsigned chunks =
        (unsigned(g.bottom) - unsigned(g.top) + COMPOSE_CHUNK_ROWS - 1) / COMPOSE_CHUNK_ROWS;
    if (m_chunks.size() != chunks) m_chunks.resize(chunks);
    for (auto &bucket : m_chunks) bucket.clear();
    if (raster.accumulate) return; // chunked raster clears its window; trails keep the whole plane
    for (unsigned i = 0; i < sprites.size(); ++i) {
        const auto &sprite = sprites[i];
        // One row of margin each side: a bucket may hold a sprite that draws nothing in it, never
        // the other way round.
        const int start = ((sprite.y * g.scale + g.y_bias) >> 8) - g.origin_y * g.scale - 1;
        const int end = (((sprite.y + 16 * sprite.scale_y) * g.scale + g.y_bias) >> 8) -
                        g.origin_y * g.scale + 1;
        const int first = std::max(g.top, start), last = std::min(g.bottom, end);
        if (first >= last) continue;
        const unsigned begin_chunk = unsigned(first - g.top) / COMPOSE_CHUNK_ROWS;
        const unsigned end_chunk = unsigned(last - g.top) / COMPOSE_CHUNK_ROWS;
        for (unsigned chunk = begin_chunk; chunk <= end_chunk && chunk < chunks; ++chunk)
            m_chunks[chunk].push_back(uint16_t(i));
    }
}

void raster_sprite_chunk(const SpriteBins &bins, std::span<const SceneSprite> sprites,
                         unsigned chunk, uint8_t mask, std::span<const uint8_t> assets,
                         std::span<uint16_t> plane, SpriteRasterOptions raster,
                         SpriteCoverage &coverage) {
    const RasterGeometry g = make_geometry(assets, raster);
    const unsigned first = unsigned(g.top) + chunk * COMPOSE_CHUNK_ROWS;
    const unsigned last =
        std::min(unsigned(g.top) + (chunk + 1) * COMPOSE_CHUNK_ROWS, unsigned(g.bottom));
    if (first >= last) return;
    for (unsigned row = first; row < last; ++row) coverage.clear(row);
    if (assets.empty()) return;
    const auto bucket = bins.chunk(chunk);
    const RasterGeometry windowed{g.options,
                                  g.tile_count,
                                  g.scale,
                                  g.width,
                                  g.origin_x,
                                  g.origin_y,
                                  g.left,
                                  g.right,
                                  std::max(g.top, int(first)),
                                  std::min(g.bottom, int(last)),
                                  g.y_bias,
                                  g.expanded};
    for (size_t i = bucket.size(); i; --i)
        draw_sprite(sprites[bucket[i - 1]], assets, plane, windowed, mask, &coverage);
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
