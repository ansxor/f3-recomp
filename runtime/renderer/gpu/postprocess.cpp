#include "renderer/gpu/postprocess.hpp"
#include "renderer/gpu/user_spirv.hpp"
#include <array>
#include <stdexcept>

namespace f3rt {
namespace {
constexpr SDL_GPUTextureFormat TARGET_FORMAT = SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;

GpuPipeline user_pipeline(const GpuContext &ctx, const std::filesystem::path &path) {
    const bool metal = ctx.is_metal();
    const LoadedUserShader loaded = load_user_shader(path, metal);
    const video_shaders::Shader source{reinterpret_cast<const uint8_t *>(loaded.code.data()),
                                       loaded.size,
                                       reinterpret_cast<const char *>(loaded.code.data()),
                                       loaded.size,
                                       "f3_postprocess",
                                       1,
                                       0,
                                       1};
    return ctx.create_pipeline(video_shaders::fullscreen_vert, source, TARGET_FORMAT);
}
}

void Postprocessor::configure(const GpuContext &ctx, GameVideoOptions options, Postprocess preset,
                              const std::filesystem::path &user_shader) {
    if (preset != Postprocess::Off && preset != Postprocess::Crt && preset != Postprocess::User)
        throw std::runtime_error("Unknown GPU postprocess preset");
    if (preset == Postprocess::Off) {
        m_pipeline.reset();
        m_surface.reset();
        m_preset = preset;
        return;
    }
    GpuPipeline next_pipeline =
        preset == Postprocess::Crt
            ? ctx.create_pipeline(video_shaders::fullscreen_vert, video_shaders::postprocess_frag,
                                  TARGET_FORMAT)
            : user_pipeline(ctx, user_shader);
    GpuTexture next_surface;
    if (!m_surface) next_surface = ctx.create_texture(TARGET_FORMAT, options.width(), options.height());
    // SDL defers destruction of the replaced pipeline until pending commands finish.
    m_pipeline = std::move(next_pipeline);
    if (next_surface) m_surface = std::move(next_surface);
    m_preset = preset;
    m_start_ns = SDL_GetTicksNS();
}

GpuTexture Postprocessor::reallocate_target(const GpuContext &ctx, GameVideoOptions options) const {
    if (!m_surface) return {};
    return ctx.create_texture(TARGET_FORMAT, options.width(), options.height());
}

GpuSurface Postprocessor::apply(GpuCommand &cmd, GpuSurface input, const GpuContext &ctx,
                                GameVideoOptions options, bool apply_now) {
    if (!active() || !apply_now) return input;
    const float parameters[]{float(options.width()), float(options.height()), float(options.scale),
                             float(double(SDL_GetTicksNS() - m_start_ns) / 1.0e9)};
    SDL_PushGPUFragmentUniformData(cmd.raw(), 0, parameters, sizeof(parameters));
    auto *pass = cmd.begin_render_pass(m_surface.get());
    SDL_BindGPUGraphicsPipeline(pass, m_pipeline.get());
    const SDL_GPUTextureSamplerBinding binding{input.texture, ctx.sampler()};
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    return m_surface.surface();
}

void SurfaceRotator::validate(unsigned rotation) {
    if (rotation != 0 && rotation != 90 && rotation != 270)
        throw std::invalid_argument("Unsupported GPU presentation rotation");
}

SurfaceRotator::SurfaceRotator(const GpuContext &ctx, GameVideoOptions options, unsigned rotation)
    : m_rotation(rotation) {
    validate(rotation);
    if (!rotation) return;
    m_pipeline = ctx.create_pipeline(video_shaders::fullscreen_vert, video_shaders::rotate_frag,
                                     TARGET_FORMAT);
    m_surface = reallocate_target(ctx, options);
}

GpuTexture SurfaceRotator::reallocate_target(const GpuContext &ctx, GameVideoOptions options) const {
    if (!m_rotation) return {};
    return ctx.create_texture(TARGET_FORMAT, options.height(), options.width());
}

GpuSurface SurfaceRotator::apply(GpuCommand &cmd, GpuSurface input, const GpuContext &ctx) {
    if (!m_rotation) return input;
    const std::array<Uint32, 4> uniform{m_rotation};
    SDL_PushGPUFragmentUniformData(cmd.raw(), 0, uniform.data(), sizeof(uniform));
    auto *pass = cmd.begin_render_pass(m_surface.get());
    SDL_BindGPUGraphicsPipeline(pass, m_pipeline.get());
    const SDL_GPUTextureSamplerBinding binding{input.texture, ctx.sampler()};
    SDL_BindGPUFragmentSamplers(pass, 0, &binding, 1);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    return m_surface.surface();
}
}
