#pragma once
#include "gpu_interp.hpp"
#include "gpu_motion.hpp"
#include "video_scale.hpp"
#include <SDL3/SDL.h>
#include <filesystem>
#include <memory>
#include <span>

namespace f3rt {
enum class Postprocess { Off, Crt, User };
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
    // Host overlay is invoked on the actual swapchain after the game blit.
    using Overlay = void (*)(void *, SDL_GPUCommandBuffer *, SDL_GPUTexture *, Uint32, Uint32);
    SDL_GPUDevice *device() const;
    void set_linear(bool linear);
    void set_overlay(Overlay callback, void *userdata);
    // User ABI: .metal entry f3_postprocess on Metal, .spv entry main on Vulkan.
    // Fullscreen triangle, position builtin only; texture/sampler slot 0 and a
    // float4 uniform slot 0: internal width, height, scale, elapsed seconds.
    // Vulkan resources: combined sampler set 2/binding 0, uniform set 3/binding 0.
    // Files are limited to 1 MiB. Errors throw and retain the last valid preset.
    void set_postprocess(Postprocess preset, const std::filesystem::path &user_shader = {});
    Postprocess postprocess() const;
    // Output/layer-mask diagnostics remain baseline unless process_diagnostic is
    // explicitly true. Optional output receives width*height ARGB8888 pixels.
    void draw(const GpuScene &scene, std::span<uint32_t> output = {}, unsigned layer_mask = 511,
              bool process_diagnostic = false);
    // Explicit opt-in; completed emulated frames are captured, never draws.
    void capture_motion(const GpuScene &scene, uint64_t frame);
    void reset_motion();
    void draw_motion(const GpuScene &scene, float alpha, std::span<uint32_t> output = {},
                     unsigned layer_mask = 511, bool process_diagnostic = false);
    const MotionInterpolationStats &last_motion() const;
    // Saves the last rendered game image, including active postprocessing, but
    // excluding the crisp swapchain overlay. A preset/scale change needs a draw.
    void save_surface(const char *path);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace f3rt
