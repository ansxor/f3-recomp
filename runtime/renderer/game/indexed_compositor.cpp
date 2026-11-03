#include "renderer/game/indexed_compositor.hpp"
#include "renderer/game/clip.hpp"
#include "renderer/game/mix.hpp"
#include "renderer/game/text.hpp"
#include <algorithm>
#include <array>

namespace f3rt {
namespace {
// Row index buffer sentinels. A resolved pixel holds its palette index; UNRESOLVED still needs its
// first visible sample; SPECIAL carries a partially blended PixelMix that every later layer feeds
// through the canonical mix().
constexpr uint16_t UNRESOLVED = 0xffff;
constexpr uint16_t SPECIAL = 0xfffe;
// Widest picture the fixed buffers must hold: native width plus both borders at the largest scale.
constexpr unsigned MAX_WIDTH =
    (geometry::NATIVE_WIDTH + GameVideoOptions::MAX_BORDER * 2) * GameVideoOptions::MAX_GPU_SCALE;
constexpr unsigned MAX_WORDS = MAX_WIDTH / 32;

int floor_divide(int value, int divisor) {
    return value >= 0 ? value / divisor : -1 - (-1 - value) / divisor;
}

// One row's working state. `px` is written only when a pixel becomes SPECIAL, so it is never read
// uninitialised and never cleared.
struct RowWork {
    const IndexedComposeJob &job;
    const SceneRow &row;
    unsigned width = 0;
    // Sprite plane layout: the expanded plane is one row per picture line at `width` columns; the
    // fixed native plane is 432 columns indexed by the raw scanline and only 46..365 are visible.
    unsigned plane_stride = 0, plane_row = 0;
    int left_edge = 0, right_edge = 0, border = 0;
    bool expanded = false, flipped = false, palette_15bit = false;
    std::array<uint16_t, MAX_WIDTH> *idx = nullptr;
    std::array<PixelMix, MAX_WIDTH> *px = nullptr;
    std::array<uint32_t, MAX_WORDS> unresolved{}, pending{};
    std::array<ClipRanges, LAYER_COUNT> clips{};
    bool any_pending = false;

    void reset() {
        std::fill_n(idx->begin(), width, UNRESOLVED);
        const unsigned words = (width + 31) / 32;
        for (unsigned w = 0; w < words; ++w) unresolved[w] = 0xffffffffu;
        if (width & 31) unresolved[words - 1] &= (1u << (width & 31)) - 1;
        std::fill_n(unresolved.begin() + words, MAX_WORDS - words, 0u);
        pending.fill(0u);
        any_pending = false;
    }
};

// Whether the first visible sample of this layer resolves the pixel by itself: mode 0/3 with the
// two weights of the selected slot summing to eight leaves (c*8)>>3 == c per channel.
[[gnu::always_inline]] inline bool resolves_first(const RowWork &work, const SceneLayer &layer,
                                                  bool select) {
    return (layer.blend_mode == 0 || layer.blend_mode == 3) &&
           work.row.blend[unsigned(select)] + work.row.blend[unsigned(select) + 2] == 8;
}

// Feed one visible sample into the row. Callers only pass pixels that are unresolved or pending, so
// a resolved pixel can never be revisited (the class-A row guarantees it is already final).
[[gnu::always_inline]] inline void visit(RowWork &work, int x, uint16_t palette, bool select,
                                         const SceneLayer &layer) {
    const unsigned word = unsigned(x) >> 5, bit = 1u << (unsigned(x) & 31);
    if (work.unresolved[word] & bit) {
        work.unresolved[word] &= ~bit;
        if (resolves_first(work, layer, select)) {
            (*work.idx)[x] = palette;
            return;
        }
        work.pending[word] |= bit;
        work.any_pending = true;
        (*work.idx)[x] = SPECIAL;
        PixelMix &pixel = (*work.px)[x];
        pixel = background_mix(work.row.background);
        mix(pixel, layer, palette, select, work.row.blend);
    } else if (work.pending[word] & bit) {
        mix((*work.px)[x], layer, palette, select, work.row.blend);
    }
}

// Calls body(x) for every unresolved or pending column of [left, right), skipping resolved spans.
template <class Body>
[[gnu::always_inline]] inline void for_each_visit(const RowWork &work, int left, int right,
                                                  Body &&body) {
    if (left >= right) return;
    const unsigned first_word = unsigned(left) >> 5, last_word = unsigned(right - 1) >> 5;
    for (unsigned w = first_word; w <= last_word; ++w) {
        uint32_t mask = work.unresolved[w] | work.pending[w];
        if (w == first_word) mask &= 0xffffffffu << (unsigned(left) & 31);
        if (w == last_word && (unsigned(right) & 31)) mask &= (1u << (unsigned(right) & 31)) - 1;
        while (mask) {
            const int x = int(w * 32 + unsigned(__builtin_ctz(mask)));
            mask &= mask - 1;
            body(x);
        }
    }
}

// Bits [x, x + len) of a row bitmask as bit i = column x + i. len <= 16, so at most two words.
[[gnu::always_inline]] inline uint32_t extract_columns(const std::array<uint32_t, MAX_WORDS> &words,
                                                       int x, unsigned len) {
    const unsigned word = unsigned(x) >> 5, shift = unsigned(x) & 31;
    uint32_t bits = words[word] >> shift;
    if (shift + len > 32) bits |= words[word + 1] << (32 - shift);
    return bits & ((1u << len) - 1);
}

// Clear the columns of `bits` (bit i = column x + i, at most 16 wide) in a row bitmask.
[[gnu::always_inline]] inline void clear_columns(std::array<uint32_t, MAX_WORDS> &words, int x,
                                                 uint32_t bits) {
    const unsigned word = unsigned(x) >> 5, shift = unsigned(x) & 31;
    words[word] &= ~(bits << shift);
    if (shift > 16) words[word + 1] &= ~(bits >> (32 - shift));
}

// Columns of [x, x + len) that still need a sample: unresolved, or pending a real blend.
[[gnu::always_inline]] inline uint32_t open_columns(const RowWork &work, int x, unsigned len) {
    const uint32_t open = extract_columns(work.unresolved, x, len);
    return work.any_pending ? open | extract_columns(work.pending, x, len) : open;
}

constexpr uint32_t reverse16(uint32_t v) {
    v = ((v & 0x5555u) << 1) | ((v >> 1) & 0x5555u);
    v = ((v & 0x3333u) << 2) | ((v >> 2) & 0x3333u);
    v = ((v & 0x0f0fu) << 4) | ((v >> 4) & 0x0f0fu);
    return ((v & 0x00ffu) << 8) | ((v >> 8) & 0x00ffu);
}

int clip_left(const RowWork &work, const ClipRanges &ranges, unsigned r) {
    return std::max(work.left_edge, int(ranges.ranges[r].left)) - work.left_edge;
}
int clip_right(const RowWork &work, const ClipRanges &ranges, unsigned r) {
    return std::min(work.right_edge, int(ranges.ranges[r].right)) - work.left_edge;
}

// A playfield layer's row-decoded state.
struct PlayfieldState {
    ScenePlayfield pf;
    GameTiles::RowSampler sampler;
};

PlayfieldState playfield_state(const RowWork &work, LayerId id) {
    PlayfieldState state;
    state.pf = work.row.playfields[sub_index(id)];
    // The canonical native kernel never applies the presented (full-resolution alt map) transform.
    if (work.expanded && work.job.scene.presented) state.pf = presented_playfield(state.pf);
    // Scale 1 samples the native subpixel phase: source_y is the playfield row, y_step unused.
    state.sampler = work.job.scene.tiles.row_sampler(sub_index(id), state.pf.source_y,
                                                     work.flipped, work.job.scene.tile_pixels,
                                                     state.pf.alt_map);
    return state;
}

// Mosaic (any layer kind) and zoom (playfields) sample per column, exactly like the canonical row
// kernel's generic path.
void compose_generic_layer(RowWork &work, LayerId id, const SceneLayer &layer) {
    const auto &ranges = work.clips[unsigned(id)];
    const LayerKind layer_kind = kind(id);
    const bool mosaic = layer.mosaic && work.row.mosaic_period > 1;
    PlayfieldState playfield;
    if (layer_kind == LayerKind::Playfield) playfield = playfield_state(work, id);
    const uint16_t *plane =
        work.job.plane.data() + size_t(work.plane_row) * work.plane_stride;
    for (unsigned r = 0; r < ranges.count; ++r) {
        const int left = clip_left(work, ranges, r), right = clip_right(work, ranges, r);
        for_each_visit(work, left, right, [&](int x) {
            int sample_x = x + work.left_edge;
            if (mosaic) {
                const int hardware_x = work.left_edge + x;
                const int phase = hardware_x + 68;
                const int count = phase < 0 ? phase + 432 : phase >= 432 ? phase - 432 : phase;
                sample_x = hardware_x - count % work.row.mosaic_period;
            }
            ScenePixel source;
            bool select = layer.blend_select;
            if (layer_kind == LayerKind::Playfield) {
                const int source_x = floor_divide(
                    playfield.pf.source_x + (sample_x - 46) * playfield.pf.x_step, 256);
                source = playfield.sampler.pixel(source_x);
                select = (source.flags & 1) != 0;
                if (!(source.flags & 0x10)) return;
                source.palette = uint16_t(source.palette + playfield.pf.palette_add);
            } else if (layer_kind == LayerKind::Sprite) {
                // Native plane columns are hardware scanout columns; expanded ones are picture
                // columns (the border shifted out).
                const int source_x = work.expanded ? sample_x - work.left_edge : sample_x;
                if (source_x < 0 || source_x >= int(work.plane_stride)) return;
                source.palette = plane[source_x];
                if (!source.palette || sprite_layer(source.palette) != id) return;
            } else {
                const int source_x = work.row.text_x + sample_x - 46;
                source = work.job.scene.text.pixel(source_x, work.row.text_y, work.flipped);
                if (!(source.flags & 0x10)) return;
            }
            visit(work, x, source.palette, select, layer);
        });
    }
}

// The playfield run kernel: x_step == 256 advances one texel per output column, so each map cell is
// one run of at most 16 columns. A run walks bitmasks: the columns still open, intersected with the
// opacity table's opaque texels when it proves them, and the unresolved ones that resolve on first
// sight are written directly.
void compose_playfield(RowWork &work, LayerId id, const SceneLayer &layer) {
    PlayfieldState state = playfield_state(work, id);
    if (state.pf.x_step != 256) {
        compose_generic_layer(work, id, layer);
        return;
    }
    const auto cells = work.job.scene.tiles.cells(
        GameTiles::physical_map(sub_index(id), state.pf.alt_map));
    const auto tile_pixels = work.job.scene.tile_pixels;
    const uint32_t tile_count = uint32_t(tile_pixels.size() / 256);
    const TileRowOpacity *opacity = work.job.opacity;
    const bool opacity_usable = opacity && !opacity->empty() &&
                                opacity->tile_count() == tile_count && tile_count != 0;
    constexpr unsigned MAP_WIDTH = GameTiles::MAP_WIDTH_PX;
    const unsigned screen_flip = work.flipped ? MAP_WIDTH - 1 : 0;
    const int base = floor_divide(state.pf.source_x, 256);
    const unsigned wrapped_y = (unsigned(state.pf.source_y) & 511) ^ (work.flipped ? 511 : 0);
    const uint32_t *map_row = cells.data() + size_t(wrapped_y / 16) * GameTiles::COLUMNS;
    const unsigned texel_y = wrapped_y & 15;
    const auto &ranges = work.clips[unsigned(id)];
    for (unsigned r = 0; r < ranges.count; ++r) {
        const int left = clip_left(work, ranges, r), right = clip_right(work, ranges, r);
        int x = left;
        while (x < right) {
            const unsigned wrapped =
                (unsigned(base + x - work.border) & (MAP_WIDTH - 1)) ^ screen_flip;
            const unsigned cell = wrapped >> 4, texel = wrapped & 15;
            const unsigned run = std::min<unsigned>(
                work.flipped ? texel + 1 : 16 - texel, unsigned(right - x));
            // Columns of the run that no earlier layer has settled; a settled run is skipped
            // without touching its map cell.
            const uint32_t unresolved = extract_columns(work.unresolved, x, run);
            const uint32_t open =
                work.any_pending ? unresolved | extract_columns(work.pending, x, run) : unresolved;
            if (!open) {
                x += int(run);
                continue;
            }
            const uint32_t word = map_row[cell];
            const uint16_t attributes = uint16_t(word >> 16);
            const uint32_t tile = wrap_tile_index(uint16_t(word), tile_count);
            const unsigned tile_y = texel_y ^ ((attributes & 0x8000) ? 15 : 0);
            const uint8_t *pens = tile_pixels.data() + size_t(tile) * 256 + size_t(tile_y) * 16;
            const uint8_t pen_mask = uint8_t((((attributes >> 10) & 3 & ~attributes) << 4) | 15);
            const unsigned flip_x = (attributes & 0x4000) ? 15 : 0;
            const bool select = (attributes & 0x200) != 0;
            const uint16_t palette = uint16_t((attributes & 0x1ff) * 16 + state.pf.palette_add);
            // Column i of the run shows texel (texel -/+ i) ^ flip_x of the row.
            const auto pen_at = [&](unsigned i) {
                return uint8_t(pens[((work.flipped ? texel - i : texel + i)) ^ flip_x] & pen_mask);
            };
            uint32_t live = open;
            if (opacity_usable && pen_mask == 15) {
                // A set opacity bit is an opaque texel and, with the plain pen mask, a clear one is
                // transparent: the table answers every texel of the run at once. Column i is
                // texel bit (texel -/+ i) ^ flip_x, so the row is mirrored when exactly one of the
                // screen flip and the cell flip applies.
                const uint32_t row = opacity->row(tile, tile_y);
                const uint32_t mirrored = work.flipped != (flip_x != 0) ? reverse16(row) : row;
                live &= mirrored >> (work.flipped ? 15 - texel : texel);
            }
            if (live && resolves_first(work, layer, select)) {
                // Unresolved opaque columns are final as they are: write them, settle them with
                // one mask operation, and leave only the pending ones to the blend.
                uint32_t settled = 0;
                for (uint32_t fast = live & unresolved; fast; fast &= fast - 1) {
                    const unsigned i = unsigned(__builtin_ctz(fast));
                    const uint8_t pen = pen_at(i);
                    if (!pen) continue;
                    (*work.idx)[x + int(i)] = uint16_t(palette + pen);
                    settled |= 1u << i;
                }
                clear_columns(work.unresolved, x, settled);
                live &= ~unresolved;
            }
            for (; live; live &= live - 1) {
                const unsigned i = unsigned(__builtin_ctz(live));
                const uint8_t pen = pen_at(i);
                if (pen) visit(work, x + int(i), uint16_t(palette + pen), select, layer);
            }
            x += int(run);
        }
    }
}

// Text: walks one glyph cell at a time, mirrored when the frame is flipped exactly like
// GameText::pixel. A cell none of whose columns is open, or whose glyph row is blank, costs no
// pixel work.
void compose_text(RowWork &work, LayerId id, const SceneLayer &layer) {
    const auto &text = work.job.scene.text;
    const auto &ranges = work.clips[unsigned(id)];
    int y = work.row.text_y & 511;
    if (work.flipped) y = 511 - y;
    const uint16_t *map = text.map().data();
    const uint8_t *glyphs = text.glyphs().data();
    const unsigned row_base = unsigned(y / 8) * 64;
    const unsigned texel_y = unsigned(y & 7);
    for (unsigned r = 0; r < ranges.count; ++r) {
        const int left = clip_left(work, ranges, r), right = clip_right(work, ranges, r);
        int x = left;
        while (x < right) {
            // Text column u of the 512-wide map; a cell spans eight consecutive u.
            const unsigned u = unsigned(work.row.text_x + x - work.border) & 511;
            const unsigned phase = u & 7;
            const unsigned len = std::min(8 - phase, unsigned(right - x));
            const uint32_t open = open_columns(work, x, len);
            if (open) {
                const unsigned cell = work.flipped ? 63 - (u >> 3) : u >> 3;
                const uint16_t word = map[row_base + cell];
                const unsigned flip_x = (word & 0x0100) ? 7u : 0u;
                const unsigned flip_y = (word & 0x8000) ? 7u : 0u;
                const uint8_t *glyph =
                    glyphs + size_t(word & 0xff) * 32 + size_t((texel_y ^ flip_y) * 4);
                // The row's eight 4-bit texels as one word: texel t is nibble t.
                const uint32_t texels = uint32_t(glyph[0]) << 24 | uint32_t(glyph[1]) << 16 |
                                        uint32_t(glyph[2]) << 8 | glyph[3];
                const uint16_t palette_base = uint16_t(((word >> 9) & 0x3f) * 16);
                if (texels)
                    for (uint32_t bits = open; bits; bits &= bits - 1) {
                        const unsigned i = unsigned(__builtin_ctz(bits));
                        const unsigned column = work.flipped ? 7 - (phase + i) : phase + i;
                        const unsigned pen = (texels >> (((column ^ flip_x)) * 4)) & 0x0f;
                        if (pen)
                            visit(work, x + int(i), uint16_t(palette_base + pen),
                                  layer.blend_select, layer);
                    }
            }
            x += int(len);
        }
    }
}

void compose_sprites(RowWork &work, LayerId id, const SceneLayer &layer) {
    const unsigned index = sub_index(id);
    const SpriteRowSpans::Row &span = work.job.spans.rows[work.plane_row];
    if (!(span.mask & (1u << index))) return; // no plane value of this layer on the row
    const auto &ranges = work.clips[unsigned(id)];
    const uint16_t *plane = work.job.plane.data() + size_t(work.plane_row) * work.plane_stride;
    // Spans and the native plane are indexed by plane column (the native plane keeps the 46-column
    // left margin); the row's index buffer is indexed by picture column.
    const int shift = work.expanded ? 0 : work.left_edge;
    for (unsigned r = 0; r < ranges.count; ++r) {
        const int left = std::max(clip_left(work, ranges, r), int(span.lo[index]) - shift);
        const int right = std::min(clip_right(work, ranges, r), int(span.hi[index]) - shift);
        for_each_visit(work, left, right, [&](int x) {
            const uint16_t palette = plane[x + shift];
            if (!palette || sprite_layer(palette) != id) return;
            visit(work, x, palette, layer.blend_select, layer);
        });
    }
}

// Resolve one pixel to the canonical 0xAARRGGBB word.
[[gnu::always_inline]] inline uint32_t resolve_argb(const RowWork &work, int x) {
    const uint16_t value = (*work.idx)[x];
    if (value == SPECIAL)
        return work.palette_15bit ? rgb<true>((*work.px)[x], work.job.scene.colors)
                                  : rgb<false>((*work.px)[x], work.job.scene.colors);
    const uint16_t index = value == UNRESOLVED ? work.row.background : value;
    const uint32_t color = work.job.scene.colors[index & 8191];
    return 0xff000000u | (work.palette_15bit ? color15(color) : color);
}

bool row_is_class_a(const SceneRow &row, const std::array<bool, LAYER_COUNT> &active) {
    // The first visible sample of a mode 0/3 layer is final when every active layer outranks the
    // cleared destination and equal priorities cannot reintroduce a different blend.
    std::array<uint8_t, 16> mode{};
    mode.fill(0xff);
    for (unsigned i = 0; i < LAYER_COUNT; ++i) {
        if (!active[i]) continue;
        const auto &layer = row.layer(LayerId{uint8_t(i)});
        if (!layer.priority || layer.priority >= mode.size()) return false;
        uint8_t &slot = mode[layer.priority];
        if (slot == 0xff)
            slot = layer.blend_mode;
        else if (slot != layer.blend_mode)
            return false;
    }
    return true;
}

void compose_row(const IndexedComposeJob &job, unsigned y, uint32_t *argb_scratch) {
    const auto &scene = job.scene;
    const auto &row = scene.rows[y & 255];
    const unsigned width = job.geometry.width();
    const unsigned picture_row = y - geometry::FIRST_LINE;
    uint32_t *out = job.output.data() + size_t(picture_row) * job.stride;
    const bool flipped = scene.flipped;
    const bool expanded = job.geometry.expanded();
    const unsigned plane_row = expanded ? picture_row : y;
    // A sprite layer with no plane value on this row cannot change any pixel of it, so it is not
    // active here (the canonical kernel would sample and skip all of it). Leaving it out also keeps
    // its line-RAM priority out of the class-A test.
    const SpriteRowSpans::Row &sprite_span = job.spans.rows[plane_row];
    std::array<bool, LAYER_COUNT> active{};
    for (unsigned i = 0; i < LAYER_COUNT; ++i) {
        const LayerId id{uint8_t(i)};
        const int text_y = flipped ? (511 - row.text_y) : row.text_y;
        const bool empty =
            (kind(id) == LayerKind::Playfield && row.playfields[sub_index(id)].empty_row) ||
            (id == LayerId::Text && !scene.text.row_used(unsigned(text_y))) ||
            (kind(id) == LayerKind::Sprite &&
             !(sprite_span.mask & (1u << sub_index(id))));
        active[i] = row.layer(id).enabled && (scene.layer_mask & layer_bit(id)) != 0 && !empty;
    }
    if (!row_is_class_a(row, active)) {
        // Not provably first-visible-layer-wins: the canonical row kernel decides every pixel.
        SceneTarget target{job.plane, {argb_scratch, width}, job.geometry};
        compose_scene_rows(scene, target, y, y, y + 1);
        for (unsigned sub = 0; sub < job.geometry.scale; ++sub) {
            const uint32_t *source = argb_scratch + size_t(sub) * width;
            uint32_t *row = out + size_t(sub) * job.stride;
            for (unsigned x = 0; x < width; ++x) row[x] = to_pixel_format(source[x], job.format);
        }
        if (job.stats) job.stats->fallback_rows.fetch_add(1, std::memory_order_relaxed);
        return;
    }

    std::array<uint16_t, MAX_WIDTH> idx;
    std::array<PixelMix, MAX_WIDTH> px;
    RowWork work{job, row};
    work.width = width;
    work.expanded = expanded;
    work.plane_stride = expanded ? width : 432;
    work.plane_row = plane_row;
    work.left_edge = 46 - int(job.geometry.border);
    work.right_edge = 366 + int(job.geometry.border);
    work.border = int(job.geometry.border);
    work.flipped = flipped;
    work.palette_15bit = row.palette_15bit;
    work.idx = &idx;
    work.px = &px;
    work.reset();

    for (LayerId id : scene_order(row)) {
        if (!active[unsigned(id)]) continue;
        const auto &layer = row.layer(id);
        work.clips[unsigned(id)] =
            clip_ranges(row, layer, int16_t(work.left_edge), int16_t(work.right_edge));
        if (layer.mosaic && row.mosaic_period > 1) {
            compose_generic_layer(work, id, layer);
            continue;
        }
        switch (kind(id)) {
        case LayerKind::Playfield:
            compose_playfield(work, id, layer);
            break;
        case LayerKind::Sprite:
            compose_sprites(work, id, layer);
            break;
        case LayerKind::Text:
            compose_text(work, id, layer);
            break;
        }
    }

    if (row.blur) {
        // Blur averages ARGB channels, so resolve in ARGB, blur, then convert to the output order.
        for (unsigned x = 0; x < width; ++x) argb_scratch[x] = resolve_argb(work, int(x));
        for (int x = int(width) - 1; x >= 0; --x) {
            const uint32_t color = argb_scratch[x], previous = x >= 1 ? argb_scratch[x - 1] : 0;
            const uint32_t rb =
                (((color & 0x00ff00ffu) + (previous & 0x00ff00ffu)) >> 1) & 0x00ff00ffu;
            const uint32_t g =
                (((color & 0x0000ff00u) + (previous & 0x0000ff00u)) >> 1) & 0x0000ff00u;
            argb_scratch[x] = 0xff000000u | rb | g;
        }
        for (unsigned x = 0; x < width; ++x) out[x] = to_pixel_format(argb_scratch[x], job.format);
        return;
    }
    const std::span<const uint32_t> lut = row.palette_15bit ? job.palette15 : job.palette;
    if (!work.any_pending) {
        for (unsigned x = 0; x < width; ++x) {
            const uint16_t value = idx[x];
            out[x] = lut[(value == UNRESOLVED ? row.background : value) & 8191];
        }
        return;
    }
    uint32_t special = 0;
    for (unsigned x = 0; x < width; ++x) {
        const uint16_t value = idx[x];
        if (value == SPECIAL) {
            ++special;
            out[x] = to_pixel_format(resolve_argb(work, int(x)), job.format);
        } else {
            out[x] = lut[(value == UNRESOLVED ? row.background : value) & 8191];
        }
    }
    if (job.stats && special) job.stats->special_px.fetch_add(special, std::memory_order_relaxed);
}
} // namespace

void IndexedPalette::build(std::span<const uint32_t> palette, PixelFormat format, bool need_15bit) {
    for (unsigned i = 0; i < colors.size(); ++i) {
        colors[i] = to_pixel_format(0xff000000u | (palette[i] & 0xffffffu), format);
        if (need_15bit) colors15[i] = to_pixel_format(0xff000000u | color15(palette[i]), format);
    }
    uses_15bit = need_15bit;
}

void IndexedScratch::configure(GameVideoOptions geometry) {
    const size_t plane_size = sprite_plane_size(geometry);
    if (plane.size() != plane_size) plane.assign(plane_size, 0);
    const size_t rows = geometry.expanded() ? geometry.height() : size_t(256);
    if (span_rows.size() != rows) span_rows.assign(rows, SpriteRowSpans::Row{});
}

bool indexed_frame_uses_15bit(const FrameScene &scene) noexcept {
    for (unsigned y = geometry::FIRST_LINE; y < geometry::END_LINE; ++y)
        if (scene.rows[y & 255].palette_15bit) return true;
    return false;
}

void compose_indexed_chunk(IndexedComposeJob &job, unsigned chunk) noexcept {
    const unsigned rows = job.geometry.height();
    const unsigned first = chunk * COMPOSE_CHUNK_ROWS;
    const unsigned last = std::min(first + COMPOSE_CHUNK_ROWS, rows);
    if (first >= last) return;
    // raster_captured_sprites() rasterises the captured list without the flipscreen row phase (the
    // latched positions are already in scanout space), so the fused raster matches it exactly.
    raster_sprite_chunk(*job.bins, job.sprites, chunk, job.pen_mask, job.sprite_tiles, job.plane,
                        SpriteRasterOptions{job.geometry, false, false}, &job.spans);
    std::array<uint32_t, MAX_WIDTH> argb_scratch;
    for (unsigned row = first; row < last; ++row)
        compose_row(job, geometry::FIRST_LINE + row, argb_scratch.data());
}
} // namespace f3rt
