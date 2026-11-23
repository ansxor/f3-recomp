#pragma once
#include "renderer/gpu/sdl_gpu.hpp"
#include <cstdint>
#include <filesystem>

namespace f3rt {
enum class Postprocess { Off, Crt, User };

class Postprocessor {
public:
    // User ABI: .metal entry f3_postprocess on Metal, .spv entry main on Vulkan.
    // Fullscreen triangle, position builtin only; texture/sampler slot 0 and a
    // float4 uniform slot 0: internal width, height, scale, elapsed seconds.
    // Vulkan resources: combined sampler set 2/binding 0, uniform set 3/binding 0.
    // Files are limited to 1 MiB. Throws before mutating anything, retaining the last valid preset.
    void configure(const GpuContext &ctx, GameVideoOptions options, Postprocess preset,
                   const std::filesystem::path &user_shader);
    // A new target when a postprocess surface exists, else an empty texture.
    GpuTexture reallocate_target(const GpuContext &ctx, GameVideoOptions options) const;
    void commit_target(GpuTexture next) noexcept { m_surface = std::move(next); }
    bool active() const noexcept { return m_preset != Postprocess::Off; }
    Postprocess preset() const noexcept { return m_preset; }
    // Returns `input` unchanged unless a preset is active and `apply_now`.
    GpuSurface apply(GpuCommand &cmd, GpuSurface input, const GpuContext &ctx,
                     GameVideoOptions options, bool apply_now);

private:
    Postprocess m_preset = Postprocess::Off;
    GpuPipeline m_pipeline;
    GpuTexture m_surface;
    uint64_t m_start_ns = 0;
};

// Presentation rotation (90 or 270 degrees) into a portrait surface; passthrough when upright.
class SurfaceRotator {
public:
    // Throws std::invalid_argument unless `rotation` is 0, 90 or 270.
    SurfaceRotator(const GpuContext &ctx, GameVideoOptions options, unsigned rotation);
    static void validate(unsigned rotation);
    GpuTexture reallocate_target(const GpuContext &ctx, GameVideoOptions options) const;
    void commit_target(GpuTexture next) noexcept { m_surface = std::move(next); }
    bool rotated() const noexcept { return m_rotation != 0; }
    GpuSurface apply(GpuCommand &cmd, GpuSurface input, const GpuContext &ctx);

private:
    unsigned m_rotation = 0;
    GpuPipeline m_pipeline;
    GpuTexture m_surface;
};
}
