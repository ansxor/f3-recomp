#include "renderer/gpu/interp.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numeric>

namespace f3rt {
namespace {
constexpr unsigned first_row = geometry::first_line, end_row = geometry::end_line;
int phase_delta(int a, int b, int period) {
    int delta = (b - a) % period;
    if (delta > period / 2) delta -= period;
    if (delta < -period / 2) delta += period;
    return delta;
}
int64_t floor_divide(int64_t n, int64_t d) { return n >= 0 ? n / d : -1 - (-1 - n) / d; }
float tangent(float a, float b) {
    return a * b <= 0 ? 0 : 2 * a * b / (a + b);
}
void coefficients(std::array<float, 3> &out, float previous, float delta, float next,
                  VideoInterpolation mode) {
    const float m0 = mode == VideoInterpolation::Fit ? tangent(previous, delta) : delta;
    const float m1 = mode == VideoInterpolation::Fit ? tangent(delta, next) : delta;
    out = {m0, 3 * delta - 2 * m0 - m1, m0 + m1 - 2 * delta};
}
struct Row {
    int source = 0, zoom = 0, phase = 0, vertical = 0, palette = 0;
    bool valid = false;
};
// Fills `clips` (visible ranges at the render border) for rows that pass the cheap checks.
bool valid_row(const SceneRow &row, unsigned pf, GameVideoOptions options, ClipRanges &clips) {
    const auto &p = row.playfields[pf];
    const auto &layer = p.layer;
    // Alternate-map rows switch the sampled map per scanline; never interpolated across.
    if (row.bitmap || row.palette_15bit || !layer.enabled || layer.mosaic || p.alt_map ||
        uint32_t(p.source_y) > 511 || uint32_t(p.x_step) == 0 || uint32_t(p.x_step) > 256 ||
        uint32_t(p.y_step) == 0 || uint32_t(p.y_step) > 510 || p.palette_add > 8191 ||
        std::abs(int64_t(p.source_x)) > (int64_t{1} << 24)) return false;
    for (unsigned i = 0; i < 4; ++i)
        if (row.blend[i] > 8) return false;
    const int left_edge = 46 - int(options.border), right_edge = 366 + int(options.border);
    clips = clip_ranges(row, layer, int16_t(left_edge), int16_t(right_edge));
    for (unsigned i = 0; i < clips.count; ++i) {
        const int left = clips.ranges[i].left, right = clips.ranges[i].right;
        if (left >= right || left < left_edge || right > right_edge) return false;
    }
    return true;
}
bool same_controls(const SceneRow &a, const SceneRow &b, const ClipRanges &ca, const ClipRanges &cb, unsigned pf) {
    return a.blend == b.blend && a.playfields[pf].layer == b.playfields[pf].layer && ca.count == cb.count &&
        std::equal(ca.ranges.begin(), ca.ranges.begin() + ca.count, cb.ranges.begin());
}
unsigned rgb_jump(uint32_t a, uint32_t b) {
    unsigned maximum = 0;
    for (unsigned shift : {0u, 8u, 16u})
        maximum = std::max(maximum, unsigned(std::abs(int((a >> shift) & 255) - int((b >> shift) & 255))));
    return maximum;
}
// Conservative endpoint-bounded tile footprint, including border and fractional taps.
// Every bank traversed by the polynomial is checked against actual tile pen sets.
bool safe_palette(const CapturedFrame &frame, const Row &a, const Row &b, unsigned pf,
                  unsigned stride, GameVideoOptions options, std::span<const uint64_t> masks) {
    if (masks.empty()) return false;
    const int source_b = a.source + phase_delta(a.source, b.source, 1024 * 256);
    // Source and zoom have independent cubics: all endpoint combinations,
    // not only paired endpoints, bound their combined horizontal footprint.
    const int64_t maximum_zoom = std::max(a.zoom, b.zoom);
    const int64_t x0 = int64_t(std::min(a.source, source_b)) - int64_t(options.border) * maximum_zoom;
    const int64_t x1 = int64_t(std::max(a.source, source_b)) + int64_t(geometry::native_width + options.border) * maximum_zoom;
    const int phase_b = a.phase + phase_delta(a.phase, b.phase, 512 * 256);
    const int y0 = int(floor_divide(std::min(a.phase, phase_b), 16 * 256));
    const int y1 = int(floor_divide(std::max(a.phase, phase_b) + 256, 16 * 256));
    const int low = std::min(a.palette, b.palette), high = std::max(a.palette, b.palette);
    for (int cy = y0; cy <= y1; ++cy) {
        for (int64_t cx = floor_divide(x0 - 256, 16 * 256); cx <= floor_divide(x1 + 256, 16 * 256); ++cx) {
            const unsigned cell = (unsigned(cy) & 31) * 64 + (unsigned(cx) & 63);
            // Raw 4-byte PF cell (attributes<<16 | code); see runtime/renderer/game/tiles.hpp.
            const uint32_t raw = frame.tiles.cells(pf)[cell];
            const unsigned attr = raw >> 16, code = raw & 65535;
            const unsigned base = (attr & 511u) * 16u;
            const unsigned mask = (((attr >> 10u) & 3u & ~attr) << 4u) | 15u;
            if (base >= 8192 || mask > 63) return false;
            uint64_t pens = masks[wrap_tile_index(code, uint32_t(masks.size()))];
            while (pens) {
                const unsigned pen = unsigned(std::countr_zero(pens)) & mask;
                pens &= pens - 1;
                if (!pen) continue;
                for (int bank = low; bank < high; bank += int(stride)) {
                    if (rgb_jump(frame.colors[(base + pen + bank) & 8191],
                                 frame.colors[(base + pen + bank + stride) & 8191]) > 32) return false;
                }
            }
        }
    }
    return true;
}
} // namespace

InterpolationAnalysis analyze_gpu_interpolation(const CapturedFrame &frame, GameVideoOptions options,
    VideoInterpolation mode, InterpolationFields fields, const MotionState *motion,
    std::span<const uint64_t> tile_pen_masks) noexcept {
    InterpolationAnalysis result;
    InterpolationStats &stats = result.stats;
    const auto reject = [&](InterpolationReason reason) {
        stats.reason = reason;
        for (auto &layer : stats.layers) layer.reason = reason;
    };
    if (mode == VideoInterpolation::Off) { reject(InterpolationReason::Off); return result; }
    if (options.scale == 1) { reject(InterpolationReason::NativeScale); return result; }
    if (frame.fallback) { reject(InterpolationReason::Oracle); return result; }
    if (options.scale == 0 || options.scale > GameVideoOptions::max_gpu_scale ||
        options.border > GameVideoOptions::max_border)
        { reject(InterpolationReason::InvalidRows); return result; }
    stats.reason = InterpolationReason::NoEffect;
    const bool geometry = (unsigned(fields) & unsigned(InterpolationFields::Geometry)) != 0;
    const bool palette = (unsigned(fields) & unsigned(InterpolationFields::Palette)) != 0;
    if (!geometry && !palette) return result;
    for (unsigned pf = 0; pf < 4; ++pf) {
        auto &layer = stats.layers[pf];
        std::array<Row, 256> rows{};
        std::array<ClipRanges, 256> clips{};
        std::array<bool, 256> links{};
        for (unsigned y = first_row; y < end_row; ++y) {
            auto &r = rows[y];
            const auto &row = frame.rows[y];
            const auto &p = row.playfields[pf];
            const int32_t source = motion ? motion->rows[y].source_x[pf] : p.source_x;
            const int32_t phase = motion ? motion->rows[y].phase[pf] :
                int32_t(uint32_t(p.source_y) * 256u + p.y_fraction);
            r = {source, int(uint32_t(p.x_step)), int(phase), int(uint32_t(p.y_step)),
                 int(uint32_t(p.palette_add)), valid_row(row, pf, options, clips[y])};
            if (!r.valid && p.layer.enabled) ++layer.invalid_rows;
            if (y == first_row || !r.valid || !rows[y - 1].valid) continue;
            links[y - 1] = same_controls(frame.rows[y - 1], row, clips[y - 1], clips[y], pf) &&
                phase_delta(rows[y - 1].phase, r.phase, 512 * 256) == rows[y - 1].vertical;
            if (!links[y - 1]) ++layer.discontinuities;
        }
        // Each field gets its own maximal valid run: palette cannot disable geometry.
        for (unsigned field = 0; field < 4; ++field) {
            if ((field < 3 && !geometry) || (field == 3 && !palette)) continue;
            std::array<int, 256> delta{};
            std::array<bool, 256> continuous{};
            for (unsigned y = first_row; y + 1 < end_row; ++y) {
                if (!links[y]) continue;
                const auto &a = rows[y]; const auto &b = rows[y + 1];
                if (field == 0) delta[y] = phase_delta(a.source, b.source, 1024 * 256);
                if (field == 1) delta[y] = b.zoom - a.zoom;
                if (field == 2) delta[y] = phase_delta(a.phase, b.phase, 512 * 256);
                if (field == 3) delta[y] = b.palette - a.palette;
                continuous[y] = (field != 0 || std::abs(delta[y]) <= 4096) &&
                    (field != 1 || std::abs(delta[y]) <= 8) &&
                    (field != 2 || std::abs(b.vertical - a.vertical) <= 8) &&
                    (field != 3 || (a.palette % 16 == 0 && b.palette % 16 == 0));
                if (!continuous[y]) ++layer.discontinuities;
            }
            unsigned start = first_row;
            while (start < end_row) {
                unsigned last = start;
                int direction = 0;
                while (last + 1 < end_row && continuous[last]) {
                    if (field == 3 && delta[last]) {
                        const int sign = delta[last] > 0 ? 1 : -1;
                        if (direction && direction != sign) break;
                        direction = sign;
                    }
                    ++last;
                }
                unsigned steps = 0, stride = 0;
                for (unsigned y = start; y < last; ++y) {
                    if (field == 2 ? rows[y].vertical != rows[y + 1].vertical : delta[y] != 0) ++steps;
                    if (field == 3) stride = std::gcd(stride, unsigned(std::abs(delta[y])));
                }
                // Palette consumer banks are rooted at zero; include the run's origin
                // so an offset ramp cannot accidentally interpolate different pens.
                if (field == 3) stride = std::gcd(stride, unsigned(rows[start].palette));
                if (last - start >= 3 && steps >= 2 &&
                    (field != 3 || (stride && stride % 16 == 0))) {
                    std::array<bool, 256> safe{};
                    if (field == 3) {
                        for (unsigned y = start; y < last; ++y) {
                            safe[y] = !delta[y] || safe_palette(frame, rows[y], rows[y + 1], pf,
                                                               stride, options, tile_pen_masks);
                            if (!safe[y]) ++layer.unsafe_palette_pairs;
                        }
                    }
                    for (unsigned y = start + 1; y + 1 <= last; ++y) {
                        // Four real neighbors only, with raw run endpoints and no unsafe derivative samples.
                        if (y + 2 > last || !delta[y] ||
                            (field == 3 && (!safe[y - 1] || !safe[y] || !safe[y + 1]))) continue;
                        auto &out = result.coefficients.rows[y][pf];
                        std::array<float, 3> &triple = field == 0 ? out.source : field == 1 ? out.zoom :
                            field == 2 ? out.vertical : out.palette;
                        coefficients(triple, float(delta[y - 1]), float(delta[y]), float(delta[y + 1]), mode);
                        const uint32_t flag = 1u << field;
                        out.flags |= uint8_t(flag);
                        if (field == 3) out.palette_stride = uint16_t(stride);
                        stats.row_fields[pf][y] |= uint8_t(flag);
                        if (field == 0) ++layer.source_rows;
                        if (field == 1) ++layer.zoom_rows;
                        if (field == 2) ++layer.vertical_rows;
                        if (field == 3) ++layer.palette_rows;
                    }
                }
                start = last + 1;
            }
        }
        if (layer.source_rows || layer.zoom_rows || layer.vertical_rows || layer.palette_rows)
            layer.reason = InterpolationReason::Applied;
        else if (layer.unsafe_palette_pairs) layer.reason = InterpolationReason::PaletteDiscontinuity;
        else if (layer.discontinuities) layer.reason = InterpolationReason::DiscreteBoundary;
        else if (layer.invalid_rows) layer.reason = InterpolationReason::InvalidRows;
        if (layer.reason == InterpolationReason::Applied) stats.reason = InterpolationReason::Applied;
        else if (stats.reason != InterpolationReason::Applied && layer.reason != InterpolationReason::NoEffect)
            stats.reason = layer.reason;
    }
    return result;
}

const char *interpolation_reason_name(InterpolationReason reason) noexcept {
    switch (reason) {
    case InterpolationReason::Off: return "off";
    case InterpolationReason::NativeScale: return "native-scale";
    case InterpolationReason::Oracle: return "oracle";
    case InterpolationReason::NoEffect: return "no-effect";
    case InterpolationReason::InvalidRows: return "invalid-rows";
    case InterpolationReason::DiscreteBoundary: return "discrete-boundary";
    case InterpolationReason::PaletteDiscontinuity: return "palette-discontinuity";
    case InterpolationReason::Applied: return "applied";
    }
    return "unknown";
}
} // namespace f3rt
