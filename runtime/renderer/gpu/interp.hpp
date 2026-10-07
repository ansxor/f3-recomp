#pragma once
#include "renderer/gpu/motion.hpp"
#include "renderer/gpu/scene_layout.h"
#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace f3rt {
enum class VideoInterpolation { Off, Linear, Fit };
enum class InterpolationFields : uint8_t { None = 0, Geometry = 1, Palette = 2, All = 3 };
inline std::optional<InterpolationFields> parse_interpolation_fields(std::string_view value) noexcept {
    if (value == "none") return InterpolationFields::None;
    if (value == "geometry") return InterpolationFields::Geometry;
    if (value == "palette") return InterpolationFields::Palette;
    if (value == "geometry,palette" || value == "palette,geometry") return InterpolationFields::All;
    return std::nullopt;
}
inline constexpr const char *interpolation_fields_name(InterpolationFields fields) noexcept {
    switch (fields) {
    case InterpolationFields::None: return "none";
    case InterpolationFields::Geometry: return "geometry";
    case InterpolationFields::Palette: return "palette";
    case InterpolationFields::All: return "geometry,palette";
    }
    return "unknown";
}
enum class InterpolationReason {
    Off, NativeScale, Oracle, NoEffect, InvalidRows, DiscreteBoundary,
    PaletteDiscontinuity, Applied
};
struct LayerInterpolationStats {
    InterpolationReason reason = InterpolationReason::NoEffect;
    unsigned source_rows = 0, zoom_rows = 0, vertical_rows = 0, palette_rows = 0;
    unsigned invalid_rows = 0, discontinuities = 0, unsafe_palette_pairs = 0;
    bool operator==(const LayerInterpolationStats &) const = default;
};
struct InterpolationStats {
    InterpolationReason reason = InterpolationReason::Off;
    std::array<LayerInterpolationStats, 4> layers{};
    std::array<std::array<uint8_t, 256>, 4> row_fields{};
};
// Native-anchored polynomial increments for one playfield row; the canonical
// frame stays unchanged. Each triple is the cubic's (c1, c2, c3); a triple is
// zero unless its flag is set.
inline constexpr uint32_t interpolation_source = F3_INTERP_FLAG_SOURCE;
inline constexpr uint32_t interpolation_zoom = F3_INTERP_FLAG_ZOOM;
inline constexpr uint32_t interpolation_vertical = F3_INTERP_FLAG_VERTICAL;
inline constexpr uint32_t interpolation_palette = F3_INTERP_FLAG_PALETTE;
struct PlayfieldCoefficients {
    uint8_t flags = 0;             // interpolation_* bits
    uint16_t palette_stride = 0;   // bank stride, meaningful with interpolation_palette
    std::array<float, 3> source{}, zoom{}, vertical{}, palette{};
};
struct InterpolationCoefficients {
    std::array<std::array<PlayfieldCoefficients, 4>, 256> rows{}; // [scanline][playfield]
};
struct InterpolationAnalysis {
    InterpolationStats stats;
    InterpolationCoefficients coefficients;
};
// Motion-effective geometry comes from `motion` (null: the canonical frame).
InterpolationAnalysis analyze_gpu_interpolation(const CapturedFrame &frame, GameVideoOptions options,
    VideoInterpolation mode, InterpolationFields fields, const MotionState *motion,
    std::span<const uint64_t> tile_pen_masks) noexcept;
const char *interpolation_reason_name(InterpolationReason reason) noexcept;
} // namespace f3rt
