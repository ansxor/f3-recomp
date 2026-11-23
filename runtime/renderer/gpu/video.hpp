#pragma once
#include "renderer/game/scene_snapshot.hpp"
#include "renderer/gpu/encode.hpp"
#include "renderer/gpu/interp.hpp"
#include "renderer/gpu/motion.hpp"
#include "renderer/gpu/pacing.hpp"
#include "renderer/gpu/postprocess.hpp"
#include "renderer/gpu/scene_pass.hpp"
#include "renderer/gpu/sdl_gpu.hpp"
#include "renderer/scale.hpp"
#include "sprites/presentation.hpp"
#include <SDL3/SDL.h>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <vector>

namespace f3rt {
struct RenderRequest {
    SpriteList sprites = SpriteList::Presented;
    // Output/layer-mask diagnostics remain baseline unless `postprocess` is explicitly true.
    uint32_t layer_mask = ALL_LAYERS;
    // Temporal geometry at this alpha (needs capture_motion history); nullopt renders the canonical frame.
    std::optional<float> alpha = std::nullopt;
    bool postprocess = false;
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
    const InterpolationStats &last_interpolation() const;
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
    // See Postprocessor::configure for the user shader ABI. Errors throw and retain the last valid preset.
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
    EncodedScene prepare_scene(const SceneSnapshot &scene, SpriteList list, uint32_t layer_mask,
                               std::optional<float> alpha);

    GpuContext m_ctx;
    GameVideoOptions m_options;
    GpuTileAssets m_assets;
    ScenePass m_scene_pass;
    Postprocessor m_postprocess;
    SurfaceRotator m_rotator;
    ReadbackBuffer m_readback;
    DisplayPacer m_pacer;
    std::unique_ptr<GpuMotionHistory> m_motion;
    GpuPrepScratch m_prep_scratch;
    InterpolationConfig m_interp_config;
    InterpolationStats m_interpolation_stats{};
    InterpolationLog m_interp_log;
    MotionInterpolationStats m_motion_stats{};
    VideoScaleMode m_scale_mode = VideoScaleMode::Fixed;
    bool m_linear = false;
    bool m_rendered = false;
    bool m_last_presented = false;
    uint64_t m_presented_frames = 0;
    GpuSurface m_unrotated_surface{};
    std::vector<uint32_t> m_saved_pixels;
    Overlay m_overlay = nullptr;
    void *m_overlay_userdata = nullptr;
};
} // namespace f3rt
