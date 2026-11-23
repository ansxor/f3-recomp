#pragma once
#include "f3rt_video_cxx/video.h"
#include "renderer/gpu/motion.hpp"
#include "renderer/gpu/scene_layout.h"
#include <array>
#include <optional>
#include <span>
#include <string_view>

namespace f3rt {
using VideoInterpolation = rust_video::VideoInterpolation;
using InterpolationFields = rust_video::InterpolationFields;
using InterpolationReason = rust_video::InterpolationReason;

inline std::optional<InterpolationFields>
parse_interpolation_fields(std::string_view value) noexcept {
    if (value == "none") return InterpolationFields::None;
    if (value == "geometry") return InterpolationFields::Geometry;
    if (value == "palette") return InterpolationFields::Palette;
    if (value == "geometry,palette" || value == "palette,geometry") return InterpolationFields::All;
    return std::nullopt;
}
inline constexpr const char *interpolation_fields_name(InterpolationFields fields) noexcept {
    switch (fields) {
    case InterpolationFields::None:
        return "none";
    case InterpolationFields::Geometry:
        return "geometry";
    case InterpolationFields::Palette:
        return "palette";
    case InterpolationFields::All:
        return "geometry,palette";
    }
    return "unknown";
}
struct InterpolationConfig {
    VideoInterpolation mode = VideoInterpolation::Off;
    InterpolationFields fields = InterpolationFields::Geometry;
    bool operator==(const InterpolationConfig &) const = default;
};
using LayerInterpolationStats = rust_video::LayerInterpolationStats;
using InterpolationStats = rust_video::InterpolationStats;
using PlayfieldCoefficients = rust_video::PlayfieldCoefficients;
using InterpolationCoefficients = rust_video::InterpolationCoefficients;
struct InterpolationAnalysis {
    InterpolationStats stats;
    InterpolationCoefficients coefficients;
};

// Native-anchored polynomial increments for one playfield row; the canonical frame stays
// unchanged. Each triple is the cubic's (c1, c2, c3); a triple is zero unless its flag is set.
inline constexpr uint32_t INTERPOLATION_SOURCE = F3_INTERP_FLAG_SOURCE;
inline constexpr uint32_t INTERPOLATION_ZOOM = F3_INTERP_FLAG_ZOOM;
inline constexpr uint32_t INTERPOLATION_VERTICAL = F3_INTERP_FLAG_VERTICAL;
inline constexpr uint32_t INTERPOLATION_PALETTE = F3_INTERP_FLAG_PALETTE;

// Motion-effective geometry comes from `motion` (null: the canonical frame).
// Writes the coefficients into caller-owned `out` (no 56 KiB return copy).
InterpolationStats analyze_gpu_interpolation_into(const SceneSnapshot &frame,
                                                  GameVideoOptions options, VideoInterpolation mode,
                                                  InterpolationFields fields,
                                                  const MotionState *motion,
                                                  std::span<const uint64_t> tile_pen_masks,
                                                  InterpolationCoefficients &out) noexcept;
InterpolationAnalysis analyze_gpu_interpolation(const SceneSnapshot &frame,
                                                GameVideoOptions options, VideoInterpolation mode,
                                                InterpolationFields fields,
                                                const MotionState *motion,
                                                std::span<const uint64_t> tile_pen_masks) noexcept;
const char *interpolation_reason_name(InterpolationReason reason) noexcept;

// Prints one `video_interp ...` line to stdout whenever the observed reason/layer outcome changes.
class InterpolationLog {
public:
    InterpolationLog();
    ~InterpolationLog();
    InterpolationLog(const InterpolationLog &) = delete;
    InterpolationLog &operator=(const InterpolationLog &) = delete;
    InterpolationLog(InterpolationLog &&) noexcept;
    InterpolationLog &operator=(InterpolationLog &&) noexcept;

    void reset() noexcept;
    void observe(InterpolationConfig config, const InterpolationStats &stats);

private:
    rust::Box<rust_video::InterpolationLog> m_log;
};

} // namespace f3rt
