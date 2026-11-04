#include "renderer/game/indexed_compositor.hpp"
#include "renderer/game/clip.hpp"
#include "renderer/game/mix.hpp"
#include "renderer/game/row_bits.hpp"
#include "renderer/game/text.hpp"
#include <algorithm>
#include <array>
#include <climits>
#include <cstring>

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
// itself, provided no later layer can still change it (RowSetup::settle). `resolves` is indexed by
// the sample's blend selector; `layer` is the layer's index.
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
// (overlapping clip ranges), which mix() treats as a second visit. `index` is the palette index
// each layer sampled; the two entries after the layers hold the literal index 0 and the row
// background, so a blend picks its source and destination colours by slot alone.
constexpr unsigned SLOT_ZERO = LAYER_COUNT, SLOT_BACKGROUND = LAYER_COUNT + 1;
struct BlendSamples {
    uint32_t seen;
    std::array<uint16_t, LAYER_COUNT + 2> index;
};

// The stand-in a replayed mix() colour carries: the sampling layer's index + 1, so the outcome says
// which sample ends up as source and destination, FROM_ZERO for the literal index 0 a priority tie
// puts in the destination, and FROM_BACKGROUND for the row background.
constexpr unsigned FROM_ZERO = 0, FROM_BACKGROUND = 0xffff;

// The replayed mix() of one `seen` combination: the sample slots its source and destination colours
// come from, and their weights.
struct BlendResult {
    uint32_t seen; // 0 marks an unused memo entry
    uint8_t source, destination;
    uint8_t source_weight, destination_weight;
};

constexpr uint8_t sample_slot(uint16_t from) {
    return from == FROM_ZERO ? SLOT_ZERO : from == FROM_BACKGROUND ? SLOT_BACKGROUND
                                                                    : uint8_t(from - 1);
}

// What a row's layers need that depends on its line state alone, not on its scroll position or
// pixels: the composition order, which layers may settle a pixel, and each layer's column ranges.
// Neighbouring rows mostly share a line state, so a chunk keeps the last one and rebuilds it only
// when the key changes.
struct RowSetup {
    // The key: every layer's state, the clip windows, and the layers active on the row.
    std::array<SceneLayer, LAYER_COUNT> layers;
    std::array<SceneClip, 4> clips;
    unsigned active = 0; // bit per LayerId
    bool valid = false;
    // Derived from the key.
    std::array<LayerId, LAYER_COUNT> order; // active layers in composition order
    unsigned order_count = 0;
    unsigned settle = 0; // bit per LayerId: the layer may settle a pixel by its first sample
    // True when a layer's priority is outside what the hardware encodes (4 bits): the canonical row
    // kernel decides such a row.
    bool canonical = false;
    std::array<ColumnRanges, LAYER_COUNT> ranges;
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
    const RowSetup &setup; // the active layers in composition order, for replaying mix()
    std::array<BlendResult, 64> memo; // open-addressed by `seen`; filled when a pixel first blends

    RowWork(const IndexedComposeJob &job, const SceneRow &row, const RowSetup &setup)
        : job(job), row(row), setup(setup) {}

    void reset() {
        words = (width + 31) / 32;
        for (unsigned w = 0; w < words; ++w) unresolved[w] = 0xffffffffu;
        if (width & 31) unresolved[words - 1] &= (1u << (width & 31)) - 1;
        unresolved[words] = 0;
        std::fill_n(pending.begin(), words + 1, 0u);
        any_pending = false;
    }

    LayerPlan plan(const SceneLayer &layer, LayerId id) const {
        const bool mode = (layer.blend_mode == 0 || layer.blend_mode == 3) &&
                          (setup.settle >> unsigned(id) & 1);
        return {
            {mode && row.blend[0] + row.blend[2] == 8, mode && row.blend[1] + row.blend[3] == 8},
            unsigned(id)};
    }

    // Replay mix() over the samples of `seen` in composition order. Colours are stood in for by
    // their source (see FROM_ZERO), so the outcome says which sample ends up as source and
    // destination.
    BlendResult replay(uint32_t seen) const {
        PixelMix pixel = background_mix(FROM_BACKGROUND);
        for (unsigned n = 0; n < setup.order_count; ++n) {
            const unsigned layer = unsigned(setup.order[n]);
            for (unsigned select = 0; select < 2; ++select) {
                if (!(seen >> (layer * 2 + select) & 1)) continue;
                const unsigned visits = 1 + (seen >> (18 + layer) & 1);
                for (unsigned visit = 0; visit < visits; ++visit)
                    mix(pixel, row.layer(setup.order[n]), uint16_t(layer + 1), select != 0,
                        row.blend);
            }
        }
        return {seen, sample_slot(pixel.source), sample_slot(pixel.destination),
                pixel.source_weight, pixel.destination_weight};
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
// a resolved pixel is never revisited (RowSetup::settle guarantees it is already final).
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
        sample.index[SLOT_ZERO] = 0;
        sample.index[SLOT_BACKGROUND] = work.row.background;
    } else if (work.pending[word] & bit) {
        // A layer sampling a pixel it already sampled (the same select) is a second visit.
        sample.seen |= sample.seen & seen ? 1u << (18 + plan.layer) : seen;
    } else {
        return;
    }
    sample.index[plan.layer] = palette;
}

// visit() for the open columns `bits` of bitmask word `w` (bit c = column w * 32 + c), none of them
// settled by the layer itself: each becomes pending, or takes one more sample when it already was.
// palette_at(x) is the palette index the layer sampled at column x.
template <class PaletteAt>
[[gnu::always_inline]] inline void visit_bits(RowWork &work, unsigned w, uint32_t bits,
                                              bool select, const LayerPlan &plan,
                                              PaletteAt &&palette_at) {
    const uint32_t seen = 1u << (plan.layer * 2 + unsigned(select));
    const uint32_t again = bits & work.pending[w], fresh = bits & ~again;
    if (fresh) {
        if (!work.any_pending) {
            work.any_pending = true;
            for (auto &entry : work.memo) entry.seen = 0;
        }
        work.unresolved[w] &= ~fresh;
        work.pending[w] |= fresh;
        for (uint32_t mask = fresh; mask; mask &= mask - 1) {
            const unsigned x = w * 32 + unsigned(__builtin_ctz(mask));
            BlendSamples &sample = (*work.samples)[x];
            sample.seen = seen;
            sample.index[SLOT_ZERO] = 0;
            sample.index[SLOT_BACKGROUND] = work.row.background;
            sample.index[plan.layer] = palette_at(x);
        }
    }
    for (uint32_t mask = again; mask; mask &= mask - 1) {
        const unsigned x = w * 32 + unsigned(__builtin_ctz(mask));
        BlendSamples &sample = (*work.samples)[x];
        // A layer sampling a pixel it already sampled (the same select) is a second visit.
        sample.seen |= sample.seen & seen ? 1u << (18 + plan.layer) : seen;
        sample.index[plan.layer] = palette_at(x);
    }
}

// visit_bits() for the open columns `bits` of a run at column x (bit i = column x + i, at most 16
// wide), which can straddle two bitmask words.
template <class PaletteAt>
[[gnu::always_inline]] inline void visit_run(RowWork &work, int x, uint32_t bits, bool select,
                                             const LayerPlan &plan, PaletteAt &&palette_at) {
    const unsigned word = unsigned(x) >> 5, shift = unsigned(x) & 31;
    if (bits << shift) visit_bits(work, word, bits << shift, select, plan, palette_at);
    if (shift > 16 && bits >> (32 - shift))
        visit_bits(work, word + 1, bits >> (32 - shift), select, plan, palette_at);
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

// `layer`'s visible column ranges of a row presented between the scanout columns [left_edge,
// right_edge), in picture columns.
ColumnRanges column_ranges(const SceneRow &row, const SceneLayer &layer, int left_edge,
                           int right_edge) {
    ColumnRanges result;
    if (!layer.clip_enabled) {
        result.count = 1;
        result.range[0] = {0, right_edge - left_edge};
        return result;
    }
    const ClipRanges clips = clip_ranges(row, layer, int16_t(left_edge), int16_t(right_edge));
    result.count = clips.count;
    for (unsigned r = 0; r < clips.count; ++r)
        result.range[r] = {std::max(left_edge, int(clips.ranges[r].left)) - left_edge,
                           std::min(right_edge, int(clips.ranges[r].right)) - left_edge};
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
    uint32_t tile;            // the tile and the pixel row of it the cell shows
    uint8_t tile_y;
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
            uint8_t((attributes & 0x4000) ? 15 : 0), (attributes & 0x200) != 0, tile,
            uint8_t(tile_y)};
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
    const TileRows *tile_rows = work.job.tile_rows.data();
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
            if (!tile_rows[cell.tile].visible(cell.tile_y, cell.pen_mask)) {
                // Under the cell's pen mask this tile row has no pen to draw.
                x += int(run);
                continue;
            }
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
                if (rest)
                    visit_run(work, x, rest, cell.select, plan, [&](unsigned column) {
                        const unsigned pen = source[(int(column) - x) * step] & cell.pen_mask;
                        return uint16_t(cell.palette + pen);
                    });
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
            // The columns of the run no layer has settled, and those open to this layer.
            const bool visible = glyph_rows[word & 0xff] >> glyph_y & 1;
            const uint32_t unresolved =
                visible ? extract_columns(work.unresolved.data(), x, len) : 0;
            const uint32_t open = work.any_pending && visible
                                      ? unresolved | extract_columns(work.pending.data(), x, len)
                                      : unresolved;
            if (open) {
                // Cell column s (0 = left on screen) shows texel s ^ 7 when the cell is mirrored,
                // by the frame flip or the cell's own flip but not both, else texel s.
                const bool mirrored = work.flipped != ((word & 0x0100) != 0);
                const uint32_t pens = glyph_row_pens(
                    glyphs + size_t(word & 0xff) * 32 + size_t(glyph_y * 4), mirrored);
                // The open columns of the run that have a pen to draw, as run bits.
                const uint32_t live = open & (nonzero_nibbles(pens) >> phase);
                const unsigned palette_base = ((word >> 9) & 0x3f) * 16;
                const uint32_t *palette = work.lut + palette_base;
                const uint32_t direct = resolves ? live & unresolved : 0;
                const unsigned shift = phase * 4;
                if (direct == 0xffu) {
                    // A whole cell of unresolved opaque columns: eight stores, no per-column test.
                    uint32_t *pixel = work.out + x;
                    for (unsigned i = 0; i < 8; ++i) pixel[i] = palette[pens >> (i * 4) & 0x0f];
                } else {
                    for (uint32_t bits = direct; bits; bits &= bits - 1) {
                        const unsigned i = unsigned(__builtin_ctz(bits));
                        work.out[x + int(i)] = palette[pens >> (shift + i * 4) & 0x0f];
                    }
                }
                if (direct) clear_columns(work.unresolved.data(), x, direct);
                if (live & ~direct)
                    visit_run(work, x, live & ~direct, layer.blend_select, plan,
                              [&](unsigned column) {
                                  return uint16_t(palette_base +
                                                  (pens >> (shift + (column - x) * 4) & 0x0f));
                              });
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
                                if (pending)
                                    visit_bits(work, w, pending, layer.blend_select, plan,
                                               [&](unsigned x) { return plane[x]; });
                            });
}

constexpr uint32_t from_pixel_format(uint32_t pixel, PixelFormat format) {
    return format == PixelFormat::Rgba8888 ? (pixel >> 8) | 0xff000000u : pixel;
}

// The weighted blend of two palette entries, as rgb() of mix.hpp computes it, with the red and blue
// channels blended in one multiply. A channel is at most 255 and the two weights add up to at most
// 16, so a sum of at most 4080 stays inside its 16-bit lane. Both colours and the result are in
// ARGB order, alpha opaque.
[[gnu::always_inline]] inline uint32_t blend_argb(uint32_t source, uint32_t destination,
                                                  unsigned source_weight,
                                                  unsigned destination_weight) {
    const uint32_t blue_red = ((source & 0x00ff00ffu) * source_weight +
                               (destination & 0x00ff00ffu) * destination_weight) >>
                                  3 &
                              0x01ff01ffu;
    const uint32_t green = (((source >> 8) & 0xffu) * source_weight +
                            ((destination >> 8) & 0xffu) * destination_weight) >>
                           3;
    // Clamp each lane to 255: a lane at 256 or more sets bit 8.
    const uint32_t over = (blue_red >> 8) & 0x00010001u;
    return 0xff000000u | ((blue_red | over * 0xffu) & 0x00ff00ffu) |
           std::min<uint32_t>(green, 255u) << 8;
}

// Resolve the pending (blended) pixels of a row: rgb() of mix.hpp over the colours the replayed
// mix() of each pixel's samples picked, read from the row's palette table so a 15-bit row needs no
// conversion. A run of pixels with the same samples shares one result. Returns the pixel count.
unsigned resolve_pending(RowWork &work, uint32_t *out) {
    // The table words are in output byte order; the blend works on ARGB words, which for the
    // Rgba8888 order are one right rotation away (the opaque low byte drops out).
    const bool rgba = work.job.format == PixelFormat::Rgba8888;
    const unsigned rotate = rgba ? 8 : 0;
    const uint32_t low = rgba ? 0xffu : 0;
    BlendResult result{};
    uint32_t seen = 0; // no pixel has seen no sample
    unsigned source = UINT_MAX, destination = UINT_MAX; // the palette entries of `color`
    uint32_t color = 0;
    unsigned count = 0;
    for (unsigned w = 0; w < work.words; ++w)
        for (uint32_t mask = work.pending[w]; mask; mask &= mask - 1, ++count) {
            const int x = int(w * 32 + unsigned(__builtin_ctz(mask)));
            const BlendSamples &sample = (*work.samples)[x];
            if (sample.seen != seen) {
                seen = sample.seen;
                result = work.blend_result(seen);
                source = destination = UINT_MAX;
            }
            const unsigned source_index = sample.index[result.source] & 8191u;
            const unsigned destination_index = sample.index[result.destination] & 8191u;
            if (result.source_weight == 8 && result.destination_weight == 0) {
                out[x] = work.lut[source_index];
                continue;
            }
            if (result.source_weight == 0 && result.destination_weight == 8) {
                out[x] = work.lut[destination_index];
                continue;
            }
            if (source_index != source || destination_index != destination) {
                source = source_index;
                destination = destination_index;
                color = (blend_argb(work.lut[source] >> rotate, work.lut[destination] >> rotate,
                                    result.source_weight, result.destination_weight)
                         << rotate) |
                        low;
            }
            out[x] = color;
        }
    return count;
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

// Whether `setup` was built from this line state.
bool setup_matches(const RowSetup &setup, const SceneRow &row, unsigned active) {
    if (!setup.valid || setup.active != active || setup.clips != row.clips) return false;
    for (unsigned i = 0; i < LAYER_COUNT; ++i)
        if (setup.layers[i] != row.layer(LayerId{uint8_t(i)})) return false;
    return true;
}

// Build `setup` for a row presented between the scanout columns [left_edge, right_edge).
//
// A layer's first visible sample settles a pixel when the layer blends in mode 0 or 3 with weights
// that sum to eight (LayerPlan), and nothing later in the composition order can change the pixel
// afterwards. mix() lets a later layer change a pixel that a mode 0/3 layer sourced only when the
// layer has the same priority and a different blend mode: a lower priority neither outranks the
// source nor reaches the destination priority, and the order never puts a higher one later. A layer
// of priority 0 never becomes the source of anything. Every other pixel stays pending and is
// resolved by replaying mix(), which holds for any priorities and modes.
void build_setup(RowSetup &setup, const SceneRow &row, unsigned active, int left_edge,
                 int right_edge) {
    for (unsigned i = 0; i < LAYER_COUNT; ++i) setup.layers[i] = row.layer(LayerId{uint8_t(i)});
    setup.clips = row.clips;
    setup.active = active;
    setup.order_count = 0;
    setup.settle = 0;
    setup.canonical = false;
    for (LayerId id : scene_order(row)) {
        if (!(active >> unsigned(id) & 1)) continue;
        setup.order[setup.order_count++] = id;
        setup.canonical |= row.layer(id).priority >= 16;
        setup.ranges[unsigned(id)] = column_ranges(row, row.layer(id), left_edge, right_edge);
    }
    for (unsigned n = 0; n < setup.order_count; ++n) {
        const SceneLayer &layer = row.layer(setup.order[n]);
        bool tie = false;
        for (unsigned m = n + 1; m < setup.order_count; ++m) {
            const SceneLayer &later = row.layer(setup.order[m]);
            if (later.priority != layer.priority) break;
            tie |= later.blend_mode != layer.blend_mode;
        }
        if (layer.priority && !tie) setup.settle |= 1u << unsigned(setup.order[n]);
    }
    setup.valid = true;
}

void compose_row(const IndexedComposeJob &job, unsigned y, RowSetup &setup,
                 uint32_t *argb_scratch) {
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
    // its line-RAM priority out of the ties that stop a layer from settling pixels.
    const unsigned sprite_layers = job.coverage.layers[plane_row];
    unsigned active = 0;
    for (unsigned i = 0; i < LAYER_COUNT; ++i) {
        const LayerId id{uint8_t(i)};
        const int text_y = flipped ? (511 - row.text_y) : row.text_y;
        const bool empty =
            (kind(id) == LayerKind::Playfield && row.playfields[sub_index(id)].empty_row) ||
            (id == LayerId::Text && !scene.text.row_used(unsigned(text_y))) ||
            (kind(id) == LayerKind::Sprite && !(sprite_layers & (1u << sub_index(id))));
        if (row.layer(id).enabled && (scene.layer_mask & layer_bit(id)) != 0 && !empty)
            active |= 1u << i;
    }
    const int left_edge = 46 - int(job.geometry.border);
    const int right_edge = 366 + int(job.geometry.border);
    if (!setup_matches(setup, row, active)) build_setup(setup, row, active, left_edge, right_edge);
    if (setup.canonical) {
        // A priority the hardware cannot encode: the canonical row kernel decides every pixel. It
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
    RowWork work{job, row, setup};
    work.width = width;
    work.expanded = expanded;
    work.plane_stride = expanded ? width : 432;
    work.plane_row = plane_row;
    work.left_edge = left_edge;
    work.right_edge = right_edge;
    work.border = int(job.geometry.border);
    work.flipped = flipped;
    work.out = out;
    work.lut = (row.palette_15bit ? job.palette15 : job.palette).data();
    work.samples = &samples;
    work.reset();

    for (unsigned n = 0; n < setup.order_count; ++n) {
        const LayerId id = setup.order[n];
        const auto &layer = row.layer(id);
        const ColumnRanges &ranges = setup.ranges[unsigned(id)];
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
        const unsigned special = resolve_pending(work, out);
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
    // A block whose source colours differ from m_source is converted again, and so is every block
    // when the format changes. The 15-bit table is only kept current while a frame wants it; when
    // it was not, all of its blocks are converted.
    const bool rebuild = !m_built || format != m_format;
    if (rebuild) m_built_15bit = false;
    const bool refresh_15bit = need_15bit && !m_built_15bit;
    bool any_changed = false, first_changed = false;
    for (unsigned first = 0; first < SIZE; first += BLOCK) {
        const bool changed = rebuild || std::memcmp(m_source.data() + first, palette.data() + first,
                                                    BLOCK * sizeof(uint32_t)) != 0;
        if (!changed && !refresh_15bit) continue;
        if (changed)
            for (unsigned i = first; i < first + BLOCK; ++i)
                colors[i] = to_pixel_format(0xff000000u | (palette[i] & 0xffffffu), format);
        if (need_15bit)
            for (unsigned i = first; i < first + BLOCK; ++i)
                colors15[i] = to_pixel_format(0xff000000u | color15(palette[i]), format);
        if (changed) {
            std::copy_n(palette.begin() + first, BLOCK, m_source.begin() + first);
            any_changed = true;
            first_changed |= first == 0;
        }
    }
    // The guard entries repeat the table's first ones: an index may run past the end by up to a
    // pen (or a sprite's palette offset) before it wraps.
    if (first_changed) std::copy_n(colors.begin(), GUARD, colors.begin() + SIZE);
    if (need_15bit && (first_changed || refresh_15bit))
        std::copy_n(colors15.begin(), GUARD, colors15.begin() + SIZE);
    m_format = format;
    m_built = true;
    m_built_15bit = need_15bit || (m_built_15bit && !any_changed);
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

void TileRowTable::ensure(std::span<const uint8_t> tile_pixels) {
    if (tile_pixels.data() == m_source && tile_pixels.size() == m_size) return;
    const size_t tile_count = tile_pixels.size() / 256;
    m_tiles.assign(tile_count, TileRows{});
    for (size_t tile = 0; tile < tile_count; ++tile) {
        uint32_t low = 0, plane4 = 0, plane5 = 0;
        for (unsigned y = 0; y < 16; ++y) {
            // A tile row is 16 pen bytes, read as words: the OR of its words holds every pen bit.
            const uint8_t *row = tile_pixels.data() + tile * 256 + y * 16;
            uint32_t words[4];
            std::memcpy(words, row, sizeof words);
            const uint32_t any = words[0] | words[1] | words[2] | words[3];
            low |= uint32_t((any & 0x0f0f0f0fu) != 0) << y;
            plane4 |= uint32_t((any & 0x10101010u) != 0) << y;
            plane5 |= uint32_t((any & 0x20202020u) != 0) << y;
        }
        m_tiles[tile] = {uint16_t(low), uint16_t(plane4), uint16_t(plane5)};
    }
    m_source = tile_pixels.data();
    m_size = tile_pixels.size();
}

namespace {
// True when any presented row of `scene` selects 15-bit palette words.
bool uses_15bit_palette(const FrameScene &scene) noexcept {
    for (unsigned y = geometry::FIRST_LINE; y < geometry::END_LINE; ++y)
        if (scene.rows[y & 255].palette_15bit) return true;
    return false;
}

// Which glyph rows of `scene`'s text are not blank: bit y of entry g is set when row y of glyph g
// has a nonzero texel.
void glyph_row_masks(const FrameScene &scene, std::array<uint8_t, 256> &rows) noexcept {
    const auto glyphs = scene.text.glyphs();
    for (unsigned glyph = 0; glyph < rows.size(); ++glyph) {
        const uint8_t *data = glyphs.data() + size_t(glyph) * 32;
        uint8_t used = 0;
        for (unsigned y = 0; y < 8; ++y) {
            // A glyph row is four bytes: one nonzero byte makes it used.
            uint32_t word;
            std::memcpy(&word, data + y * 4, sizeof word);
            used |= uint8_t(word != 0) << y;
        }
        rows[glyph] = used;
    }
}
} // namespace

bool IndexedScratch::prepare(const FrameScene &scene, PixelFormat format) {
    const bool need_15bit = uses_15bit_palette(scene);
    palette.build(scene.colors, format, need_15bit);
    glyph_row_masks(scene, glyph_rows);
    tile_rows.ensure(scene.tile_pixels);
    return need_15bit;
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
    // The rows of a chunk share what they can of their line state.
    RowSetup setup;
    setup.valid = false;
    for (unsigned row = first; row < last; ++row)
        compose_row(job, geometry::FIRST_LINE + row, setup, argb_scratch.data());
}
} // namespace f3rt
