#pragma once
#include "gpu_interp.hpp"
#include "video_scale.hpp"
#include <SDL3/SDL.h>
#include <memory>
#include <span>

namespace f3rt {
class GpuVideo {
public:
    // SDL video must be initialized. Window may be null for offscreen parity.
    GpuVideo(SDL_Window *window, GameVideoOptions options,
             std::span<const uint8_t> playfield_assets,
             std::span<const uint8_t> sprite_assets, bool linear = false, bool vsync = true,
             VideoInterpolation interpolation = VideoInterpolation::Off,
             InterpolationFields fields = InterpolationFields::Geometry);
    ~GpuVideo();
    GpuVideo(const GpuVideo &) = delete;
    GpuVideo &operator=(const GpuVideo &) = delete;
    const char *driver() const;
    const InterpolationStats &last_interpolation() const;
    // Host-only geometry. Assets, pipelines and canonical game state survive.
    void set_scale(unsigned scale);
    void set_scale_mode(VideoScaleMode mode);
    // Same scene pipeline for presentation and fenced diagnostic readback.
    // Optional output receives exactly width*height ARGB8888 pixels.
    // layer_mask isolates layer contributions for parity; default is all nine.
    void draw(const GpuScene &scene, std::span<uint32_t> output = {}, unsigned layer_mask = 511);
    void save_surface(const char *path);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace f3rt
