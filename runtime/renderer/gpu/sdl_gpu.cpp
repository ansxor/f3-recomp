#include "renderer/gpu/sdl_gpu.hpp"
#include "renderer/gpu/pacing.hpp"
#include <algorithm>
#include <cstring>
#include <memory>
#include <stdexcept>
#include <string>

namespace f3rt {
void gpu_fail(const char *operation) {
    throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}

void GpuTexture::ensure(const GpuContext &ctx, SDL_GPUTextureFormat format, uint32_t width,
                        uint32_t height) {
    if (get() && format == m_format && width == m_width && height == m_height) return;
    *this = ctx.create_texture(format, width, height);
}

GpuMapping GpuTransfer::map(bool cycle, const char *operation) const {
    return GpuMapping(m_device, get(),
                      gpu_checked(SDL_MapGPUTransferBuffer(m_device, get(), cycle), operation));
}

std::optional<float> PresentRequest::sample_alpha() const {
    if (const auto *alpha = std::get_if<float>(&timing)) return *alpha;
    const auto *clock = std::get_if<MotionLiveClock>(&timing);
    if (!clock) return std::nullopt;
    if (clock->period.count() <= 0) return 1.0f;
    const double elapsed =
        std::chrono::duration<double>(std::chrono::steady_clock::now() - clock->frame_start).count();
    return std::clamp(float(elapsed / std::chrono::duration<double>(clock->period).count()), 0.0f,
                      1.0f);
}

GpuContext::GpuContext(SDL_Window *window, bool vsync) : m_window(window), m_vsync(vsync) {
    m_device = gpu_checked(SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV |
                                                   SDL_GPU_SHADERFORMAT_MSL |
                                                   SDL_GPU_SHADERFORMAT_METALLIB,
                                               false, nullptr),
                           "Create SDL GPU device");
    try {
        if (window) {
            if (!SDL_ClaimWindowForGPUDevice(m_device, window)) gpu_fail("Claim GPU window");
            m_claimed = true;
            // FIFO can make a slow display throttle native simulation/audio.
            // Mailbox remains tear-free without queuing obsolete display frames.
            const auto mode =
                !vsync && SDL_WindowSupportsGPUPresentMode(m_device, window,
                                                           SDL_GPU_PRESENTMODE_IMMEDIATE)
                    ? SDL_GPU_PRESENTMODE_IMMEDIATE
                : SDL_WindowSupportsGPUPresentMode(m_device, window, SDL_GPU_PRESENTMODE_MAILBOX)
                    ? SDL_GPU_PRESENTMODE_MAILBOX
                    : SDL_GPU_PRESENTMODE_VSYNC;
            if (mode != SDL_GPU_PRESENTMODE_VSYNC &&
                !SDL_SetGPUSwapchainParameters(m_device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR,
                                               mode))
                gpu_fail("Set GPU presentation mode");
        }
        SDL_GPUSamplerCreateInfo sampling{};
        sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
        sampling.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w =
            SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        m_sampler = GpuSampler(m_device, gpu_checked(SDL_CreateGPUSampler(m_device, &sampling),
                                                     "Create GPU sampler"));
    } catch (...) {
        destroy();
        throw;
    }
}

GpuContext::~GpuContext() {
    wait_idle();
    destroy();
}

void GpuContext::destroy() noexcept {
    m_sampler.reset();
    if (m_claimed) SDL_ReleaseWindowFromGPUDevice(m_device, m_window);
    SDL_DestroyGPUDevice(m_device);
}

GpuCommand GpuContext::begin_command() const { return GpuCommand(m_device); }

GpuBuffer GpuContext::create_buffer(uint32_t bytes) const {
    SDL_GPUBufferCreateInfo info{};
    info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    info.size = bytes;
    return {m_device, gpu_checked(SDL_CreateGPUBuffer(m_device, &info), "Create GPU storage buffer")};
}

GpuTexture GpuContext::create_texture(SDL_GPUTextureFormat format, uint32_t width,
                                      uint32_t height) const {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = format;
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return {m_device, gpu_checked(SDL_CreateGPUTexture(m_device, &info), "Create GPU render texture"),
            format, width, height};
}

GpuTransfer GpuContext::create_transfer(uint32_t bytes, SDL_GPUTransferBufferUsage usage) const {
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = usage;
    info.size = bytes;
    return {m_device,
            gpu_checked(SDL_CreateGPUTransferBuffer(m_device, &info), "Create GPU transfer buffer"),
            bytes};
}

bool GpuContext::is_metal() const noexcept {
    return (SDL_GetGPUShaderFormats(m_device) & SDL_GPU_SHADERFORMAT_MSL) != 0;
}

SDL_GPUShader *GpuContext::create_shader(const video_shaders::Shader &source,
                                         SDL_GPUShaderStage stage) const {
    SDL_GPUShaderCreateInfo info{};
    const bool metal = is_metal();
    info.format = metal ? SDL_GPU_SHADERFORMAT_MSL : SDL_GPU_SHADERFORMAT_SPIRV;
    info.code = metal ? reinterpret_cast<const Uint8 *>(source.msl) : source.spirv;
    info.code_size = metal ? source.msl_size : source.spirv_size;
    info.entrypoint = metal ? source.msl_entry : "main";
    info.stage = stage;
    info.num_samplers = source.samplers;
    info.num_storage_buffers = source.buffers;
    info.num_uniform_buffers = source.uniforms;
    return gpu_checked(SDL_CreateGPUShader(m_device, &info), "Create GPU shader");
}

GpuPipeline GpuContext::create_pipeline(const video_shaders::Shader &vertex,
                                        const video_shaders::Shader &fragment,
                                        SDL_GPUTextureFormat format) const {
    auto *vs = create_shader(vertex, SDL_GPU_SHADERSTAGE_VERTEX);
    SDL_GPUShader *fs = nullptr;
    try {
        fs = create_shader(fragment, SDL_GPU_SHADERSTAGE_FRAGMENT);
        SDL_GPUColorTargetDescription target{};
        target.format = format;
        SDL_GPUGraphicsPipelineCreateInfo info{};
        info.vertex_shader = vs;
        info.fragment_shader = fs;
        info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        info.rasterizer_state.enable_depth_clip = true;
        info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        info.target_info.color_target_descriptions = &target;
        info.target_info.num_color_targets = 1;
        GpuPipeline result(m_device, gpu_checked(SDL_CreateGPUGraphicsPipeline(m_device, &info),
                                                 "Create GPU graphics pipeline"));
        SDL_ReleaseGPUShader(m_device, fs);
        SDL_ReleaseGPUShader(m_device, vs);
        return result;
    } catch (...) {
        if (fs) SDL_ReleaseGPUShader(m_device, fs);
        SDL_ReleaseGPUShader(m_device, vs);
        throw;
    }
}

GpuCommand::GpuCommand(SDL_GPUDevice *device)
    : m_device(device),
      m_cmd(gpu_checked(SDL_AcquireGPUCommandBuffer(device), "Acquire GPU command buffer")) {}

GpuCommand::GpuCommand(GpuCommand &&other) noexcept
    : m_device(other.m_device), m_cmd(std::exchange(other.m_cmd, nullptr)),
      m_swapchain(other.m_swapchain), m_acquired(other.m_acquired) {}

GpuCommand::~GpuCommand() {
    if (!m_cmd) return;
    if (m_acquired) SDL_SubmitGPUCommandBuffer(m_cmd);
    else SDL_CancelGPUCommandBuffer(m_cmd);
}

PresentAcquire GpuCommand::acquire_for_present(const GpuContext &ctx, DisplayPacer &pacer,
                                               const PresentRequest &request) {
    const bool pace_prepared = pacer.consume_prepared();
    SDL_Window *window = ctx.window();
    SDL_GPUTexture *texture = nullptr;
    Uint32 width = 0, height = 0;
    if (!request.wait) {
        if (!window) return PresentAcquire::SkipFrame;
        if (!SDL_AcquireGPUSwapchainTexture(m_cmd, window, &texture, &width, &height))
            gpu_fail("Try acquiring GPU swapchain");
        if (!texture) return PresentAcquire::SkipFrame;
        m_acquired = true;
        m_swapchain = {texture, width, height};
        return PresentAcquire::DrawableReady;
    }
    const bool timed = std::holds_alternative<MotionLiveClock>(request.timing);
    if (timed && !pace_prepared) pacer.wait_tick(ctx.vsync());
    if (window) {
        if (!SDL_WaitAndAcquireGPUSwapchainTexture(m_cmd, window, &texture, &width, &height))
            gpu_fail(timed ? "Acquire timed GPU swapchain" : "Acquire GPU swapchain");
        m_acquired = texture != nullptr;
        if (texture) {
            m_swapchain = {texture, width, height};
            return PresentAcquire::DrawableReady;
        }
    }
    return PresentAcquire::HeadlessProceed;
}

SDL_GPUCopyPass *GpuCommand::begin_copy_pass(const char *operation) {
    return gpu_checked(SDL_BeginGPUCopyPass(m_cmd), operation);
}

SDL_GPURenderPass *GpuCommand::begin_render_pass(SDL_GPUTexture *target, bool load) {
    SDL_GPUColorTargetInfo info{};
    info.texture = target;
    info.load_op = load ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
    info.store_op = SDL_GPU_STOREOP_STORE;
    info.clear_color = {0, 0, 0, 1};
    info.cycle = !load;
    return gpu_checked(SDL_BeginGPURenderPass(m_cmd, &info, 1, nullptr), "Begin GPU render pass");
}

void GpuCommand::upload_buffer(SDL_GPUCopyPass *pass, const GpuTransfer &source,
                               const GpuBuffer &destination, uint32_t offset, uint32_t size,
                               bool cycle) {
    const SDL_GPUTransferBufferLocation from{source.get(), offset};
    const SDL_GPUBufferRegion to{destination.get(), 0, size};
    SDL_UploadToGPUBuffer(pass, &from, &to, cycle);
}

void GpuCommand::blit_copy(GpuSurface source, GpuSurface destination) {
    SDL_GPUBlitInfo blit{};
    blit.source = {source.texture, 0, 0, 0, 0, source.width, source.height};
    blit.destination = {destination.texture, 0, 0, 0, 0, source.width, source.height};
    blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(m_cmd, &blit);
}

bool GpuCommand::present(const GpuContext &ctx, GpuSurface surface, GpuSurface left_pane,
                         VideoScaleMode scale_mode, GameVideoOptions options, bool linear,
                         bool rotated, GpuOverlay overlay, void *overlay_userdata,
                         const char *capture_path) {
    if (!m_swapchain) {
        submit();
        return false;
    }
    const Uint32 w = m_swapchain.width, h = m_swapchain.height;
    GpuTexture capture_texture;
    GpuTransfer capture_download;
    SDL_PixelFormat capture_format = SDL_PIXELFORMAT_UNKNOWN;
    SDL_GPUTexture *composition = m_swapchain.texture;
    if (capture_path) {
        const auto format = SDL_GetGPUSwapchainTextureFormat(m_device, ctx.window());
        if (format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM) capture_format = SDL_PIXELFORMAT_ARGB8888;
        else if (format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)
            capture_format = SDL_PIXELFORMAT_ABGR8888;
        else throw std::runtime_error("Window capture requires an 8-bit SDR swapchain");
        const uint64_t bytes = uint64_t(w) * h * 4;
        if (bytes > UINT32_MAX)
            throw std::runtime_error("Window capture exceeds GPU transfer size");
        capture_texture = ctx.create_texture(format, w, h);
        composition = capture_texture.get();
        capture_download = ctx.create_transfer(uint32_t(bytes), SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD);
    }
    const unsigned panes = left_pane && w >= 2 ? 2 : 1;
    for (unsigned pane = 0; pane < panes; ++pane) {
        const Uint32 offset = panes == 2 && pane == 1 ? w / 2 : 0;
        const Uint32 pane_width = panes == 1 ? w : (pane == 0 ? w / 2 : w - w / 2);
        const auto viewport = video_blit_for_window(scale_mode, options, pane_width, h, rotated);
        SDL_GPUBlitInfo blit{};
        blit.source = {panes == 2 && pane == 0 ? left_pane.texture : surface.texture,
                       0,
                       0,
                       viewport.source_x,
                       viewport.source_y,
                       viewport.source_width,
                       viewport.source_height};
        blit.destination = {composition,         0,          0,
                            offset + viewport.x, viewport.y, viewport.width,
                            viewport.height};
        blit.load_op = pane == 0 ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
        blit.clear_color = {0, 0, 0, 1};
        blit.filter = linear && scale_mode != VideoScaleMode::AutoInteger ? SDL_GPU_FILTER_LINEAR
                                                                          : SDL_GPU_FILTER_NEAREST;
        SDL_BlitGPUTexture(m_cmd, &blit);
    }
    if (overlay) overlay(overlay_userdata, m_cmd, composition, w, h);
    if (!capture_texture) {
        submit();
        return true;
    }
    // Download the exact full-window composition submitted to the drawable. Do not assume the
    // backend's swapchain permits readback or reconfigure its native layer.
    SDL_GPUBlitInfo final_blit{};
    final_blit.source = {composition, 0, 0, 0, 0, w, h};
    final_blit.destination = {m_swapchain.texture, 0, 0, 0, 0, w, h};
    final_blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
    final_blit.filter = SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(m_cmd, &final_blit);
    auto *copy = begin_copy_pass("Begin window capture download");
    const SDL_GPUTextureRegion from{composition, 0, 0, 0, 0, 0, w, h, 1};
    const SDL_GPUTextureTransferInfo to{capture_download.get(), 0, w, h};
    SDL_DownloadFromGPUTexture(copy, &from, &to);
    SDL_EndGPUCopyPass(copy);
    submit_and_wait();
    const auto pixels = capture_download.map(false, "Map window capture");
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
        gpu_checked(SDL_CreateSurfaceFrom(int(w), int(h), capture_format, pixels.data(), int(w * 4)),
                    "Create window capture image"),
        SDL_DestroySurface);
    if (!SDL_SavePNG(image.get(), capture_path)) gpu_fail("Save window capture PNG");
    return true;
}

void GpuCommand::submit() {
    if (!SDL_SubmitGPUCommandBuffer(std::exchange(m_cmd, nullptr))) gpu_fail("Submit GPU frame");
}

void GpuCommand::submit_and_wait() {
    auto *fence = gpu_checked(SDL_SubmitGPUCommandBufferAndAcquireFence(std::exchange(m_cmd, nullptr)),
                              "Submit GPU command buffer");
    const bool success = SDL_WaitForGPUFences(m_device, true, &fence, 1);
    SDL_ReleaseGPUFence(m_device, fence);
    if (!success) gpu_fail("Wait for GPU fence");
}

void ReadbackBuffer::finish(GpuCommand &cmd, GpuSurface surface, const GpuContext &ctx,
                            std::span<uint32_t> output) {
    const size_t pixels = size_t(surface.width) * surface.height;
    if (output.size() < pixels) throw std::runtime_error("Incomplete GPU output buffer");
    const uint32_t bytes = uint32_t(pixels * 4);
    if (m_download.size() != bytes) m_download = ctx.create_transfer(bytes, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD);
    auto *copy = cmd.begin_copy_pass("Begin GPU readback copy");
    const SDL_GPUTextureRegion from{surface.texture, 0, 0, 0, 0, 0, surface.width, surface.height, 1};
    const SDL_GPUTextureTransferInfo to{m_download.get(), 0, surface.width, surface.height};
    SDL_DownloadFromGPUTexture(copy, &from, &to);
    SDL_EndGPUCopyPass(copy);
    cmd.submit_and_wait();
    const auto mapped = m_download.map(false, "Map GPU readback");
    std::memcpy(output.data(), mapped.data(), bytes);
}
}
