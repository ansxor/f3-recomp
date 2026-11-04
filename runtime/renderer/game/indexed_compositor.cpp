#include "renderer/game/indexed_compositor.hpp"
#include "renderer/game/clip.hpp"
#include "renderer/game/layer_visits.hpp"
#include "renderer/game/mix.hpp"
#include "renderer/game/row_bits.hpp"
#include "renderer/game/text.hpp"
#include <algorithm>
#include <array>
#include <climits>
#include <cstring>

namespace f3rt {
namespace {
constexpr unsigned MAX_WIDTH = geometry::NATIVE_WIDTH + GameVideoOptions::MAX_BORDER * 2;
constexpr unsigned MAX_WORDS = MAX_WIDTH / 32 + 1;

int floor_divide(int value, int divisor) {
    return value >= 0 ? value / divisor : -1 - (-1 - value) / divisor;
}

struct LayerPlan {
    std::array<bool, 2> resolves;
    unsigned layer;

    LayerPass pass(bool select) const { return LayerVisits::pass(layer, select); }
};

struct ColumnRanges {
    unsigned count = 0;
    std::array<std::array<int, 2>, 16> range;
};

constexpr unsigned SLOT_ZERO = LAYER_COUNT, SLOT_BACKGROUND = LAYER_COUNT + 1;
struct BlendSamples {
    LayerVisits visits;
    std::array<uint16_t, LAYER_COUNT + 2> index;
};

constexpr unsigned FROM_ZERO = 0, FROM_BACKGROUND = 0xffff;

struct BlendResult {
    LayerVisits visits;
    uint8_t source, destination;
    uint8_t source_weight, destination_weight;
};

constexpr uint8_t sample_slot(uint16_t from) {
    return from == FROM_ZERO ? SLOT_ZERO : from == FROM_BACKGROUND ? SLOT_BACKGROUND
                                                                    : uint8_t(from - 1);
}

struct RowSetup {
    std::array<SceneLayer, LAYER_COUNT> layers;
    std::array<SceneClip, 4> clips;
    unsigned active = 0;
    bool valid = false;
    std::array<LayerId, LAYER_COUNT> order;
    unsigned order_count = 0;
    unsigned settle = 0;
    bool canonical = false;
    std::array<ColumnRanges, LAYER_COUNT> ranges;
};

struct RowWork {
    const IndexedComposeJob &job;
    const SceneRow &row;
    unsigned width = 0, words = 0;
    unsigned plane_stride = 0, plane_row = 0;
    int left_edge = 0, right_edge = 0, border = 0;
    bool expanded = false, flipped = false;
    uint32_t *out = nullptr;
    const uint32_t *lut = nullptr;
    std::array<BlendSamples, MAX_WIDTH> *samples = nullptr;
    std::array<uint32_t, MAX_WORDS> unresolved, pending;
    bool any_pending = false;
    const RowSetup &setup;
    std::array<BlendResult, 64> memo;

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

    BlendResult replay(LayerVisits visits) const {
        PixelMix pixel = background_mix(FROM_BACKGROUND);
        for (unsigned n = 0; n < setup.order_count; ++n) {
            const unsigned layer = unsigned(setup.order[n]);
            for (unsigned select = 0; select < 2; ++select) {
                if (!visits.saw(layer, select != 0)) continue;
                const unsigned count = visits.passes(layer);
                for (unsigned visit = 0; visit < count; ++visit)
                    mix(pixel, row.layer(setup.order[n]), uint16_t(layer + 1), select != 0,
                        row.blend);
            }
        }
        return {visits, sample_slot(pixel.source), sample_slot(pixel.destination),
                pixel.source_weight, pixel.destination_weight};
    }

    BlendResult blend_result(LayerVisits visits) {
        unsigned slot = visits.hash() >> 26;
        for (unsigned probe = 0; probe < 4; ++probe, slot = (slot + 1) & 63) {
            if (memo[slot].visits == visits) return memo[slot];
            if (memo[slot].visits.empty()) return memo[slot] = replay(visits);
        }
        return replay(visits);
    }
};

[[gnu::always_inline]] inline void visit(RowWork &work, int x, uint16_t palette, bool select,
                                         const LayerPlan &plan) {
    const unsigned word = unsigned(x) >> 5, bit = 1u << (unsigned(x) & 31);
    BlendSamples &sample = (*work.samples)[x];
    const LayerPass pass = plan.pass(select);
    if (work.unresolved[word] & bit) {
        work.unresolved[word] &= ~bit;
        if (plan.resolves[unsigned(select)]) {
            work.out[x] = work.lut[palette & 8191];
            return;
        }
        if (!work.any_pending) {
            work.any_pending = true;
            for (auto &entry : work.memo) entry.visits = {};
        }
        work.pending[word] |= bit;
        sample.visits = LayerVisits(pass);
        sample.index[SLOT_ZERO] = 0;
        sample.index[SLOT_BACKGROUND] = work.row.background;
    } else if (work.pending[word] & bit) {
        sample.visits.add(pass);
    } else {
        return;
    }
    sample.index[plan.layer] = palette;
}

template <class PaletteAt>
[[gnu::always_inline]] inline void visit_bits(RowWork &work, unsigned w, uint32_t bits,
                                              bool select, const LayerPlan &plan,
                                              PaletteAt &&palette_at) {
    const LayerPass pass = plan.pass(select);
    const uint32_t again = bits & work.pending[w], fresh = bits & ~again;
    if (fresh) {
        if (!work.any_pending) {
            work.any_pending = true;
            for (auto &entry : work.memo) entry.visits = {};
        }
        work.unresolved[w] &= ~fresh;
        work.pending[w] |= fresh;
        for (uint32_t mask = fresh; mask; mask &= mask - 1) {
            const unsigned x = w * 32 + unsigned(__builtin_ctz(mask));
            BlendSamples &sample = (*work.samples)[x];
            sample.visits = LayerVisits(pass);
            sample.index[SLOT_ZERO] = 0;
            sample.index[SLOT_BACKGROUND] = work.row.background;
            sample.index[plan.layer] = palette_at(x);
        }
    }
    for (uint32_t mask = again; mask; mask &= mask - 1) {
        const unsigned x = w * 32 + unsigned(__builtin_ctz(mask));
        BlendSamples &sample = (*work.samples)[x];
        sample.visits.add(pass);
        sample.index[plan.layer] = palette_at(x);
    }
}

template <class PaletteAt>
[[gnu::always_inline]] inline void visit_run(RowWork &work, int x, uint32_t bits, bool select,
                                             const LayerPlan &plan, PaletteAt &&palette_at) {
    const unsigned word = unsigned(x) >> 5, shift = unsigned(x) & 31;
    if (bits << shift) visit_bits(work, word, bits << shift, select, plan, palette_at);
    if (shift > 16 && bits >> (32 - shift))
        visit_bits(work, word + 1, bits >> (32 - shift), select, plan, palette_at);
}

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

template <class Body>
[[gnu::always_inline]] inline void for_each_visit(const RowWork &work, int left, int right,
                                                  Body &&body) {
    for_each_range_word(left, right, [&](unsigned w, uint32_t window) {
        for (uint32_t mask = (work.unresolved[w] | work.pending[w]) & window; mask;
             mask &= mask - 1)
            body(int(w * 32 + unsigned(__builtin_ctz(mask))));
    });
}

[[gnu::always_inline]] inline uint32_t open_columns(const RowWork &work, int x, unsigned len) {
    const uint32_t open = extract_columns(work.unresolved.data(), x, len);
    return work.any_pending ? open | extract_columns(work.pending.data(), x, len) : open;
}

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

ScenePlayfield playfield_row(const RowWork &work, LayerId id) {
    const ScenePlayfield &pf = work.row.playfields[sub_index(id)];
    return work.expanded && work.job.scene.presented ? presented_playfield(pf) : pf;
}

[[gnu::noinline]] void compose_mosaic_layer(RowWork &work, LayerId id, const SceneLayer &layer,
                                            const ColumnRanges &ranges) {
    const LayerPlan plan = work.plan(layer, id);
    const LayerKind layer_kind = kind(id);
    ScenePlayfield pf;
    GameTiles::RowSampler sampler;
    if (layer_kind == LayerKind::Playfield) {
        pf = playfield_row(work, id);
        sampler = work.job.scene.tiles.row_sampler(sub_index(id), pf.source_y, work.flipped,
                                                   work.job.scene.tile_pixels, pf.alt_map);
    }
    const uint16_t *plane = work.job.plane.data() + size_t(work.plane_row) * work.plane_stride;
    const uint32_t *covered =
        kind(id) == LayerKind::Sprite
            ? work.job.coverage.row(work.plane_row).columns(sub_index(id)).data()
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

struct PlayfieldCell {
    const uint8_t *pens;
    uint16_t palette;
    uint8_t pen_mask, flip_x;
    bool select;
    uint32_t tile;
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
    const int origin = pf.source_x + (work.left_edge - 46) * pf.x_step;
    int texel = INT32_MIN;
    unsigned column = GameTiles::COLUMNS;
    PlayfieldCell cell{};
    unsigned pen = 0;
    uint32_t color = 0;
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
            const uint32_t unresolved = extract_columns(work.unresolved.data(), x, run);
            const uint32_t open = work.any_pending
                                      ? unresolved | extract_columns(work.pending.data(), x, run)
                                      : unresolved;
            if (!open) {
                x += int(run);
                if (x < right) x = next_open_column(work, x, right);
                continue;
            }
            const PlayfieldCell cell = decode_cell(map_row[wrapped >> 4], texel_y, tile_pixels,
                                                   tile_count, pf.palette_add);
            if (!tile_rows[cell.tile].visible(cell.tile_y, cell.pen_mask)) {
                x += int(run);
                continue;
            }
            const uint32_t *palette = work.lut + (cell.palette & 8191);
            const bool mirrored = work.flipped != (cell.flip_x != 0);
            const PenRow pens(cell.pens, cell.pen_mask);
            if (open == 0xffffu && unresolved == 0xffffu && plan.resolves[cell.select] &&
                run == 16 && pens.all_opaque()) {
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
            const unsigned u = unsigned(work.row.text_x + x - work.border) & 511;
            const unsigned phase = u & 7;
            const unsigned len = std::min(8 - phase, unsigned(right - x));
            const unsigned cell = work.flipped ? 63 - (u >> 3) : u >> 3;
            const uint16_t word = map[row_base + cell];
            const unsigned flip_y = (word & 0x8000) ? 7u : 0u;
            const unsigned glyph_y = texel_y ^ flip_y;
            const bool visible = glyph_rows[word & 0xff] >> glyph_y & 1;
            const uint32_t unresolved =
                visible ? extract_columns(work.unresolved.data(), x, len) : 0;
            const uint32_t open = work.any_pending && visible
                                      ? unresolved | extract_columns(work.pending.data(), x, len)
                                      : unresolved;
            if (open) {
                const bool mirrored = work.flipped != ((word & 0x0100) != 0);
                const uint32_t pens = glyph_row_pens(
                    glyphs + size_t(word & 0xff) * 32 + size_t(glyph_y * 4), mirrored);
                const uint32_t live = open & (nonzero_nibbles(pens) >> phase);
                const unsigned palette_base = ((word >> 9) & 0x3f) * 16;
                const uint32_t *palette = work.lut + palette_base;
                const uint32_t direct = resolves ? live & unresolved : 0;
                const unsigned shift = phase * 4;
                if (direct == 0xffu) {
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

[[gnu::noinline]] void compose_sprites(RowWork &work, LayerId id, const SceneLayer &layer,
                                       const ColumnRanges &ranges) {
    const unsigned index = sub_index(id);
    const SpriteCoverage::Row coverage = work.job.coverage.row(work.plane_row);
    if (!coverage.covers(index)) return;
    const uint32_t *covered = coverage.columns(index).data();
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
    const uint32_t over = (blue_red >> 8) & 0x00010001u;
    return 0xff000000u | ((blue_red | over * 0xffu) & 0x00ff00ffu) |
           std::min<uint32_t>(green, 255u) << 8;
}

unsigned resolve_pending(RowWork &work, uint32_t *out) {
    const bool rgba = work.job.format == PixelFormat::Rgba8888;
    const unsigned rotate = rgba ? 8 : 0;
    const uint32_t low = rgba ? 0xffu : 0;
    BlendResult result{};
    LayerVisits visits;
    unsigned source = UINT_MAX, destination = UINT_MAX;
    uint32_t color = 0;
    unsigned count = 0;
    for (unsigned w = 0; w < work.words; ++w)
        for (uint32_t mask = work.pending[w]; mask; mask &= mask - 1, ++count) {
            const int x = int(w * 32 + unsigned(__builtin_ctz(mask)));
            const BlendSamples &sample = (*work.samples)[x];
            if (sample.visits != visits) {
                visits = sample.visits;
                result = work.blend_result(visits);
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

void clear_uncovered_plane_row(const IndexedComposeJob &job, unsigned plane_row, unsigned stride,
                               unsigned left) {
    uint16_t *plane = job.plane.data() + size_t(plane_row) * stride;
    const unsigned width = job.geometry.width();
    const SpriteCoverage::Row coverage = job.coverage.row(plane_row);
    std::array<uint32_t, MAX_WORDS> covered{};
    for (unsigned w = 0; w < coverage.word_count(); ++w) covered[w] = coverage.covered_word(w);
    for (unsigned x = 0; x < stride; ++x) {
        const unsigned column = x - left;
        if (column >= width || !(covered[column >> 5] >> (column & 31) & 1)) plane[x] = 0;
    }
}

bool setup_matches(const RowSetup &setup, const SceneRow &row, unsigned active) {
    if (!setup.valid || setup.active != active || setup.clips != row.clips) return false;
    for (unsigned i = 0; i < LAYER_COUNT; ++i)
        if (setup.layers[i] != row.layer(LayerId{uint8_t(i)})) return false;
    return true;
}

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
    const SpriteCoverage::Row sprite_cover = job.coverage.row(plane_row);
    unsigned active = 0;
    for (unsigned i = 0; i < LAYER_COUNT; ++i) {
        const LayerId id{uint8_t(i)};
        const int text_y = flipped ? (511 - row.text_y) : row.text_y;
        const bool empty =
            (kind(id) == LayerKind::Playfield && row.playfields[sub_index(id)].empty_row) ||
            (id == LayerId::Text && !scene.text.row_used(unsigned(text_y))) ||
            (kind(id) == LayerKind::Sprite && !sprite_cover.covers(sub_index(id)));
        if (row.layer(id).enabled && (scene.layer_mask & layer_bit(id)) != 0 && !empty)
            active |= 1u << i;
    }
    const int left_edge = 46 - int(job.geometry.border);
    const int right_edge = 366 + int(job.geometry.border);
    if (!setup_matches(setup, row, active)) build_setup(setup, row, active, left_edge, right_edge);
    if (setup.canonical) {
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

TileRowTable::TileRowTable(std::span<const uint8_t> tile_pixels)
    : m_pixels(tile_pixels), m_rows(tile_pixels.size() / 256) {
    for (size_t tile = 0; tile < m_rows.size(); ++tile) {
        uint32_t low = 0, plane4 = 0, plane5 = 0;
        for (unsigned y = 0; y < 16; ++y) {
            const uint8_t *row = tile_pixels.data() + tile * 256 + y * 16;
            uint32_t words[4];
            std::memcpy(words, row, sizeof words);
            const uint32_t any = words[0] | words[1] | words[2] | words[3];
            low |= uint32_t((any & 0x0f0f0f0fu) != 0) << y;
            plane4 |= uint32_t((any & 0x10101010u) != 0) << y;
            plane5 |= uint32_t((any & 0x20202020u) != 0) << y;
        }
        m_rows[tile] = {uint16_t(low), uint16_t(plane4), uint16_t(plane5)};
    }
}

namespace {
bool uses_15bit_palette(const FrameScene &scene) noexcept {
    for (unsigned y = geometry::FIRST_LINE; y < geometry::END_LINE; ++y)
        if (scene.rows[y & 255].palette_15bit) return true;
    return false;
}

void glyph_row_masks(const FrameScene &scene, std::array<uint8_t, 256> &rows) noexcept {
    const auto glyphs = scene.text.glyphs();
    for (unsigned glyph = 0; glyph < rows.size(); ++glyph) {
        const uint8_t *data = glyphs.data() + size_t(glyph) * 32;
        uint8_t used = 0;
        for (unsigned y = 0; y < 8; ++y) {
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
    return need_15bit;
}

void compose_indexed_chunk(IndexedComposeJob &job, unsigned chunk) noexcept {
    const unsigned rows = job.geometry.height();
    const unsigned first = chunk * COMPOSE_CHUNK_ROWS;
    const unsigned last = std::min(first + COMPOSE_CHUNK_ROWS, rows);
    if (first >= last) return;
    raster_sprite_chunk(*job.bins, job.sprites, chunk, job.pen_mask, job.sprite_tiles, job.plane,
                        SpriteRasterOptions{job.geometry}, job.coverage);
    std::array<uint32_t, MAX_WIDTH> argb_scratch;
    RowSetup setup;
    setup.valid = false;
    for (unsigned row = first; row < last; ++row)
        compose_row(job, geometry::FIRST_LINE + row, setup, argb_scratch.data());
}
} // namespace f3rt
