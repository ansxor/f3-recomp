#pragma once
#include "f3rt_video_cxx/video.h"
#include <optional>

#include <string_view>

namespace f3rt {
using VideoInterpolation = rust_video::VideoInterpolation;
using InterpolationFields = rust_video::InterpolationFields;


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
} // namespace f3rt
