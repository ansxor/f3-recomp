#pragma once
#include "gpu_scene.hpp"
#include <span>

namespace f3rt {
enum class VideoInterpolation { Off, Linear, Fit };
enum class InterpolationReason {
    Off, NativeScale, Oracle, NoKnownEffect, InvalidRows, NonMonotonic,
    SourceJump, SourceResidual, PaletteResidual, PaletteDiscontinuity, Applied
};
struct InterpolationStats {
    InterpolationReason reason = InterpolationReason::Off;
    unsigned first = 152, last = 255, palette_last = 152;
    unsigned geometry_rows = 0, palette_rows = 0, unsafe_palette_pairs = 0;
    float source_residual = 0, palette_residual = 0;
};
// Metadata is written only into the upload copy, never the canonical scene.
// Six padding words per row: flags, affine source X, and four local palette
// polynomial coefficients. Flags: bit 0 geometry, bit 1 palette, upper 16
// bits minimum valid palette-bank offset for this frame's continuous prefix.
inline constexpr uint32_t interpolation_geometry = 1;
inline constexpr uint32_t interpolation_palette = 2;
inline constexpr unsigned interpolation_palette_min_shift = 16;
InterpolationStats analyze_gpu_interpolation(const GpuScene &scene, GameVideoOptions options,
    VideoInterpolation mode, std::span<uint32_t> upload_words,
    std::span<const uint64_t> tile_pen_masks) noexcept;
const char *interpolation_reason_name(InterpolationReason reason) noexcept;
} // namespace f3rt
