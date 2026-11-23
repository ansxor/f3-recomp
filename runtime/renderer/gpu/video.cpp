//! Thin C++ façade over `rust_video::GpuVideo`. All SDL3 calls live in
//! `crates/f3rt-video/src/gpu/`; the runtime links `SDL3::SDL3` through `f3rt_abi`.

#include "renderer/gpu/video.hpp"

#include <stdexcept>
#include <utility>

namespace f3rt {
namespace {
template <class F> void guarded(F &&call) {
    try {
        call();
    } catch (const rust::Error &error) {
        throw std::runtime_error(error.what());
    }
}

uintptr_t token(const void *pointer) { return reinterpret_cast<uintptr_t>(pointer); }

std::chrono::nanoseconds::rep nanos(std::chrono::nanoseconds duration) {
    return duration.count();
}

rust_video::Postprocess abi_postprocess(Postprocess preset) {
    switch (preset) {
    case Postprocess::Off:
        return rust_video::Postprocess::Off;
    case Postprocess::Crt:
        return rust_video::Postprocess::Crt;
    case Postprocess::User:
        return rust_video::Postprocess::User;
    }
    throw std::runtime_error("Unknown GPU postprocess preset");
}

Postprocess postprocess_from_abi(rust_video::Postprocess preset) {
    switch (preset) {
    case rust_video::Postprocess::Off:
        return Postprocess::Off;
    case rust_video::Postprocess::Crt:
        return Postprocess::Crt;
    case rust_video::Postprocess::User:
        return Postprocess::User;
    }
    return Postprocess::Off;
}
} // namespace

GpuTexture::GpuTexture() : m_texture(rust_video::new_gpu_texture()) {}
GpuTexture::~GpuTexture() = default;
GpuTexture::GpuTexture(GpuTexture &&other) noexcept = default;
GpuTexture &GpuTexture::operator=(GpuTexture &&other) noexcept = default;

GpuSurface GpuTexture::surface() const {
    return {m_texture->texture_handle(), m_texture->texture_width(),
            m_texture->texture_height()};
}

bool GpuTexture::empty() const { return m_texture->texture_handle() == 0; }

GpuVideo::GpuVideo(SDL_Window *window, GameVideoOptions options,
                   std::span<const uint8_t> playfield_assets,
                   std::span<const uint8_t> sprite_assets, bool linear, bool vsync,
                   VideoInterpolation interpolation, InterpolationFields fields,
                   unsigned rotation)
    : m_video([&]() {
          try {
              return rust_video::new_gpu_video(
                  token(window),
                  rust_video::GameVideoOptions{options.scale, options.border},
                  rust_video::GpuGeometry{geometry::FIRST_LINE, geometry::HEIGHT},
                  rust::Slice<const uint8_t>(playfield_assets.data(), playfield_assets.size()),
                  rust::Slice<const uint8_t>(sprite_assets.data(), sprite_assets.size()), linear,
                  vsync, interpolation, fields, rotation);
          } catch (const rust::Error &error) {
              throw std::runtime_error(error.what());
          }
      }()) {
    m_driver = std::string(m_video->driver().c_str());
}

GpuVideo::~GpuVideo() = default;

const char *GpuVideo::driver() const { return m_driver.c_str(); }

void GpuVideo::set_scale(unsigned scale) {
    guarded([&] { m_video->set_scale(scale); });
}

void GpuVideo::set_scale_mode(VideoScaleMode mode) {
    m_video->set_scale_mode(static_cast<rust_video::VideoScaleMode>(static_cast<int>(mode)));
}

uint64_t GpuVideo::presented_frames() const { return m_video->presented_frames(); }

SDL_GPUDevice *GpuVideo::device() const {
    return reinterpret_cast<SDL_GPUDevice *>(m_video->device());
}

void GpuVideo::set_linear(bool linear) { m_video->set_linear(linear); }

void GpuVideo::set_interpolation(VideoInterpolation mode, InterpolationFields fields) {
    guarded([&] { m_video->set_interpolation(mode, fields); });
}

void GpuVideo::set_overlay(Overlay callback, void *userdata) {
    m_video->set_overlay(token(reinterpret_cast<const void *>(callback)), token(userdata));
}

void GpuVideo::set_postprocess(Postprocess preset, const std::filesystem::path &user_shader) {
    guarded([&] { m_video->set_postprocess(abi_postprocess(preset), user_shader.string()); });
}

Postprocess GpuVideo::postprocess() const { return postprocess_from_abi(m_video->postprocess()); }

void GpuVideo::render(const SceneSnapshot &scene, std::span<uint32_t> output,
                      const RenderRequest &request) {
    rust_video::GpuRenderRequest abi{};
    abi.sprites = static_cast<rust_video::SpriteList>(static_cast<int>(request.sprites));
    abi.layer_mask = request.layer_mask;
    abi.has_alpha = request.alpha.has_value();
    abi.alpha = request.alpha.value_or(0.0f);
    abi.postprocess = request.postprocess;
    guarded([&] { m_video->render(scene.inner(), rust::Slice<uint32_t>(output.data(), output.size()),
                       abi); });
}

bool GpuVideo::present(const SceneSnapshot &scene, const PresentRequest &request) {
    rust_video::GpuPresentRequest abi{};
    abi.wait = request.wait;
    if (const auto *alpha = std::get_if<float>(&request.timing)) {
        abi.timing = rust_video::GpuPresentTiming::Fixed;
        abi.alpha = *alpha;
    } else if (const auto *clock = std::get_if<MotionLiveClock>(&request.timing)) {
        abi.timing = rust_video::GpuPresentTiming::Live;
        abi.frame_start_ns = static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                clock->frame_start.time_since_epoch())
                .count());
        abi.period_ns = static_cast<int64_t>(nanos(clock->period));
    } else {
        abi.timing = rust_video::GpuPresentTiming::Canonical;
    }
    abi.left_pane_handle = request.left_pane.texture;
    abi.left_pane_width = request.left_pane.width;
    abi.left_pane_height = request.left_pane.height;
    abi.capture_path =
        rust::String(request.capture_path ? request.capture_path : "");
    bool presented = false;
    guarded([&] { presented = m_video->present(scene.inner(), abi); });
    return presented;
}

void GpuVideo::capture_surface(const SceneSnapshot &scene, GpuTexture &destination) {
    guarded([&] { m_video->capture_surface(scene.inner(), *destination.inner()); });
}

void GpuVideo::capture_motion(const SceneSnapshot &scene, uint64_t frame) {
    guarded([&] { m_video->capture_motion(scene.inner(), frame); });
}

void GpuVideo::reset_motion() { m_video->reset_motion(); }

const MotionInterpolationStats &GpuVideo::last_motion() const {
    m_motion = m_video->last_motion();
    return m_motion;
}

bool GpuVideo::pace_motion() { return m_video->pace_motion(); }

bool GpuVideo::last_presented() const { return m_video->last_presented(); }

double GpuVideo::display_hz() const { return m_video->display_hz(); }

double GpuVideo::display_callback_hz() const { return m_video->display_callback_hz(); }

double GpuVideo::requested_display_hz() const { return m_video->requested_display_hz(); }

void GpuVideo::save_surface(const char *path) {
    guarded([&] { m_video->save_surface(path); });
}
} // namespace f3rt
