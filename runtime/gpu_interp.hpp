#pragma once
#include "gpu_scene.hpp"
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
// Upload-only, native-anchored polynomial increments; canonical scene stays unchanged.
struct InterpolationLayout {
    static constexpr unsigned base = GpuScene::word_count;
    static constexpr unsigned pf_stride = 13, row_stride = 52;
    static constexpr unsigned word_count = base + 256 * row_stride;
    static constexpr unsigned source = 1, zoom = 4, vertical = 7, palette = 10;
};
inline constexpr uint32_t interpolation_source = 1;
inline constexpr uint32_t interpolation_zoom = 2;
inline constexpr uint32_t interpolation_vertical = 4;
inline constexpr uint32_t interpolation_palette = 8;
inline constexpr unsigned interpolation_palette_stride_shift = 16;
InterpolationStats analyze_gpu_interpolation(const GpuScene &scene, GameVideoOptions options,
    VideoInterpolation mode, InterpolationFields fields, std::span<uint32_t> upload_words,
    std::span<const uint64_t> tile_pen_masks,
    std::span<const uint32_t> effective_words = {}) noexcept;
const char *interpolation_reason_name(InterpolationReason reason) noexcept;
} // namespace f3rt
