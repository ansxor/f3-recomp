#pragma once
#include "f3rt/game_video.hpp"
#include <algorithm>
#include <cstdint>

namespace f3rt {
enum class VideoScaleMode { Fixed, Auto, AutoInteger };

inline unsigned video_scale_for_window(VideoScaleMode mode, unsigned pixel_width,
                                       unsigned pixel_height, unsigned border) {
    const unsigned native_width = geometry::native_width + border * 2;
    unsigned scale;
    if (mode == VideoScaleMode::Auto) {
        // Ceil the limiting axis without floating point or overflowing an addition.
        const unsigned width_scale = pixel_width / native_width + (pixel_width % native_width != 0);
        const unsigned height_scale = pixel_height / geometry::height + (pixel_height % geometry::height != 0);
        scale = std::min(width_scale, height_scale);
    } else {
        scale = std::min(pixel_width / native_width, pixel_height / geometry::height);
    }
    return std::clamp(scale, 1u, GameVideoOptions::max_scale);
}

struct VideoBlit {
    unsigned source_x, source_y, source_width, source_height;
    unsigned x, y, width, height;
};

inline VideoBlit video_blit_for_window(VideoScaleMode mode, GameVideoOptions options,
                                       unsigned pixel_width, unsigned pixel_height) {
    const unsigned source_width = options.width(), source_height = options.height();
    if (mode == VideoScaleMode::AutoInteger) {
        const unsigned width = std::min(pixel_width, source_width);
        const unsigned height = std::min(pixel_height, source_height);
        return {(source_width - width) / 2, (source_height - height) / 2, width, height,
                (pixel_width - width) / 2, (pixel_height - height) / 2, width, height};
    }
    unsigned width = pixel_width, height = pixel_height;
    if (uint64_t(pixel_width) * source_height > uint64_t(pixel_height) * source_width)
        width = unsigned(uint64_t(pixel_height) * source_width / source_height);
    else
        height = unsigned(uint64_t(pixel_width) * source_height / source_width);
    return {0, 0, source_width, source_height,
            (pixel_width - width) / 2, (pixel_height - height) / 2, width, height};
}
} // namespace f3rt
