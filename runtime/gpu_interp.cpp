#include "gpu_interp.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <numeric>

namespace f3rt {
namespace {
constexpr unsigned first_row = 24, end_row = 256;
constexpr unsigned row_at(unsigned y) { return GpuScene::rows + y * GpuScene::row_stride; }
constexpr unsigned pf_at(unsigned y, unsigned pf) { return row_at(y) + GpuScene::row_pf + pf * 6; }
constexpr unsigned layer_at(unsigned y, unsigned pf) { return row_at(y) + GpuScene::row_layers + pf * GpuScene::layer_stride; }
constexpr unsigned metadata_at(unsigned y, unsigned pf) {
    return InterpolationLayout::base + y * InterpolationLayout::row_stride + pf * InterpolationLayout::pf_stride;
}
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
void coefficients(std::span<uint32_t> words, unsigned at, float previous, float delta,
                  float next, VideoInterpolation mode) {
    const float m0 = mode == VideoInterpolation::Fit ? tangent(previous, delta) : delta;
    const float m1 = mode == VideoInterpolation::Fit ? tangent(delta, next) : delta;
    words[at] = std::bit_cast<uint32_t>(m0);
    words[at + 1] = std::bit_cast<uint32_t>(3 * delta - 2 * m0 - m1);
    words[at + 2] = std::bit_cast<uint32_t>(m0 + m1 - 2 * delta);
}
struct Row {
    int source = 0, zoom = 0, phase = 0, vertical = 0, palette = 0;
    bool valid = false;
};
bool valid_row(const GpuScene &scene, unsigned y, unsigned pf, GameVideoOptions options) {
    const auto &w = scene.words;
    const unsigned pa = pf_at(y, pf), la = layer_at(y, pf), ra = row_at(y);
    const auto &reference = scene.reference_rows[y];
    const auto &layer = reference.playfields[pf].layer;
    if (reference.bitmap || !layer.enabled || layer.mosaic || !(w[la] & 64u) ||
        (w[la] & ~255u) || w[la + 1] > 16 || w[pa + 1] > 511 ||
        w[pa + 2] == 0 || w[pa + 2] > 256 || w[pa + 3] == 0 ||
        w[pa + 3] > 510 || w[pa + 4] > 255 || w[pa + 5] > 8191 ||
        std::abs(int64_t(int32_t(w[pa]))) > (int64_t{1} << 24)) return false;
    for (unsigned i = 0; i < 4; ++i)
        if (((w[ra + 4] >> (i * 8)) & 255) > 8) return false;
    for (unsigned i = 0; i < w[la + 1]; ++i) {
        const int left = int32_t(w[la + 2 + i * 2]), right = int32_t(w[la + 3 + i * 2]);
        if (left >= right || left < 46 - int(options.border) || right > 366 + int(options.border)) return false;
    }
    return true;
}
bool same_controls(const GpuScene &scene, unsigned a, unsigned b, unsigned pf) {
    const auto &w = scene.words;
    const unsigned la = layer_at(a, pf), lb = layer_at(b, pf);
    const auto &x = scene.reference_rows[a].playfields[pf].layer;
    const auto &y = scene.reference_rows[b].playfields[pf].layer;
    if (w[row_at(a) + 4] != w[row_at(b) + 4] || x.clip_enabled != y.clip_enabled ||
        x.clip_inverted != y.clip_inverted || x.clip_inverse != y.clip_inverse) return false;
    for (unsigned i = 0; i < 2 + w[la + 1] * 2; ++i)
        if (w[la + i] != w[lb + i]) return false;
    return true;
}
unsigned rgb_jump(uint32_t a, uint32_t b) {
    unsigned maximum = 0;
    for (unsigned shift : {0u, 8u, 16u})
        maximum = std::max(maximum, unsigned(std::abs(int((a >> shift) & 255) - int((b >> shift) & 255))));
    return maximum;
}
// Conservative endpoint-bounded tile footprint, including border and fractional taps.
// Every bank traversed by the polynomial is checked against actual tile pen sets.
bool safe_palette(const GpuScene &scene, const Row &a, const Row &b, unsigned pf,
                  unsigned stride, GameVideoOptions options, std::span<const uint64_t> masks) {
    if (masks.size() < 32768) return false;
    const int source_b = a.source + phase_delta(a.source, b.source, 1024 * 256);
    // Source and zoom have independent cubics: all endpoint combinations,
    // not only paired endpoints, bound their combined horizontal footprint.
    const int64_t maximum_zoom = std::max(a.zoom, b.zoom);
    const int64_t x0 = int64_t(std::min(a.source, source_b)) - int64_t(options.border) * maximum_zoom;
    const int64_t x1 = int64_t(std::max(a.source, source_b)) + int64_t(320 + options.border) * maximum_zoom;
    const int phase_b = a.phase + phase_delta(a.phase, b.phase, 512 * 256);
    const int y0 = int(floor_divide(std::min(a.phase, phase_b), 16 * 256));
    const int y1 = int(floor_divide(std::max(a.phase, phase_b) + 256, 16 * 256));
    const int low = std::min(a.palette, b.palette), high = std::max(a.palette, b.palette);
    for (int cy = y0; cy <= y1; ++cy) {
        for (int64_t cx = floor_divide(x0 - 256, 16 * 256); cx <= floor_divide(x1 + 256, 16 * 256); ++cx) {
            const unsigned cell = (unsigned(cy) & 31) * 64 + (unsigned(cx) & 63);
            // Raw 4-byte PF cell (attributes<<16 | code); see runtime/game_tiles.hpp.
            const unsigned at = GpuScene::pf_cells + (pf * 2048 + cell) * 2;
            const auto &w = scene.words;
            const unsigned attr = w[at] >> 16, code = w[at] & 65535;
            const unsigned base = (attr & 511u) * 16u;
            const unsigned mask = (((attr >> 10u) & 3u & ~attr) << 4u) | 15u;
            if (base >= 8192 || mask > 63) return false;
            uint64_t pens = masks[code & 32767];
            while (pens) {
                const unsigned pen = unsigned(std::countr_zero(pens)) & mask;
                pens &= pens - 1;
                if (!pen) continue;
                for (int bank = low; bank < high; bank += int(stride)) {
                    if (rgb_jump(w[GpuScene::palette + ((base + pen + bank) & 8191)],
                                 w[GpuScene::palette + ((base + pen + bank + stride) & 8191)]) > 32) return false;
                }
            }
        }
    }
    return true;
}
} // namespace

InterpolationStats analyze_gpu_interpolation(const GpuScene &scene, GameVideoOptions options,
    VideoInterpolation mode, InterpolationFields fields, std::span<uint32_t> upload_words,
    std::span<const uint64_t> tile_pen_masks, std::span<const uint32_t> effective_words) noexcept {
    const auto clear_end = std::min(upload_words.size(), size_t(InterpolationLayout::word_count));
    if (clear_end > InterpolationLayout::base)
        std::fill(upload_words.begin() + InterpolationLayout::base, upload_words.begin() + clear_end, 0u);
    InterpolationStats stats;
    const auto reject = [&](InterpolationReason reason) {
        stats.reason = reason;
        for (auto &layer : stats.layers) layer.reason = reason;
        return stats;
    };
    if (mode == VideoInterpolation::Off) return reject(InterpolationReason::Off);
    if (options.scale == 1) return reject(InterpolationReason::NativeScale);
    if (scene.fallback) return reject(InterpolationReason::Oracle);
    if (upload_words.size() < InterpolationLayout::word_count || options.scale == 0 ||
        options.scale > GameVideoOptions::max_gpu_scale || options.border > GameVideoOptions::max_border)
        return reject(InterpolationReason::InvalidRows);
    stats.reason = InterpolationReason::NoEffect;
    const bool geometry = (unsigned(fields) & unsigned(InterpolationFields::Geometry)) != 0;
    const bool palette = (unsigned(fields) & unsigned(InterpolationFields::Palette)) != 0;
    if (!geometry && !palette) return stats;
    for (unsigned pf = 0; pf < 4; ++pf) {
        auto &layer = stats.layers[pf];
        std::array<Row, 256> rows{};
        std::array<bool, 256> links{};
        for (unsigned y = first_row; y < end_row; ++y) {
            auto &r = rows[y];
            const unsigned pa = pf_at(y, pf);
            const std::span<const uint32_t> w = effective_words.size() >= GpuScene::word_count ?
                effective_words : std::span<const uint32_t>{scene.words};
            r = {int32_t(w[pa]), int(w[pa + 2]), int(w[pa + 1] * 256 + w[pa + 4]),
                 int(w[pa + 3]), int(w[pa + 5]), valid_row(scene, y, pf, options)};
            if (!r.valid && ((w[layer_at(y, pf)] & 64u) || scene.reference_rows[y].playfields[pf].layer.enabled))
                ++layer.invalid_rows;
            if (y == first_row || !r.valid || !rows[y - 1].valid) continue;
            links[y - 1] = same_controls(scene, y - 1, y, pf) &&
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
                            safe[y] = !delta[y] || safe_palette(scene, rows[y], rows[y + 1], pf,
                                                               stride, options, tile_pen_masks);
                            if (!safe[y]) ++layer.unsafe_palette_pairs;
                        }
                    }
                    for (unsigned y = start + 1; y + 1 <= last; ++y) {
                        // Four real neighbors only, with raw run endpoints and no unsafe derivative samples.
                        if (y + 2 > last || !delta[y] ||
                            (field == 3 && (!safe[y - 1] || !safe[y] || !safe[y + 1]))) continue;
                        const unsigned at = metadata_at(y, pf);
                        constexpr unsigned offsets[]{InterpolationLayout::source, InterpolationLayout::zoom,
                            InterpolationLayout::vertical, InterpolationLayout::palette};
                        coefficients(upload_words, at + offsets[field], float(delta[y - 1]),
                                     float(delta[y]), float(delta[y + 1]), mode);
                        const uint32_t flag = 1u << field;
                        upload_words[at] |= flag;
                        if (field == 3) upload_words[at] |= stride << interpolation_palette_stride_shift;
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
    return stats;
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
