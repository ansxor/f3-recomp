#include "gpu_interp.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>

namespace f3rt {
namespace {
constexpr unsigned first_row = 152, last_row = 255, plane = 2;
constexpr std::array<unsigned, 11> band_lengths{4, 4, 4, 6, 8, 8, 8, 12, 16, 16, 18};
constexpr unsigned row_at(unsigned y) { return GpuScene::rows + y * GpuScene::row_stride; }
constexpr unsigned pf_at(unsigned y) { return row_at(y) + GpuScene::row_pf + plane * 6; }
constexpr unsigned layer_at(unsigned y) { return row_at(y) + GpuScene::row_layers + plane * GpuScene::layer_stride; }
int64_t floor_divide(int64_t n, int64_t d) { return n >= 0 ? n / d : -1 - (-1 - n) / d; }
double polynomial(const std::array<double, 4> &c, double t) {
    return ((c[3] * t + c[2]) * t + c[1]) * t + c[0];
}
// Fixed-size normal equations on [-1,1] avoid the conditioning of screen-row powers.
bool fit_palette(const GpuScene &scene, unsigned last, std::array<double, 4> &coefficients) {
    double equations[4][5]{};
    for (unsigned y = first_row; y <= last; ++y) {
        const double t = 2.0 * (y - first_row) / (last - first_row) - 1.0;
        double powers[7]{1};
        for (unsigned i = 1; i < 7; ++i) powers[i] = powers[i - 1] * t;
        for (unsigned i = 0; i < 4; ++i) {
            for (unsigned j = 0; j < 4; ++j) equations[i][j] += powers[i + j];
            equations[i][4] += powers[i] * scene.words[pf_at(y) + 5];
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        unsigned pivot = i;
        for (unsigned j = i + 1; j < 4; ++j)
            if (std::abs(equations[j][i]) > std::abs(equations[pivot][i])) pivot = j;
        if (std::abs(equations[pivot][i]) < 1e-12) return false;
        if (pivot != i) for (unsigned j = i; j < 5; ++j) std::swap(equations[i][j], equations[pivot][j]);
        const double divisor = equations[i][i];
        for (unsigned j = i; j < 5; ++j) equations[i][j] /= divisor;
        for (unsigned k = 0; k < 4; ++k) {
            if (k == i) continue;
            const double factor = equations[k][i];
            for (unsigned j = i; j < 5; ++j) equations[k][j] -= factor * equations[i][j];
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        coefficients[i] = equations[i][4];
        if (!std::isfinite(coefficients[i])) return false;
    }
    return true;
}
bool decreasing(const std::array<double, 4> &c) {
    const auto derivative = [&](double t) { return c[1] + 2 * c[2] * t + 3 * c[3] * t * t; };
    double maximum = std::max(derivative(-1), derivative(1));
    if (c[3] != 0) {
        const double vertex = -c[2] / (3 * c[3]);
        if (vertex > -1 && vertex < 1) maximum = std::max(maximum, derivative(vertex));
    }
    return maximum <= 1e-9;
}
unsigned rgb_jump(uint32_t a, uint32_t b) {
    unsigned maximum = 0;
    for (unsigned shift : {0u, 8u, 16u}) {
        const int difference = int((a >> shift) & 255) - int((b >> shift) & 255);
        maximum = std::max(maximum, unsigned(std::abs(difference)));
    }
    return maximum;
}
}

InterpolationStats analyze_gpu_interpolation(const GpuScene &scene, GameVideoOptions options,
    VideoInterpolation mode, std::span<uint32_t> upload_words,
    std::span<const uint64_t> tile_pen_masks) noexcept {
    // Even a rejected or truncated upload must not retain metadata from a previous frame.
    for (unsigned y = 0; y < 256; ++y) {
        const unsigned at = row_at(y) + GpuScene::row_interp;
        for (unsigned i = 0; i < 6 && at + i < upload_words.size(); ++i) upload_words[at + i] = 0;
    }
    InterpolationStats stats;
    const auto reject = [&](InterpolationReason reason) { stats.reason = reason; return stats; };
    if (mode == VideoInterpolation::Off) return reject(InterpolationReason::Off);
    if (options.scale == 1) return reject(InterpolationReason::NativeScale);
    if (scene.fallback) return reject(InterpolationReason::Oracle);
    if (upload_words.size() < GpuScene::word_count || tile_pen_masks.size() < 32768 ||
        options.scale == 0 || options.scale > GameVideoOptions::max_scale || options.border > GameVideoOptions::max_border)
        return reject(InterpolationReason::InvalidRows);

    const auto &w = scene.words;
    const unsigned initial_layer = layer_at(first_row);
    const unsigned initial_row = row_at(first_row);
    const auto &initial_reference = scene.reference_rows[first_row].playfields[plane].layer;
    unsigned band = 0, remaining = band_lengths[0];
    double source_intercept = 0;
    // Complete candidate validation precedes footprint analysis, fits, and metadata writes.
    for (unsigned y = first_row; y <= last_row; ++y) {
        const unsigned pa = pf_at(y), la = layer_at(y), ra = row_at(y);
        const auto &reference = scene.reference_rows[y];
        const auto &layer = reference.playfields[plane].layer;
        if (!(w[la] & (1u << 6)) || (w[la] & (1u << 8)) || (w[la] & ~0x1ffu) ||
            reference.bitmap || !layer.enabled || layer.mosaic || w[pa + 3] != 256 || w[pa + 1] > 511 || w[pa + 4] > 255 ||
            w[la + 1] > 16)
            return reject(InterpolationReason::InvalidRows);
        if (int64_t(int32_t(w[pa])) < -(int64_t{1} << 24) || int64_t(int32_t(w[pa])) > (int64_t{1} << 24))
            return reject(InterpolationReason::InvalidRows);
        if (w[pa + 2] != 256 - 2 * (y - first_row) || w[pa + 5] != (10 - band) * 64)
            return reject(InterpolationReason::NoKnownEffect);
        if (w[la] != w[initial_layer] || w[la + 1] != w[initial_layer + 1] ||
            w[ra + 4] != w[initial_row + 4] || w[pa + 4] != w[pf_at(first_row) + 4] ||
            layer.clip_enabled != initial_reference.clip_enabled || layer.clip_inverted != initial_reference.clip_inverted ||
            layer.clip_inverse != initial_reference.clip_inverse)
            return reject(InterpolationReason::InvalidRows);
        for (unsigned i = 0; i < 4; ++i)
            if (((w[ra + 4] >> (i * 8)) & 255) > 8) return reject(InterpolationReason::InvalidRows);
        for (unsigned i = 0; i < w[la + 1]; ++i) {
            const int32_t left = int32_t(w[la + 2 + i * 2]), right = int32_t(w[la + 3 + i * 2]);
            if (left >= right || left < 46 - int(options.border) || right > 366 + int(options.border) ||
                w[la + 2 + i * 2] != w[initial_layer + 2 + i * 2] ||
                w[la + 3 + i * 2] != w[initial_layer + 3 + i * 2])
                return reject(InterpolationReason::InvalidRows);
        }
        if (y != first_row) {
            if (w[pa + 1] != ((w[pf_at(y - 1) + 1] + 1) & 511)) return reject(InterpolationReason::InvalidRows);
            const int64_t delta = int64_t(int32_t(w[pa])) - int32_t(w[pf_at(y - 1)]);
            if (delta <= 0) return reject(InterpolationReason::NonMonotonic);
            if (delta > 640) return reject(InterpolationReason::SourceJump);
        }
        source_intercept += double(int32_t(w[pa])) - 324.0 * (y - first_row);
        if (--remaining == 0 && band + 1 < band_lengths.size()) remaining = band_lengths[++band];
    }
    source_intercept /= last_row - first_row + 1;
    for (unsigned y = first_row; y <= last_row; ++y)
        stats.source_residual = std::max(stats.source_residual, float(std::abs(double(int32_t(w[pf_at(y)])) -
            (source_intercept + 324.0 * (y - first_row)))));
    if (stats.source_residual > 192) return reject(InterpolationReason::SourceResidual);

    // Union the actual tile pen sets at every potentially sampled cell. Four source
    // pixels cover both the affine residual and adjacent linear-filter taps.
    std::array<uint64_t, 512> palette_pens{};
    std::array<uint64_t, 32> visited_cells{};
    for (unsigned y = first_row; y <= last_row; ++y) {
        const unsigned pa = pf_at(y);
        const int64_t sx = int32_t(w[pa]), step = w[pa + 2];
        const int64_t left_cell = floor_divide(sx - int64_t(options.border) * step - 4 * 256, 16 * 256);
        const int64_t right_cell = floor_divide(sx + int64_t(319 + options.border) * step + 4 * 256, 16 * 256);
        for (unsigned dy = 0; dy < 2; ++dy) {
            const unsigned cell_y = ((w[pa + 1] + dy) & 511) / 16;
            for (int64_t x = left_cell; x <= right_cell; ++x) {
                const unsigned cell = cell_y * 64 + unsigned(uint64_t(x) & 63);
                const uint64_t bit = uint64_t{1} << (cell & 63);
                if (visited_cells[cell / 64] & bit) continue;
                visited_cells[cell / 64] |= bit;
                const unsigned at = GpuScene::pf_cells + (plane * 2048 + cell) * 2;
                const unsigned base = w[at + 1] & 65535, mask = (w[at + 1] >> 16) & 255;
                if (base >= 8192 || (base & 15) || mask > 63 || w[at] > 65535)
                    return reject(InterpolationReason::InvalidRows);
                uint64_t pens = tile_pen_masks[w[at] & 32767];
                while (pens) {
                    const unsigned pen = unsigned(std::countr_zero(pens)) & mask;
                    pens &= pens - 1;
                    if (pen) palette_pens[base / 16] |= uint64_t{1} << pen;
                }
            }
        }
    }
    std::array<bool, 10> unsafe{};
    for (unsigned pair = 0; pair < 10; ++pair) {
        for (unsigned base = 0; base < palette_pens.size() && !unsafe[pair]; ++base) {
            uint64_t pens = palette_pens[base];
            while (pens) {
                const unsigned pen = unsigned(std::countr_zero(pens));
                pens &= pens - 1;
                const unsigned index = base * 16 + pen + pair * 64;
                if (rgb_jump(w[GpuScene::palette + (index & 8191)],
                    w[GpuScene::palette + ((index + 64) & 8191)]) > 32) {
                    unsafe[pair] = true;
                    stats.unsafe_palette_pairs |= 1u << pair;
                    break;
                }
            }
        }
    }
    stats.palette_last = last_row;
    for (unsigned y = first_row + 1; y <= last_row; ++y) {
        const unsigned previous = w[pf_at(y - 1) + 5], current = w[pf_at(y) + 5];
        if (previous != current && unsafe[current / 64]) { stats.palette_last = y - 1; break; }
    }
    if (stats.palette_last - first_row + 1 < 16) return reject(InterpolationReason::PaletteDiscontinuity);
    std::array<double, 4> coefficients{};
    if (!fit_palette(scene, stats.palette_last, coefficients)) return reject(InterpolationReason::PaletteResidual);
    const double dt = 2.0 / (stats.palette_last - first_row);
    for (unsigned y = first_row; y <= stats.palette_last; ++y) {
        const double t = dt * (y - first_row) - 1;
        stats.palette_residual = std::max(stats.palette_residual,
            float(std::abs(polynomial(coefficients, t) - w[pf_at(y) + 5])));
    }
    if (stats.palette_residual > 48) return reject(InterpolationReason::PaletteResidual);
    if (!decreasing(coefficients)) return reject(InterpolationReason::NonMonotonic);

    for (unsigned y = first_row + 1; y < last_row; ++y) {
        const unsigned at = row_at(y) + GpuScene::row_interp;
        upload_words[at] = interpolation_geometry |
            (w[pf_at(stats.palette_last) + 5] << interpolation_palette_min_shift);
        upload_words[at + 1] = std::bit_cast<uint32_t>(float(source_intercept + 324.0 * (y - first_row)));
        ++stats.geometry_rows;
        if (y < stats.palette_last) {
            const double t = dt * (y - first_row) - 1;
            const std::array<double, 4> local{polynomial(coefficients, t),
                (coefficients[1] + 2 * coefficients[2] * t + 3 * coefficients[3] * t * t) * dt,
                (coefficients[2] + 3 * coefficients[3] * t) * dt * dt, coefficients[3] * dt * dt * dt};
            upload_words[at] |= interpolation_palette;
            for (unsigned i = 0; i < 4; ++i) upload_words[at + 2 + i] = std::bit_cast<uint32_t>(float(local[i]));
            ++stats.palette_rows;
        }
    }
    return reject(InterpolationReason::Applied);
}

const char *interpolation_reason_name(InterpolationReason reason) noexcept {
    switch (reason) {
    case InterpolationReason::Off: return "off";
    case InterpolationReason::NativeScale: return "native-scale";
    case InterpolationReason::Oracle: return "oracle";
    case InterpolationReason::NoKnownEffect: return "no-known-effect";
    case InterpolationReason::InvalidRows: return "invalid-rows";
    case InterpolationReason::NonMonotonic: return "nonmonotonic";
    case InterpolationReason::SourceJump: return "source-jump";
    case InterpolationReason::SourceResidual: return "source-residual";
    case InterpolationReason::PaletteResidual: return "palette-residual";
    case InterpolationReason::PaletteDiscontinuity: return "palette-discontinuity";
    case InterpolationReason::Applied: return "applied";
    }
    return "unknown";
}
} // namespace f3rt
