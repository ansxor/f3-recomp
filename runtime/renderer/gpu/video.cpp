#include "renderer/gpu/video.hpp"
#include <cstring>
#include <stdexcept>

namespace f3rt {
namespace {
void validate_gpu_video_config(GameVideoOptions options, unsigned rotation,
                               std::span<const uint8_t> playfield_assets,
                               std::span<const uint8_t> sprite_assets) {
    SurfaceRotator::validate(rotation);
    if (!options.scale || options.scale > GameVideoOptions::MAX_GPU_SCALE ||
        options.border > GameVideoOptions::MAX_BORDER)
        throw std::runtime_error("Game GPU presentation scale/border out of range");
    GpuTileAssets::validate(playfield_assets, sprite_assets);
}
} // namespace

GpuVideo::GpuVideo(SDL_Window *window, GameVideoOptions options,
                   std::span<const uint8_t> playfield_assets,
                   std::span<const uint8_t> sprite_assets, bool linear, bool vsync,
                   VideoInterpolation interpolation, InterpolationFields fields, unsigned rotation)
    : m_ctx((validate_gpu_video_config(options, rotation, playfield_assets, sprite_assets), window),
            vsync),
      m_options(options), m_assets(m_ctx, playfield_assets, sprite_assets),
      m_scene_pass(m_ctx, options, interpolation != VideoInterpolation::Off),
      m_rotator(m_ctx, options, rotation), m_interp_config{interpolation, fields},
      m_linear(linear) {}

GpuVideo::~GpuVideo() { m_ctx.wait_idle(); }

const char *GpuVideo::driver() const { return m_ctx.driver(); }
const InterpolationStats &GpuVideo::last_interpolation() const { return m_interpolation_stats; }
void GpuVideo::set_scale_mode(VideoScaleMode mode) { m_scale_mode = mode; }
uint64_t GpuVideo::presented_frames() const { return m_presented_frames; }
SDL_GPUDevice *GpuVideo::device() const { return m_ctx.device(); }
void GpuVideo::set_linear(bool linear) { m_linear = linear; }
Postprocess GpuVideo::postprocess() const { return m_postprocess.preset(); }
void GpuVideo::set_overlay(Overlay callback, void *userdata) {
    m_overlay = callback;
    m_overlay_userdata = userdata;
}
const MotionInterpolationStats &GpuVideo::last_motion() const { return m_motion_stats; }
bool GpuVideo::last_presented() const { return m_last_presented; }
double GpuVideo::display_hz() const { return m_pacer.display_hz(m_ctx.window()); }
double GpuVideo::display_callback_hz() const { return m_pacer.callback_hz(); }
double GpuVideo::requested_display_hz() const { return m_pacer.requested_hz(); }

void GpuVideo::set_scale(unsigned scale) {
    if (!scale || scale > GameVideoOptions::MAX_GPU_SCALE)
        throw std::runtime_error("GPU scale must be 1..8");
    if (scale == m_options.scale) return;
    GameVideoOptions next = m_options;
    next.scale = scale;
    auto next_scene = m_scene_pass.reallocate_targets(m_ctx, next);
    auto next_post = m_postprocess.reallocate_target(m_ctx, next);
    auto next_rotated = m_rotator.reallocate_target(m_ctx, next);
    std::vector<uint32_t> next_pixels;
    if (!m_saved_pixels.empty()) next_pixels.resize(size_t(next.width()) * next.height());
    // SDL defers destruction of the replaced targets until queued users finish: no GPU-idle stall.
    m_scene_pass.commit_targets(std::move(next_scene));
    m_postprocess.commit_target(std::move(next_post));
    m_rotator.commit_target(std::move(next_rotated));
    m_readback.reset();
    m_saved_pixels.swap(next_pixels);
    m_options = next;
    m_rendered = false;
    m_unrotated_surface = {};
    m_interpolation_stats = {};
}

void GpuVideo::set_interpolation(VideoInterpolation mode, InterpolationFields fields) {
    const InterpolationConfig next{mode, fields};
    if (next == m_interp_config) return;
    if (mode != VideoInterpolation::Off)
        m_scene_pass.ensure_interpolation_pipeline(m_ctx);
    m_interp_config = next;
    m_interpolation_stats = {};
    m_interp_log.reset();
}

void GpuVideo::set_postprocess(Postprocess preset, const std::filesystem::path &user_shader) {
    m_postprocess.configure(m_ctx, m_options, preset, user_shader);
    m_rendered = false;
    m_unrotated_surface = {};
}

EncodedScene GpuVideo::prepare_scene(const SceneSnapshot &scene, SpriteList list,
                                     uint32_t layer_mask, std::optional<float> alpha) {
    if (scene.sprite_list(list).size() > F3_SCENE_SPRITE_COUNT)
        throw std::runtime_error("GPU sprite count out of range");
    if (scene.fallback && m_motion) m_motion->reset();
    const ScenePlan plan{scene, m_options, list, layer_mask};
    const PreparedScene prepared =
        plan.apply_motion(m_motion.get(), alpha, m_prep_scratch.motion_state)
            .analyze_interpolation(m_interp_config, m_assets.tile_pen_masks(),
                                   m_prep_scratch.interp_coefficients);
    EncodedScene encoded;
    {
        const auto upload = m_scene_pass.upload_buffer().map(true, "Map GPU frame upload");
        encoded = prepared.encode_into(upload.words(prepared.required_words()),
                                       m_assets.pf_tile_count(), m_assets.sp_tile_count());
    }
    if (alpha) m_motion_stats = encoded.motion_stats;
    m_interpolation_stats = encoded.interp_stats;
    m_interp_log.observe(m_interp_config, m_interpolation_stats);
    return encoded;
}

void GpuVideo::render(const SceneSnapshot &scene, std::span<uint32_t> output,
                      const RenderRequest &request) {
    if (output.size() < size_t(m_options.width()) * m_options.height())
        throw std::runtime_error("Incomplete GPU output buffer");
    m_last_presented = false;
    const EncodedScene encoded =
        prepare_scene(scene, request.sprites, request.layer_mask, request.alpha);
    auto cmd = m_ctx.begin_command();
    const GpuSurface composed = m_scene_pass.draw(cmd, encoded, m_ctx, m_assets, m_options);
    const GpuSurface final_surface = m_postprocess.apply(
        cmd, composed, m_ctx, m_options, m_postprocess.active() && request.postprocess);
    m_readback.finish(cmd, final_surface, m_ctx, output);
    m_unrotated_surface = final_surface;
    m_rendered = true;
}

bool GpuVideo::present(const SceneSnapshot &scene, const PresentRequest &request) {
    m_last_presented = false;
    if (!std::holds_alternative<std::monostate>(request.timing)) m_motion_stats = {};
    auto cmd = m_ctx.begin_command();
    if (cmd.acquire_for_present(m_ctx, m_pacer, request) == PresentAcquire::SkipFrame) return false;
    const EncodedScene encoded = prepare_scene(scene, SpriteList::Presented, ALL_LAYERS,
                                               request.sample_alpha());
    const GpuSurface composed = m_scene_pass.draw(cmd, encoded, m_ctx, m_assets, m_options);
    const GpuSurface post =
        m_postprocess.apply(cmd, composed, m_ctx, m_options, m_postprocess.active());
    const GpuSurface oriented = m_rotator.apply(cmd, post, m_ctx);
    m_last_presented = cmd.present(m_ctx, oriented, request.left_pane, m_scale_mode, m_options,
                                   m_linear, m_rotator.rotated(), m_overlay, m_overlay_userdata,
                                   request.capture_path);
    m_presented_frames += m_last_presented;
    m_unrotated_surface = post;
    m_rendered = true;
    return m_last_presented;
}

void GpuVideo::capture_surface(const SceneSnapshot &scene, GpuTexture &destination) {
    const EncodedScene encoded =
        prepare_scene(scene, SpriteList::Presented, ALL_LAYERS, std::nullopt);
    auto cmd = m_ctx.begin_command();
    const GpuSurface composed = m_scene_pass.draw(cmd, encoded, m_ctx, m_assets, m_options);
    const GpuSurface post =
        m_postprocess.apply(cmd, composed, m_ctx, m_options, m_postprocess.active());
    const GpuSurface oriented = m_rotator.apply(cmd, post, m_ctx);
    destination.ensure(m_ctx, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, oriented.width, oriented.height);
    cmd.blit_copy(oriented, destination.surface());
    cmd.submit();
}

void GpuVideo::capture_motion(const SceneSnapshot &scene, uint64_t frame) {
    if (!m_motion) {
        m_pacer.enable(m_ctx.device(), m_ctx.window(), m_ctx.vsync());
        m_motion = std::make_unique<GpuMotionHistory>(m_options);
    }
    if (scene.sprite_trail_seed.empty()) m_motion->capture(scene, frame);
    else m_motion->reset();
}

void GpuVideo::reset_motion() {
    if (m_motion) m_motion->reset();
    m_motion_stats = {};
    m_pacer.reset_prepared();
}

bool GpuVideo::pace_motion() { return m_pacer.pace(m_ctx.vsync()); }

void GpuVideo::save_surface(const char *path) {
    if (!m_rendered) throw std::runtime_error("No GPU surface has been drawn");
    auto cmd = m_ctx.begin_command();
    const GpuSurface source = m_rotator.apply(cmd, m_unrotated_surface, m_ctx);
    m_saved_pixels.resize(size_t(source.width) * source.height);
    m_readback.finish(cmd, source, m_ctx, m_saved_pixels);
    auto *surface = gpu_checked(SDL_CreateSurfaceFrom(int(source.width), int(source.height),
                                                      SDL_PIXELFORMAT_ARGB8888,
                                                      m_saved_pixels.data(), int(source.width * 4)),
                                "Create GPU capture surface");
    const char *extension = std::strrchr(path, '.');
    const bool saved = extension && SDL_strcasecmp(extension, ".png") == 0
                           ? SDL_SavePNG(surface, path)
                           : SDL_SaveBMP(surface, path);
    SDL_DestroySurface(surface);
    if (!saved) gpu_fail("Save GPU surface");
}
} // namespace f3rt
