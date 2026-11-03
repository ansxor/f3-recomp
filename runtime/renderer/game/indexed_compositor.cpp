#include "renderer/game/indexed_compositor.hpp"
#include "renderer/game/clip.hpp"
#include "renderer/game/mix.hpp"
#include "renderer/game/row_bits.hpp"
#include "renderer/game/text.hpp"
#include <algorithm>
#include <array>

namespace f3rt {
namespace {
// Widest picture the fixed buffers must hold: native width plus both borders (the indexed
// compositor presents at scale 1 only).
constexpr unsigned MAX_WIDTH = geometry::NATIVE_WIDTH + GameVideoOptions::MAX_BORDER * 2;
// Column bitmask words of the widest row, plus the guard word extract_columns may read past it.
constexpr unsigned MAX_WORDS = MAX_WIDTH / 32 + 1;

int floor_divide(int value, int divisor) {
    return value >= 0 ? value / divisor : -1 - (-1 - value) / divisor;
}

// How a layer's first visible sample treats a pixel: with mode 0/3 and the two weights of the
// selected slot summing to eight, (c*8)>>3 == c per channel, so the sample resolves the pixel by
// itself. `resolves` is indexed by the sample's blend selector; `layer` is the layer's index.
struct LayerPlan {
    std::array<bool, 2> resolves;
    unsigned layer;
};

// A layer's visible column ranges of one row, in picture columns. Entries past `count` are unset.
struct ColumnRanges {
    unsigned count = 0;
    std::array<std::array<int, 2>, 16> range; // [left, right)
};

// What one blended pixel received. A pixel no layer resolves on first sight is blended: every layer
// sample it gets is recorded, and its colour comes from replaying the canonical mix() over those
// samples once per distinct combination (BlendResult), not once per pixel. `seen` holds bit
// `layer * 2 + select` for each sample and bit `18 + layer` for a layer that sampled it twice
// (overlapping clip ranges), which mix() treats as a second visit.
struct BlendSamples {
    uint32_t seen;
    std::array<uint16_t, LAYER_COUNT> index; // the palette index each layer sampled
};

// A palette index source of a blended pixel: a layer's sample (by layer index), the row background,
// or the literal index 0.
constexpr unsigned FROM_ZERO = 0, FROM_BACKGROUND = 0xffff;

// The replayed mix() of one `seen` combination: where the source and destination colours come from
// (layer index + 1, FROM_ZERO or FROM_BACKGROUND) and their weights.
struct BlendResult {
    uint32_t seen; // 0 marks an unused memo entry
    uint16_t source, destination;
    uint8_t source_weight, destination_weight;
};

// One row's working state. A pixel is unresolved until a layer settles it: a layer that resolves on
// first sight writes the final output word, and anything else makes it pending (blended): later
// layers keep sampling it into `samples`, and the end of the row resolves it.
struct RowWork {
    const IndexedComposeJob &job;
    const SceneRow &row;
    unsigned width = 0, words = 0;
    // Sprite plane layout: the expanded plane is one row per picture line at `width` columns; the
    // fixed native plane is 432 columns indexed by the raw scanline and only 46..365 are visible.
    unsigned plane_stride = 0, plane_row = 0;
    int left_edge = 0, right_edge = 0, border = 0;
    bool expanded = false, flipped = false;
    uint32_t *out = nullptr;
    const uint32_t *lut = nullptr; // index -> output word of the row's palette width
    std::array<BlendSamples, MAX_WIDTH> *samples = nullptr;
    std::array<uint32_t, MAX_WORDS> unresolved, pending;
    bool any_pending = false;
    // The active layers in composition order, for replaying mix().
    std::array<LayerId, LAYER_COUNT> order;
    unsigned order_count = 0;
    std::array<BlendResult, 64> memo; // open-addressed by `seen`; filled when a pixel first blends

    RowWork(const IndexedComposeJob &job, const SceneRow &row) : job(job), row(row) {}

    void reset() {
        words = (width + 31) / 32;
        for (unsigned w = 0; w < words; ++w) unresolved[w] = 0xffffffffu;
        if (width & 31) unresolved[words - 1] &= (1u << (width & 31)) - 1;
        unresolved[words] = 0;
        std::fill_n(pending.begin(), words + 1, 0u);
        any_pending = false;
    }

    LayerPlan plan(const SceneLayer &layer, LayerId id) const {
        const bool mode = layer.blend_mode == 0 || layer.blend_mode == 3;
        return {
            {mode && row.blend[0] + row.blend[2] == 8, mode && row.blend[1] + row.blend[3] == 8},
            unsigned(id)};
    }

    // Replay mix() over the samples of `seen` in composition order. Colours are stood in for by
    // their source: layer index + 1, so the outcome says which sample ends up as source and
    // destination.
    BlendResult replay(uint32_t seen) const {
        PixelMix pixel = background_mix(FROM_BACKGROUND);
        for (unsigned n = 0; n < order_count; ++n) {
            const unsigned layer = unsigned(order[n]);
            for (unsigned select = 0; select < 2; ++select) {
                if (!(seen >> (layer * 2 + select) & 1)) continue;
                const unsigned visits = 1 + (seen >> (18 + layer) & 1);
                for (unsigned visit = 0; visit < visits; ++visit)
                    mix(pixel, row.layer(order[n]), uint16_t(layer + 1), select != 0, row.blend);
            }
        }
        return {seen, pixel.source, pixel.destination, pixel.source_weight,
                pixel.destination_weight};
    }

    // The replay of `seen`, memoised for the few combinations a row has; a combination that finds
    // its probe window full is replayed without being kept.
    BlendResult blend_result(uint32_t seen) {
        unsigned slot = (seen * 0x9e3779b1u) >> 26;
        for (unsigned probe = 0; probe < 4; ++probe, slot = (slot + 1) & 63) {
            if (memo[slot].seen == seen) return memo[slot];
            if (!memo[slot].seen) return memo[slot] = replay(seen);
        }
        return replay(seen);
    }
};

// Feed one visible sample into the row. Callers only pass pixels that are unresolved or pending, so
// a resolved pixel can never be revisited (the class-A row guarantees it is already final).
[[gnu::always_inline]] inline void visit(RowWork &work, int x, uint16_t palette, bool select,
                                         const LayerPlan &plan) {
    const unsigned word = unsigned(x) >> 5, bit = 1u << (unsigned(x) & 31);
    BlendSamples &sample = (*work.samples)[x];
    const uint32_t seen = 1u << (plan.layer * 2 + unsigned(select));
    if (work.unresolved[word] & bit) {
        work.unresolved[word] &= ~bit;
        if (plan.resolves[unsigned(select)]) {
            work.out[x] = work.lut[palette & 8191];
            return;
        }
        if (!work.any_pending) {
            work.any_pending = true;
            for (auto &entry : work.memo) entry.seen = 0;
        }
        work.pending[word] |= bit;
        sample.seen = seen;
    } else if (work.pending[word] & bit) {
        // A layer sampling a pixel it already sampled (the same select) is a second visit.
        sample.seen |= sample.seen & seen ? 1u << (18 + plan.layer) : seen;
    } else {
        return;
    }
    sample.index[plan.layer] = palette;
}

// Calls body(word, window) for every 32-column word of [left, right); `window` selects the range's
// columns inside the word.
template <class Body>
[[gnu::always_inline]] inline void for_each_range_word(int left, int right, Body &&body) {
    if (left >= right) return;
    const unsigned first_word = unsigned(left) >> 5, last_word = unsigned(right - 1) >> 5;
    for (unsigned w = first_word; w <= last_word; ++w) {
        uint32_t window = 0xffffffffu;
        if (w == first_word) window &= 0xffffffffu << (unsigned(left) & 31);
        if (w == last_word && (unsigned(right) & 31)) window &= (1u << (unsigned(right) & 31)) - 1;
        body(w, window);
    }
}

// Calls body(x) for every unresolved or pending column of [left, right), skipping resolved spans.
template <class Body>
[[gnu::always_inline]] inline void for_each_visit(const RowWork &work, int left, int right,
                                                  Body &&body) {
    for_each_range_word(left, right, [&](unsigned w, uint32_t window) {
        for (uint32_t mask = (work.unresolved[w] | work.pending[w]) & window; mask;
             mask &= mask - 1)
            body(int(w * 32 + unsigned(__builtin_ctz(mask))));
    });
}

// Columns of [x, x + len) that still need a sample: unresolved, or pending a real blend.
[[gnu::always_inline]] inline uint32_t open_columns(const RowWork &work, int x, unsigned len) {
    const uint32_t open = extract_columns(work.unresolved.data(), x, len);
    return work.any_pending ? open | extract_columns(work.pending.data(), x, len) : open;
}

// The first unresolved or pending column in [x, right), or `right` when there is none. x < right.
[[gnu::always_inline]] inline int next_open_column(const RowWork &work, int x, int right) {
    unsigned w = unsigned(x) >> 5;
    const unsigned last = unsigned(right - 1) >> 5;
    uint32_t bits = (work.unresolved[w] | work.pending[w]) & (0xffffffffu << (unsigned(x) & 31));
    while (!bits) {
        if (++w > last) return right;
        bits = work.unresolved[w] | work.pending[w];
    }
    return std::min(right, int(w * 32 + unsigned(__builtin_ctz(bits))));
}

ColumnRanges column_ranges(const RowWork &work, const SceneLayer &layer) {
    ColumnRanges result;
    if (!layer.clip_enabled) {
        result.count = 1;
        result.range[0] = {0, int(work.width)};
        return result;
    }
    const ClipRanges clips =
        clip_ranges(work.row, layer, int16_t(work.left_edge), int16_t(work.right_edge));
    result.count = clips.count;
    for (unsigned r = 0; r < clips.count; ++r)
        result.range[r] = {std::max(work.left_edge, int(clips.ranges[r].left)) - work.left_edge,
                           std::min(work.right_edge, int(clips.ranges[r].right)) - work.left_edge};
    return result;
}

// The playfield row a layer samples. The canonical native kernel never applies the presented
// (full-resolution alt map) transform.
ScenePlayfield playfield_row(const RowWork &work, LayerId id) {
    const ScenePlayfield &pf = work.row.playfields[sub_index(id)];
    return work.expanded && work.job.scene.presented ? presented_playfield(pf) : pf;
}

// Mosaic (any layer kind) samples per column, exactly like the canonical row kernel's generic path.
[[gnu::noinline]] void compose_mosaic_layer(RowWork &work, LayerId id, const SceneLayer &layer,
                                            const ColumnRanges &ranges) {
    const LayerPlan plan = work.plan(layer, id);
    const LayerKind layer_kind = kind(id);
    ScenePlayfield pf;
    GameTiles::RowSampler sampler;
    if (layer_kind == LayerKind::Playfield) {
        pf = playfield_row(work, id);
        // Scale 1 samples the native subpixel phase: source_y is the playfield row, y_step unused.
        sampler = work.job.scene.tiles.row_sampler(sub_index(id), pf.source_y, work.flipped,
                                                   work.job.scene.tile_pixels, pf.alt_map);
    }
    const uint16_t *plane = work.job.plane.data() + size_t(work.plane_row) * work.plane_stride;
    // The sprite plane holds meaningful texels only where the layer's coverage has a bit.
    const uint32_t *covered = kind(id) == LayerKind::Sprite
                                  ? work.job.coverage.row(work.plane_row, sub_index(id)).data()
                                  : nullptr;
    for (unsigned r = 0; r < ranges.count; ++r) {
        for_each_visit(work, ranges.range[r][0], ranges.range[r][1], [&](int x) {
            const int hardware_x = work.left_edge + x;
            const int phase = hardware_x + 68;
            const int count = phase < 0 ? phase + 432 : phase >= 432 ? phase - 432 : phase;
            const int sample_x = hardware_x - count % work.row.mosaic_period;
            ScenePixel source;
            bool select = layer.blend_select;
            if (layer_kind == LayerKind::Playfield) {
                const int source_x = floor_divide(pf.source_x + (sample_x - 46) * pf.x_step, 256);
                source = sampler.pixel(source_x);
                select = (source.flags & 1) != 0;
                if (!(source.flags & 0x10)) return;
                source.palette = uint16_t(source.palette + pf.palette_add);
            } else if (layer_kind == LayerKind::Sprite) {
                // Native plane columns are hardware scanout columns; expanded ones are picture
                // columns (the border shifted out).
                const int source_x = work.expanded ? sample_x - work.left_edge : sample_x;
                const int column = work.expanded ? source_x : source_x - work.left_edge;
                if (column < 0 || column >= int(work.width) ||
                    !(covered[column >> 5] >> (column & 31) & 1))
                    return;
                source.palette = plane[source_x];
            } else {
                const int source_x = work.row.text_x + sample_x - 46;
                source = work.job.scene.text.pixel(source_x, work.row.text_y, work.flipped);
                if (!(source.flags & 0x10)) return;
            }
            visit(work, x, source.palette, select, plan);
        });
    }
}

// The cell of a playfield map row that a source texel column falls in, decoded once and reused for
// the texel columns that follow it.
struct PlayfieldCell {
    const uint8_t *pens;      // the 16 pens of the cell's tile row, before flip
    uint16_t palette;         // palette base of the cell: code * 16 plus the row's palette add
    uint8_t pen_mask, flip_x; // flip_x is 15 or 0
    bool select;
};

[[gnu::always_inline]] inline PlayfieldCell decode_cell(uint32_t word, unsigned texel_y,
                                                        std::span<const uint8_t> tile_pixels,
                                                        uint32_t tile_count, uint16_t palette_add) {
    const uint16_t attributes = uint16_t(word >> 16);
    const uint32_t tile = wrap_tile_index(uint16_t(word), tile_count);
    const unsigned tile_y = texel_y ^ ((attributes & 0x8000) ? 15 : 0);
    return {tile_pixels.data() + size_t(tile) * 256 + size_t(tile_y) * 16,
            uint16_t((attributes & 0x1ff) * 16 + palette_add),
            uint8_t((((attributes >> 10) & 3 & ~attributes) << 4) | 15),
            uint8_t((attributes & 0x4000) ? 15 : 0), (attributes & 0x200) != 0};
}

// A zoomed playfield row (x_step != 256) samples one source texel per output column.
[[gnu::noinline]] void compose_zoomed_playfield(RowWork &work, LayerId id, const SceneLayer &layer,
                                                const ColumnRanges &ranges,
                                                const ScenePlayfield &pf) {
    const LayerPlan plan = work.plan(layer, id);
    const auto cells =
        work.job.scene.tiles.cells(GameTiles::physical_map(sub_index(id), pf.alt_map));
    const auto tile_pixels = work.job.scene.tile_pixels;
    const uint32_t tile_count = uint32_t(tile_pixels.size() / 256);
    constexpr unsigned MAP_WIDTH = GameTiles::MAP_WIDTH_PX;
    const unsigned screen_flip = work.flipped ? MAP_WIDTH - 1 : 0;
    const unsigned wrapped_y = (unsigned(pf.source_y) & 511) ^ (work.flipped ? 511 : 0);
    const uint32_t *map_row = cells.data() + size_t(wrapped_y / 16) * GameTiles::COLUMNS;
    const unsigned texel_y = wrapped_y & 15;
    // Output column x samples source texel floor((source_x + (x + left_edge - 46) * x_step) / 256).
    const int origin = pf.source_x + (work.left_edge - 46) * pf.x_step;
    // A magnified row repeats a source texel over several columns: its sample is kept until the
    // texel changes.
    int texel = INT32_MIN;
    unsigned column = GameTiles::COLUMNS; // no cell decoded yet
    PlayfieldCell cell{};
    unsigned pen = 0;
    uint32_t color = 0; // the texel's output word, for a pixel it resolves
    bool resolves = false;
    for (unsigned r = 0; r < ranges.count; ++r)
        for_each_range_word(
            ranges.range[r][0], ranges.range[r][1], [&](unsigned w, uint32_t window) {
                const uint32_t unresolved = work.unresolved[w];
                uint32_t settled = 0;
                const auto sample = [&](unsigned i, int source) __attribute__((always_inline)) {
                    if (source != texel) {
                        texel = source;
                        const unsigned wrapped = (unsigned(source) & (MAP_WIDTH - 1)) ^ screen_flip;
                        if ((wrapped >> 4) != column) {
                            column = wrapped >> 4;
                            cell = decode_cell(map_row[column], texel_y, tile_pixels, tile_count,
                                               pf.palette_add);
                        }
                        pen = cell.pens[(wrapped & 15) ^ cell.flip_x] & cell.pen_mask;
                        resolves = plan.resolves[cell.select];
                        color = work.lut[uint16_t(cell.palette + pen) & 8191];
                    }
                    if (!pen) return;
                    const int x = int(w * 32 + i);
                    if (resolves && (unresolved >> i & 1)) {
                        work.out[x] = color;
                        settled |= 1u << i;
                    } else {
                        visit(work, x, uint16_t(cell.palette + pen), cell.select, plan);
                    }
                };
                if ((unresolved & window) == window && !(work.pending[w] & window)) {
                    // Every column of the window is open: walk its source texels without the mask.
                    const unsigned first = unsigned(__builtin_ctz(window));
                    const unsigned end = 32 - unsigned(__builtin_clz(window));
                    int source = origin + int(w * 32 + first) * pf.x_step;
                    for (unsigned i = first; i < end; ++i, source += pf.x_step)
                        sample(i, source >> 8);
                } else {
                    for (uint32_t mask = (unresolved | work.pending[w]) & window; mask;
                         mask &= mask - 1) {
                        const unsigned i = unsigned(__builtin_ctz(mask));
                        sample(i, (origin + int(w * 32 + i) * pf.x_step) >> 8);
                    }
                }
                work.unresolved[w] &= ~settled;
            });
}

// out[i] = palette[pen i] for the 16 pens of a tile row, mirrored when `mirrored`.
[[gnu::always_inline]] inline void write_cell(uint32_t *out, const uint32_t *palette,
                                              const PenRow &pens, bool mirrored) {
    for (unsigned group = 0; group < 4; ++group) {
        const uint32_t four = pens.group(group, mirrored);
        uint32_t *pixel = out + 4 * group;
        pixel[0] = palette[four & 0xff];
        pixel[1] = palette[(four >> 8) & 0xff];
        pixel[2] = palette[(four >> 16) & 0xff];
        pixel[3] = palette[four >> 24];
    }
}

// The playfield run kernel: x_step == 256 advances one texel per output column, so each map cell is
// one run of at most 16 columns. A run walks bitmasks: the columns still open, intersected with the
// cell row's opaque texels, and the unresolved ones that resolve on first sight are written directly.
[[gnu::noinline]] void compose_playfield(RowWork &work, LayerId id, const SceneLayer &layer,
                                         const ColumnRanges &ranges) {
    const ScenePlayfield pf = playfield_row(work, id);
    if (pf.x_step != 256) {
        compose_zoomed_playfield(work, id, layer, ranges, pf);
        return;
    }
    const LayerPlan plan = work.plan(layer, id);
    const auto cells =
        work.job.scene.tiles.cells(GameTiles::physical_map(sub_index(id), pf.alt_map));
    const auto tile_pixels = work.job.scene.tile_pixels;
    const uint32_t tile_count = uint32_t(tile_pixels.size() / 256);
    constexpr unsigned MAP_WIDTH = GameTiles::MAP_WIDTH_PX;
    const unsigned screen_flip = work.flipped ? MAP_WIDTH - 1 : 0;
    const int base = floor_divide(pf.source_x, 256);
    const unsigned wrapped_y = (unsigned(pf.source_y) & 511) ^ (work.flipped ? 511 : 0);
    const uint32_t *map_row = cells.data() + size_t(wrapped_y / 16) * GameTiles::COLUMNS;
    const unsigned texel_y = wrapped_y & 15;
    for (unsigned r = 0; r < ranges.count; ++r) {
        const int left = ranges.range[r][0], right = ranges.range[r][1];
        int x = left;
        while (x < right) {
            const unsigned wrapped =
                (unsigned(base + x - work.border) & (MAP_WIDTH - 1)) ^ screen_flip;
            const unsigned texel = wrapped & 15;
            const unsigned run =
                std::min<unsigned>(work.flipped ? texel + 1 : 16 - texel, unsigned(right - x));
            // Columns of the run that no earlier layer has settled; a settled run is skipped
            // without touching its map cell.
            const uint32_t unresolved = extract_columns(work.unresolved.data(), x, run);
            const uint32_t open = work.any_pending
                                      ? unresolved | extract_columns(work.pending.data(), x, run)
                                      : unresolved;
            if (!open) {
                // Nothing here is open: jump to the next open column instead of testing each cell.
                x += int(run);
                if (x < right) x = next_open_column(work, x, right);
                continue;
            }
            const PlayfieldCell cell = decode_cell(map_row[wrapped >> 4], texel_y, tile_pixels,
                                                   tile_count, pf.palette_add);
            const uint32_t *palette = work.lut + (cell.palette & 8191);
            // Column i of the run shows texel (texel -/+ i) ^ flip_x of the row, so the row is
            // mirrored when exactly one of the screen flip and the cell flip applies.
            const bool mirrored = work.flipped != (cell.flip_x != 0);
            const PenRow pens(cell.pens, cell.pen_mask);
            if (open == 0xffffu && unresolved == 0xffffu && plan.resolves[cell.select] &&
                run == 16 && pens.all_opaque()) {
                // A whole cell no earlier layer touched and with no transparent texel: all 16
                // columns are final as they are.
                write_cell(work.out + x, palette, pens, mirrored);
                clear_columns(work.unresolved.data(), x, 0xffffu);
                x += 16;
                continue;
            }
            const uint32_t live =
                open & (pens.opaque(mirrored) >> (work.flipped ? 15 - texel : texel));
            if (live) {
                const uint8_t *source = cell.pens + (texel ^ cell.flip_x);
                const int step = mirrored ? -1 : 1;
                uint32_t rest = live;
                if (plan.resolves[cell.select]) {
                    // Unresolved opaque columns are final as they are: write them, settle them with
                    // one mask operation, and leave only the pending ones to the blend.
                    const uint32_t fast = live & unresolved;
                    for_each_run(fast, [&](unsigned first, unsigned length) {
                        const uint8_t *pen = source + int(first) * step;
                        uint32_t *pixel = work.out + x + int(first);
                        for (unsigned i = 0; i < length; ++i, pen += step)
                            pixel[i] = palette[*pen & cell.pen_mask];
                    });
                    clear_columns(work.unresolved.data(), x, fast);
                    rest &= ~fast;
                }
                for (; rest; rest &= rest - 1) {
                    const unsigned i = unsigned(__builtin_ctz(rest));
                    const unsigned pen = source[int(i) * step] & cell.pen_mask;
                    visit(work, x + int(i), uint16_t(cell.palette + pen), cell.select, plan);
                }
            }
            x += int(run);
        }
    }
}

// Text: walks one glyph cell at a time, mirrored when the frame is flipped exactly like
// GameText::pixel. A cell whose glyph row is blank, or none of whose columns is open, costs no pixel
// work.
[[gnu::noinline]] void compose_text(RowWork &work, const SceneLayer &layer,
                                    const ColumnRanges &ranges) {
    constexpr LayerId id = LayerId::Text;
    const auto &text = work.job.scene.text;
    const LayerPlan plan = work.plan(layer, id);
    const bool resolves = plan.resolves[layer.blend_select];
    int y = work.row.text_y & 511;
    if (work.flipped) y = 511 - y;
    const uint16_t *map = text.map().data();
    const uint8_t *glyphs = text.glyphs().data();
    const uint8_t *glyph_rows = work.job.glyph_rows.data();
    const unsigned row_base = unsigned(y / 8) * 64;
    const unsigned texel_y = unsigned(y & 7);
    for (unsigned r = 0; r < ranges.count; ++r) {
        const int left = ranges.range[r][0], right = ranges.range[r][1];
        int x = left;
        while (x < right) {
            // Text column u of the 512-wide map; a cell spans eight consecutive u.
            const unsigned u = unsigned(work.row.text_x + x - work.border) & 511;
            const unsigned phase = u & 7;
            const unsigned len = std::min(8 - phase, unsigned(right - x));
            const unsigned cell = work.flipped ? 63 - (u >> 3) : u >> 3;
            const uint16_t word = map[row_base + cell];
            const unsigned flip_y = (word & 0x8000) ? 7u : 0u;
            const unsigned glyph_y = texel_y ^ flip_y;
            const uint32_t open =
                (glyph_rows[word & 0xff] >> glyph_y & 1) ? open_columns(work, x, len) : 0;
            if (open) {
                const unsigned flip_x = (word & 0x0100) ? 7u : 0u;
                const uint8_t *glyph = glyphs + size_t(word & 0xff) * 32 + size_t(glyph_y * 4);
                // The row's eight 4-bit texels as one word: texel t is nibble t.
                const uint32_t texels = uint32_t(glyph[0]) << 24 | uint32_t(glyph[1]) << 16 |
                                        uint32_t(glyph[2]) << 8 | glyph[3];
                const uint16_t palette_base = uint16_t(((word >> 9) & 0x3f) * 16);
                const uint32_t direct =
                    resolves ? open & extract_columns(work.unresolved.data(), x, len) : 0;
                uint32_t settled = 0;
                for (uint32_t bits = open; bits; bits &= bits - 1) {
                    const unsigned i = unsigned(__builtin_ctz(bits));
                    const unsigned column = work.flipped ? 7 - (phase + i) : phase + i;
                    const unsigned pen = (texels >> (((column ^ flip_x)) * 4)) & 0x0f;
                    if (!pen) continue;
                    if (direct >> i & 1) {
                        work.out[x + int(i)] = work.lut[palette_base + pen];
                        settled |= 1u << i;
                    } else {
                        visit(work, x + int(i), uint16_t(palette_base + pen), layer.blend_select,
                              plan);
                    }
                }
                clear_columns(work.unresolved.data(), x, settled);
            }
            x += int(len);
        }
    }
}

// Sprites: the coverage bits name exactly the open columns whose plane value belongs to this layer.
[[gnu::noinline]] void compose_sprites(RowWork &work, LayerId id, const SceneLayer &layer,
                                       const ColumnRanges &ranges) {
    const unsigned index = sub_index(id);
    const SpriteCoverage &coverage = work.job.coverage;
    if (!(coverage.layers[work.plane_row] & (1u << index))) return; // no plane value of this layer
    const uint32_t *covered = coverage.row(work.plane_row, index).data();
    // The native plane is indexed by scanout column, which keeps its 46-column left margin; the
    // row's output is indexed by picture column.
    const uint16_t *plane = work.job.plane.data() + size_t(work.plane_row) * work.plane_stride +
                            (work.expanded ? 0 : work.left_edge);
    const LayerPlan plan = work.plan(layer, id);
    const bool resolves = plan.resolves[layer.blend_select];
    for (unsigned r = 0; r < ranges.count; ++r)
        for_each_range_word(ranges.range[r][0], ranges.range[r][1],
                            [&](unsigned w, uint32_t window) {
                                uint32_t pending = covered[w] & work.pending[w] & window;
                                uint32_t direct = covered[w] & work.unresolved[w] & window;
                                if (resolves) {
                                    work.unresolved[w] &= ~direct;
                                    for_each_run(direct, [&](unsigned first, unsigned length) {
                                        const unsigned x = w * 32 + first;
                                        for (unsigned i = 0; i < length; ++i)
                                            work.out[x + i] = work.lut[plane[x + i]];
                                    });
                                } else {
                                    pending |= direct;
                                }
                                for (; pending; pending &= pending - 1) {
                                    const unsigned x = w * 32 + unsigned(__builtin_ctz(pending));
                                    visit(work, int(x), plane[x], layer.blend_select, plan);
                                }
                            });
}

// Resolve a blended pixel to the canonical 0xAARRGGBB word: rgb() of mix.hpp over the colours its
// replayed mix() picked, with the red and blue channels blended in one multiply. A channel is at
// most 255 and the two weights add up to at most 16, so a sum of at most 4080 stays inside its
// 16-bit lane.
template <bool Palette15>
[[gnu::always_inline]] inline uint32_t
resolve_blend(const BlendResult &result, const BlendSamples &sample, uint16_t background,
              std::span<const uint32_t> palette) {
    const auto fetch = [&](uint16_t from) {
        const uint16_t index = from == FROM_BACKGROUND ? background
                               : from == FROM_ZERO     ? uint16_t(0)
                                                       : sample.index[from - 1u];
        const uint32_t color = palette[index & 8191];
        return Palette15 ? color15(color) : color;
    };
    if (result.source_weight == 8 && result.destination_weight == 0)
        return 0xff000000u | fetch(result.source);
    if (result.source_weight == 0 && result.destination_weight == 8)
        return 0xff000000u | fetch(result.destination);
    const uint32_t source = fetch(result.source), destination = fetch(result.destination);
    const uint32_t blue_red = ((source & 0x00ff00ffu) * result.source_weight +
                               (destination & 0x00ff00ffu) * result.destination_weight) >>
                                  3 &
                              0x01ff01ffu;
    const uint32_t green = (((source >> 8) & 0xffu) * result.source_weight +
                            ((destination >> 8) & 0xffu) * result.destination_weight) >>
                           3;
    // Clamp each lane to 255: a lane at 256 or more sets bit 8.
    const uint32_t over = (blue_red >> 8) & 0x00010001u;
    return 0xff000000u | ((blue_red | over * 0xffu) & 0x00ff00ffu) |
           std::min<uint32_t>(green, 255u) << 8;
}

constexpr uint32_t from_pixel_format(uint32_t pixel, PixelFormat format) {
    return format == PixelFormat::Rgba8888 ? (pixel >> 8) | 0xff000000u : pixel;
}

// Zero the texels of plane row `plane_row` (`stride` texels, picture column 0 at texel `left`) that
// no sprite layer covers: the chunk raster leaves them stale.
void clear_uncovered_plane_row(const IndexedComposeJob &job, unsigned plane_row, unsigned stride,
                               unsigned left) {
    uint16_t *plane = job.plane.data() + size_t(plane_row) * stride;
    const unsigned width = job.geometry.width();
    std::array<uint32_t, MAX_WORDS> covered{};
    for (unsigned layers = job.coverage.layers[plane_row]; layers; layers &= layers - 1) {
        const auto bits = job.coverage.row(plane_row, unsigned(__builtin_ctz(layers)));
        for (unsigned w = 0; w < bits.size(); ++w) covered[w] |= bits[w];
    }
    for (unsigned x = 0; x < stride; ++x) {
        const unsigned column = x - left; // wraps for texels left of the picture
        if (column >= width || !(covered[column >> 5] >> (column & 31) & 1)) plane[x] = 0;
    }
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
        if (slot == 0xff) slot = layer.blend_mode;
        else if (slot != layer.blend_mode) return false;
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
    const unsigned sprite_layers = job.coverage.layers[plane_row];
    std::array<bool, LAYER_COUNT> active{};
    for (unsigned i = 0; i < LAYER_COUNT; ++i) {
        const LayerId id{uint8_t(i)};
        const int text_y = flipped ? (511 - row.text_y) : row.text_y;
        const bool empty =
            (kind(id) == LayerKind::Playfield && row.playfields[sub_index(id)].empty_row) ||
            (id == LayerId::Text && !scene.text.row_used(unsigned(text_y))) ||
            (kind(id) == LayerKind::Sprite && !(sprite_layers & (1u << sub_index(id))));
        active[i] = row.layer(id).enabled && (scene.layer_mask & layer_bit(id)) != 0 && !empty;
    }
    if (!row_is_class_a(row, active)) {
        // Not provably first-visible-layer-wins: the canonical row kernel decides every pixel. It
        // reads the plane as a whole, so first zero the texels the raster did not write.
        clear_uncovered_plane_row(job, plane_row, expanded ? width : 432, expanded ? 0 : 46);
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

    std::array<BlendSamples, MAX_WIDTH> samples;
    RowWork work{job, row};
    work.width = width;
    work.expanded = expanded;
    work.plane_stride = expanded ? width : 432;
    work.plane_row = plane_row;
    work.left_edge = 46 - int(job.geometry.border);
    work.right_edge = 366 + int(job.geometry.border);
    work.border = int(job.geometry.border);
    work.flipped = flipped;
    work.out = out;
    work.lut = (row.palette_15bit ? job.palette15 : job.palette).data();
    work.samples = &samples;
    work.reset();

    for (LayerId id : scene_order(row))
        if (active[unsigned(id)]) work.order[work.order_count++] = id;
    for (unsigned n = 0; n < work.order_count; ++n) {
        const LayerId id = work.order[n];
        const auto &layer = row.layer(id);
        const ColumnRanges ranges = column_ranges(work, layer);
        if (layer.mosaic && row.mosaic_period > 1) {
            compose_mosaic_layer(work, id, layer, ranges);
            continue;
        }
        switch (kind(id)) {
        case LayerKind::Playfield:
            compose_playfield(work, id, layer, ranges);
            break;
        case LayerKind::Sprite:
            compose_sprites(work, id, layer, ranges);
            break;
        case LayerKind::Text:
            compose_text(work, layer, ranges);
            break;
        }
    }

    // Columns no layer drew show the background; pending pixels resolve through their blend.
    const uint32_t background = work.lut[row.background & 8191];
    for (unsigned w = 0; w < work.words; ++w) {
        uint32_t mask = work.unresolved[w];
        if (mask == 0xffffffffu) {
            std::fill_n(out + w * 32, 32, background);
            continue;
        }
        for (; mask; mask &= mask - 1) out[w * 32 + unsigned(__builtin_ctz(mask))] = background;
    }
    if (work.any_pending) {
        uint32_t special = 0;
        for (unsigned w = 0; w < work.words; ++w)
            for (uint32_t mask = work.pending[w]; mask; mask &= mask - 1, ++special) {
                const int x = int(w * 32 + unsigned(__builtin_ctz(mask)));
                const BlendSamples &sample = samples[x];
                const BlendResult result = work.blend_result(sample.seen);
                out[x] = to_pixel_format(
                    row.palette_15bit
                        ? resolve_blend<true>(result, sample, row.background, scene.colors)
                        : resolve_blend<false>(result, sample, row.background, scene.colors),
                    job.format);
            }
        if (job.stats && special)
            job.stats->special_px.fetch_add(special, std::memory_order_relaxed);
    }
    if (row.blur) {
        // Blur averages ARGB channels, so blur in ARGB and convert back to the output order.
        for (unsigned x = 0; x < width; ++x)
            argb_scratch[x] = from_pixel_format(out[x], job.format);
        for (int x = int(width) - 1; x >= 0; --x) {
            const uint32_t color = argb_scratch[x], previous = x >= 1 ? argb_scratch[x - 1] : 0;
            const uint32_t rb =
                (((color & 0x00ff00ffu) + (previous & 0x00ff00ffu)) >> 1) & 0x00ff00ffu;
            const uint32_t g =
                (((color & 0x0000ff00u) + (previous & 0x0000ff00u)) >> 1) & 0x0000ff00u;
            argb_scratch[x] = 0xff000000u | rb | g;
        }
        for (unsigned x = 0; x < width; ++x) out[x] = to_pixel_format(argb_scratch[x], job.format);
    }
}
} // namespace

void IndexedPalette::build(std::span<const uint32_t> palette, PixelFormat format, bool need_15bit) {
    for (unsigned i = 0; i < SIZE; ++i) {
        colors[i] = to_pixel_format(0xff000000u | (palette[i] & 0xffffffu), format);
        if (need_15bit) colors15[i] = to_pixel_format(0xff000000u | color15(palette[i]), format);
    }
    // The guard entries repeat the table's first ones: an index may run past the end by up to a
    // pen (or a sprite's palette offset) before it wraps.
    std::copy_n(colors.begin(), GUARD, colors.begin() + SIZE);
    if (need_15bit) std::copy_n(colors15.begin(), GUARD, colors15.begin() + SIZE);
    uses_15bit = need_15bit;
}

void IndexedScratch::configure(GameVideoOptions geometry) {
    const size_t plane_size = sprite_plane_size(geometry);
    if (plane.size() != plane_size) plane.assign(plane_size, 0);
    const size_t rows = geometry.expanded() ? geometry.height() : size_t(256);
    if (coverage_layers.size() != rows) coverage_layers.assign(rows, 0);
    const size_t bits = rows * SpriteCoverage::LAYERS * SpriteCoverage::words_for(geometry);
    if (coverage_bits.size() != bits) coverage_bits.assign(bits, 0);
}

bool indexed_frame_uses_15bit(const FrameScene &scene) noexcept {
    for (unsigned y = geometry::FIRST_LINE; y < geometry::END_LINE; ++y)
        if (scene.rows[y & 255].palette_15bit) return true;
    return false;
}

void indexed_glyph_rows(const FrameScene &scene, std::array<uint8_t, 256> &rows) noexcept {
    const auto glyphs = scene.text.glyphs();
    for (unsigned glyph = 0; glyph < rows.size(); ++glyph) {
        const uint8_t *data = glyphs.data() + size_t(glyph) * 32;
        uint8_t used = 0;
        for (unsigned y = 0; y < 8; ++y) {
            // A glyph row is four bytes: one nonzero byte makes it used.
            used |=
                uint8_t((data[y * 4] | data[y * 4 + 1] | data[y * 4 + 2] | data[y * 4 + 3]) != 0)
                << y;
        }
        rows[glyph] = used;
    }
}

void compose_indexed_chunk(IndexedComposeJob &job, unsigned chunk) noexcept {
    const unsigned rows = job.geometry.height();
    const unsigned first = chunk * COMPOSE_CHUNK_ROWS;
    const unsigned last = std::min(first + COMPOSE_CHUNK_ROWS, rows);
    if (first >= last) return;
    // raster_captured_sprites() rasterises the captured list without the flipscreen row phase (the
    // latched positions are already in scanout space), so the fused raster matches it exactly.
    raster_sprite_chunk(*job.bins, job.sprites, chunk, job.pen_mask, job.sprite_tiles, job.plane,
                        SpriteRasterOptions{job.geometry, false, false}, job.coverage);
    std::array<uint32_t, MAX_WIDTH> argb_scratch;
    for (unsigned row = first; row < last; ++row)
        compose_row(job, geometry::FIRST_LINE + row, argb_scratch.data());
}
} // namespace f3rt
