#pragma once
#include "f3rt/game_video.hpp"
#include "f3rt_video_cxx/video.h"
#include "renderer/game/scene_snapshot.hpp"
#include "renderer/gpu/interp.hpp"

#include "renderer/scale.hpp"
#include <SDL3/SDL.h>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>

namespace f3rt {
using MotionInterpolationStats = rust_video::MotionInterpolationStats;

enum class Postprocess { Off, Crt, User };

// Optional host overlay invoked on the window composition target (the swapchain directly, or the
// offscreen capture texture during window capture) after the game blit.
using GpuOverlay = void (*)(void *, SDL_GPUCommandBuffer *, SDL_GPUTexture *composition_target,
                            Uint32, Uint32);

// Opaque Rust texture token; only the GPU backend interprets it.
struct GpuSurface {
    uintptr_t texture = 0;
    uint32_t width = 0, height = 0;
    explicit operator bool() const noexcept { return texture != 0; }
};

/// Owned render texture shared with the Rust backend. Move-only; destroy it before its GpuVideo.
class GpuTexture {
public:
    GpuTexture();
    ~GpuTexture();
    GpuTexture(GpuTexture &&other) noexcept;
    GpuTexture &operator=(GpuTexture &&other) noexcept;
    GpuTexture(const GpuTexture &) = delete;
    GpuTexture &operator=(const GpuTexture &) = delete;

    GpuSurface surface() const;
    bool empty() const;
    explicit operator bool() const { return !empty(); }
    rust::Box<rust_video::GpuTexture> &inner() { return m_texture; }

private:
    rust::Box<rust_video::GpuTexture> m_texture;
};

/// One offscreen draw request.
struct RenderRequest {
    SpriteList sprites = SpriteList::Presented;
    // Output/layer-mask diagnostics remain baseline unless `postprocess` is explicitly true.
    uint32_t layer_mask = ALL_LAYERS;
    // Temporal geometry at this alpha (needs capture_motion history); nullopt renders the
    // canonical frame.
    std::optional<float> alpha = std::nullopt;
    bool postprocess = false;
};

// Live motion alpha is the elapsed fraction of the native frame period, sampled after acquisition.
struct MotionLiveClock {
    std::chrono::steady_clock::time_point frame_start;
    std::chrono::nanoseconds period{};
};
// monostate: static (canonical); float: fixed alpha; MotionLiveClock: timed live alpha.
using PresentTiming = std::variant<std::monostate, float, MotionLiveClock>;

struct PresentRequest {
    // Block for the swapchain and any display tick. False skips the frame when the GPU is busy:
    // nothing is uploaded or rendered, and no fence is waited on.
    bool wait = false;
    PresentTiming timing{};
    // Empty: one full-window blit. Otherwise the left half shows this (already oriented) surface
    // and the right half the presented game image.
    GpuSurface left_pane{};
    // One-shot PNG of the full-window composition submitted to the drawable, including overlay.
    // Null adds no readback, allocation or fence wait.
    const char *capture_path = nullptr;
};

class GpuVideo {
public:
    // SDL video must be initialized. Window may be null for offscreen parity.
    GpuVideo(SDL_Window *window, GameVideoOptions options,
             std::span<const uint8_t> playfield_assets, std::span<const uint8_t> sprite_assets,
             bool linear = false, bool vsync = true,
             VideoInterpolation interpolation = VideoInterpolation::Off,
             InterpolationFields fields = InterpolationFields::Geometry,
             unsigned rotation = 0);
    ~GpuVideo();
    GpuVideo(const GpuVideo &) = delete;
    GpuVideo &operator=(const GpuVideo &) = delete;
    const char *driver() const;

    // Host-only geometry. Assets, pipelines and canonical game state survive. Strong guarantee:
    // on failure the previous scale and every live resource are untouched.
    void set_scale(unsigned scale);
    void set_scale_mode(VideoScaleMode mode);
    // Accepted swapchain submissions.
    uint64_t presented_frames() const;
    // Host overlay is invoked on the window composition target after the game blit.
    using Overlay = GpuOverlay;
    SDL_GPUDevice *device() const;
    void set_linear(bool linear);
    // Live scanline interpolation mode and fields; takes effect on the next draw. Off/on
    // changes only switch the pipeline and coefficient upload, so assets and buffers persist.
    // Strong guarantee: if the pipeline cannot be built the previous mode stays.
    void set_interpolation(VideoInterpolation mode, InterpolationFields fields);
    void set_overlay(Overlay callback, void *userdata);
    // See the user postprocess shader ABI in the GPU backend. Errors throw and retain the last
    // valid preset.
    void set_postprocess(Postprocess preset, const std::filesystem::path &user_shader = {});
    Postprocess postprocess() const;

    // Renders `scene` offscreen and reads back width*height ARGB8888 pixels (unrotated), waiting on
    // the GPU fence before returning.
    void render(const SceneSnapshot &scene, std::span<uint32_t> output,
                const RenderRequest &request = {});
    // Presents `scene` to the window; see PresentRequest for skip-busy, live alpha, comparison
    // pane and window capture. Returns whether a swapchain texture was submitted.
    bool present(const SceneSnapshot &scene, const PresentRequest &request = {});
    // Renders the canonical scene (presented sprites, no motion) through postprocess and rotation
    // into `destination`, re-allocating it when its size differs. Offscreen; never presents.
    // `destination` must be destroyed before this GpuVideo.
    void capture_surface(const SceneSnapshot &scene, GpuTexture &destination);

    // Explicit opt-in; completed emulated frames are captured, never draws.
    void capture_motion(const SceneSnapshot &scene, uint64_t frame);
    void reset_motion();
    const MotionInterpolationStats &last_motion() const;
    // Pace before testing the native-frame deadline so the selected pair is
    // current at this display tick. The next presenting frame consumes this wait.
    // Returns false without an active native pacer; caller must use deadlines.
    bool pace_motion();
    // Accepted non-null swapchain submission in the last render/present, not scanout.
    bool last_presented() const;
    double display_hz() const;           // SDL current display mode, not observed cadence.
    double display_callback_hz() const;  // Observed macOS vsync callback cadence.
    double requested_display_hz() const; // Best-effort display-link request.
    // Saves the last rendered game image, including active postprocessing, but
    // excluding the crisp swapchain overlay. A preset/scale change needs a draw.
    void save_surface(const char *path);

private:
    rust::Box<rust_video::GpuVideo> m_video;
    std::string m_driver;

    mutable MotionInterpolationStats m_motion{};
};
} // namespace f3rt
