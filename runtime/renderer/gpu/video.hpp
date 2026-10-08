#pragma once
#include "renderer/game/captured_frame.hpp"
#include "renderer/gpu/interp.hpp"
#include "renderer/gpu/motion.hpp"
#include "renderer/scale.hpp"
#include "renderer/sprite_presentation.hpp"
#include <SDL3/SDL.h>
#include <chrono>
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
    // Flicker shadows (SceneSprite::shadow) are always presented display-synced: tagged shadows are
    // skipped on odd presented frames, counted as accepted swapchain submissions (presented_frames()).
    // set_presented_frames exists for offscreen tools, which never present.
    uint64_t presented_frames() const;
    void set_presented_frames(uint64_t count);
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
    void draw(const CapturedFrame &scene, std::span<uint32_t> output = {}, unsigned layer_mask = 511,
              bool process_diagnostic = false);
    // Skip busy in-flight frames before uploading/rendering. No fence wait or
    // readback; draw() still waits for explicit captures and diagnostics.
    bool present(const CapturedFrame &scene);
    // One GPU frame in flight and (macOS) a display-link tick for pace_motion/present_motion. Implied by
    // capture_motion; call it alone to redraw at display rate without temporal history (flicker shadows).
    void enable_display_pacing();
    // Explicit opt-in; completed emulated frames are captured, never draws.
    void capture_motion(const CapturedFrame &scene, uint64_t frame);
    void reset_motion();
    void draw_motion(const CapturedFrame &scene, float alpha, std::span<uint32_t> output = {},
                     unsigned layer_mask = 511, bool process_diagnostic = false);
    const MotionInterpolationStats &last_motion() const;
    // Pace before testing the native-frame deadline so the selected pair is
    // current at this display tick. Next presenting draw consumes this wait.
    // Returns false without an active native pacer; caller must use deadlines.
    bool pace_motion();
    // Sample live alpha after acquisition. Caller owns display pacing; busy
    // GPU frames are skipped unless an explicit capture requests a wait.
    bool present_motion(const CapturedFrame &scene, std::chrono::steady_clock::time_point frame_start,
                        std::chrono::nanoseconds period, bool wait = false);
    // Diagnostic single-window comparison: left native/current, right temporal.
    // Capture each changed input scene first; the native pane is cached until
    // capture_motion, reset_motion, scale or postprocess changes.
    // Optional one-shot PNG of the full-window composition submitted to the
    // drawable, including overlay. Null adds no readback/allocation/fence wait.
    void draw_motion_comparison_timed(const CapturedFrame &scene,
                                     std::chrono::steady_clock::time_point frame_start,
                                     std::chrono::nanoseconds period, const char *capture_path = nullptr);
    // Accepted non-null swapchain submission in the last draw, not scanout.
    bool last_presented() const;
    double display_hz() const; // SDL current display mode, not observed cadence.
    double display_callback_hz() const; // Observed macOS vsync callback cadence.
    double requested_display_hz() const; // Best-effort display-link request.
    // Saves the last rendered game image, including active postprocessing, but
    // excluding the crisp swapchain overlay. A preset/scale change needs a draw.
    void save_surface(const char *path);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace f3rt
