//! Thin SDL3 C-struct adapter implementing the Rust `GpuDriverVtable`.
//!
//! Everything here is a direct translation of the former `sdl_gpu.cpp`, `pacing.cpp` and the
//! GPU clock calls; the renderer's lifetime, validation and pass logic live in Rust.

#include "renderer/gpu/sdl_driver_shim.hpp"
#include "renderer/gpu/pacing_macos.hpp"
#include "video_shaders.hpp"

#include <chrono>
#include <cstring>
#include <memory>

namespace f3rt {
namespace {

GpuDriverShim &shim(void *self) { return *static_cast<GpuDriverShim *>(self); }

SDL_GPUDevice *device(void *handle) { return static_cast<SDL_GPUDevice *>(handle); }
SDL_GPUTexture *texture(void *handle) { return static_cast<SDL_GPUTexture *>(handle); }
SDL_GPUBuffer *buffer(void *handle) { return static_cast<SDL_GPUBuffer *>(handle); }
SDL_GPUTransferBuffer *transfer(void *handle) {
    return static_cast<SDL_GPUTransferBuffer *>(handle);
}
SDL_GPUSampler *sampler(void *handle) { return static_cast<SDL_GPUSampler *>(handle); }
SDL_GPUGraphicsPipeline *pipeline(void *handle) {
    return static_cast<SDL_GPUGraphicsPipeline *>(handle);
}
SDL_GPUCommandBuffer *command(void *handle) {
    return static_cast<SDL_GPUCommandBuffer *>(handle);
}
SDL_GPUCopyPass *copy_pass(void *handle) { return static_cast<SDL_GPUCopyPass *>(handle); }
SDL_GPURenderPass *render_pass(void *handle) { return static_cast<SDL_GPURenderPass *>(handle); }

bool is_metal_device(SDL_GPUDevice *gpu) {
    return (SDL_GetGPUShaderFormats(gpu) & SDL_GPU_SHADERFORMAT_MSL) != 0;
}

SDL_GPUTextureFormat texture_format(uint32_t token) {
    switch (token) {
    case 1:
        return SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM;
    case 2:
        return SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM;
    case 3:
        return SDL_GPU_TEXTUREFORMAT_R32_UINT;
    default:
        return SDL_GPU_TEXTUREFORMAT_INVALID;
    }
}

SDL_GPUTransferBufferUsage transfer_usage(uint32_t token) {
    return token == 2 ? SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD
                      : SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD;
}

const video_shaders::Shader *shader(uint32_t kind) {
    switch (kind) {
    case 0:
        return &video_shaders::fullscreen_vert;
    case 1:
        return &video_shaders::sprite_vert;
    case 2:
        return &video_shaders::sprite_frag;
    case 3:
        return &video_shaders::scene_frag;
    case 4:
        return &video_shaders::scene_interp_frag;
    case 5:
        return &video_shaders::rotate_frag;
    case 6:
        return &video_shaders::postprocess_frag;
    default:
        return nullptr;
    }
}

SDL_GPUShader *create_shader(SDL_GPUDevice *gpu, const video_shaders::Shader &source,
                             SDL_GPUShaderStage stage) {
    SDL_GPUShaderCreateInfo info{};
    const bool metal = is_metal_device(gpu);
    info.format = metal ? SDL_GPU_SHADERFORMAT_MSL : SDL_GPU_SHADERFORMAT_SPIRV;
    info.code = metal ? reinterpret_cast<const Uint8 *>(source.msl) : source.spirv;
    info.code_size = metal ? source.msl_size : source.spirv_size;
    info.entrypoint = metal ? source.msl_entry : "main";
    info.stage = stage;
    info.num_samplers = source.samplers;
    info.num_storage_buffers = source.buffers;
    info.num_uniform_buffers = source.uniforms;
    return SDL_CreateGPUShader(gpu, &info);
}

SDL_GPULoadOp load_op(uint32_t value) {
    switch (value) {
    case 0:
        return SDL_GPU_LOADOP_CLEAR;
    case 1:
        return SDL_GPU_LOADOP_LOAD;
    default:
        return SDL_GPU_LOADOP_DONT_CARE;
    }
}

// --- device and window ------------------------------------------------------

void *create_device(void *) {
    return SDL_CreateGPUDevice(
        SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB,
        false, nullptr);
}

const char *last_error(void *) { return SDL_GetError(); }

bool claim_window(void *self, void *handle) {
    auto *gpu = device(handle);
    auto *window = shim(self).window;
    if (!window) return false;
    return SDL_ClaimWindowForGPUDevice(gpu, window);
}

bool set_swapchain_mode(void *self, void *handle, bool vsync) {
    auto *gpu = device(handle);
    auto *window = shim(self).window;
    if (!window || vsync) return true;
    // FIFO can make a slow display throttle native simulation/audio.
    // Mailbox remains tear-free without queuing obsolete display frames.
    const auto mode =
        SDL_WindowSupportsGPUPresentMode(gpu, window, SDL_GPU_PRESENTMODE_IMMEDIATE)
            ? SDL_GPU_PRESENTMODE_IMMEDIATE
        : SDL_WindowSupportsGPUPresentMode(gpu, window, SDL_GPU_PRESENTMODE_MAILBOX)
            ? SDL_GPU_PRESENTMODE_MAILBOX
            : SDL_GPU_PRESENTMODE_VSYNC;
    if (mode != SDL_GPU_PRESENTMODE_VSYNC &&
        !SDL_SetGPUSwapchainParameters(gpu, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
        return false;
    return true;
}

void release_window(void *self, void *handle) {
    auto *gpu = device(handle);
    auto *window = shim(self).window;
    if (gpu && window) SDL_ReleaseWindowFromGPUDevice(gpu, window);
}

void destroy_device(void *, void *handle) {
    if (auto *gpu = device(handle)) SDL_DestroyGPUDevice(gpu);
}

void wait_idle(void *, void *handle) {
    if (auto *gpu = device(handle)) SDL_WaitForGPUIdle(gpu);
}

bool is_metal(void *, void *handle) { return is_metal_device(device(handle)); }

const char *driver_name(void *, void *handle) { return SDL_GetGPUDeviceDriver(device(handle)); }

double display_hz(void *self) {
    auto *window = shim(self).window;
    if (!window) return 0;
    const auto *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    return mode ? mode->refresh_rate : 0;
}

uint32_t swapchain_format(void *self, void *handle) {
    auto *window = shim(self).window;
    if (!window) return 0;
    switch (SDL_GetGPUSwapchainTextureFormat(device(handle), window)) {
    case SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM:
        return 1;
    case SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM:
        return 2;
    default:
        return 0;
    }
}

// --- objects ----------------------------------------------------------------

void *create_sampler(void *, void *handle) {
    SDL_GPUSamplerCreateInfo sampling{};
    sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
    sampling.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
    sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w =
        SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
    return SDL_CreateGPUSampler(device(handle), &sampling);
}

void *create_buffer(void *, void *handle, uint32_t bytes) {
    SDL_GPUBufferCreateInfo info{};
    info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
    info.size = bytes;
    return SDL_CreateGPUBuffer(device(handle), &info);
}

void *create_texture(void *, void *handle, uint32_t format, uint32_t width, uint32_t height) {
    SDL_GPUTextureCreateInfo info{};
    info.type = SDL_GPU_TEXTURETYPE_2D;
    info.format = texture_format(format);
    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
    info.width = width;
    info.height = height;
    info.layer_count_or_depth = 1;
    info.num_levels = 1;
    info.sample_count = SDL_GPU_SAMPLECOUNT_1;
    return SDL_CreateGPUTexture(device(handle), &info);
}

void *create_transfer(void *, void *handle, uint32_t bytes, uint32_t usage) {
    SDL_GPUTransferBufferCreateInfo info{};
    info.usage = transfer_usage(usage);
    info.size = bytes;
    return SDL_CreateGPUTransferBuffer(device(handle), &info);
}

void *create_pipeline(void *, void *handle, uint32_t vertex, uint32_t fragment,
                      uint32_t format) {
    auto *gpu = device(handle);
    const auto *vertex_source = shader(vertex);
    const auto *fragment_source = shader(fragment);
    if (!vertex_source || !fragment_source) return nullptr;
    auto *vs = create_shader(gpu, *vertex_source, SDL_GPU_SHADERSTAGE_VERTEX);
    if (!vs) return nullptr;
    SDL_GPUShader *fs = nullptr;
    SDL_GPUGraphicsPipeline *created = nullptr;
    fs = create_shader(gpu, *fragment_source, SDL_GPU_SHADERSTAGE_FRAGMENT);
    if (fs) {
        SDL_GPUColorTargetDescription target{};
        target.format = texture_format(format);
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
        created = SDL_CreateGPUGraphicsPipeline(gpu, &info);
    }
    if (fs) SDL_ReleaseGPUShader(gpu, fs);
    SDL_ReleaseGPUShader(gpu, vs);
    return created;
}

void *create_user_pipeline(void *, void *handle, const uint8_t *code, size_t size,
                           const char *entry, uint32_t samplers, uint32_t buffers,
                           uint32_t uniforms, uint32_t format) {
    auto *gpu = device(handle);
    const bool metal = is_metal_device(gpu);
    auto *vs = create_shader(gpu, video_shaders::fullscreen_vert, SDL_GPU_SHADERSTAGE_VERTEX);
    if (!vs) return nullptr;
    SDL_GPUShaderCreateInfo info{};
    info.format = metal ? SDL_GPU_SHADERFORMAT_MSL : SDL_GPU_SHADERFORMAT_SPIRV;
    info.code = code;
    info.code_size = size;
    info.entrypoint = metal ? entry : "main";
    info.stage = SDL_GPU_SHADERSTAGE_FRAGMENT;
    info.num_samplers = samplers;
    info.num_storage_buffers = buffers;
    info.num_uniform_buffers = uniforms;
    SDL_GPUShader *fs = SDL_CreateGPUShader(gpu, &info);
    SDL_GPUGraphicsPipeline *created = nullptr;
    if (fs) {
        SDL_GPUColorTargetDescription target{};
        target.format = texture_format(format);
        SDL_GPUGraphicsPipelineCreateInfo pipeline_info{};
        pipeline_info.vertex_shader = vs;
        pipeline_info.fragment_shader = fs;
        pipeline_info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
        pipeline_info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
        pipeline_info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
        pipeline_info.rasterizer_state.enable_depth_clip = true;
        pipeline_info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
        pipeline_info.target_info.color_target_descriptions = &target;
        pipeline_info.target_info.num_color_targets = 1;
        created = SDL_CreateGPUGraphicsPipeline(gpu, &pipeline_info);
        SDL_ReleaseGPUShader(gpu, fs);
    }
    SDL_ReleaseGPUShader(gpu, vs);
    return created;
}

void release_buffer(void *, void *handle, void *object) {
    if (auto *gpu = device(handle)) SDL_ReleaseGPUBuffer(gpu, buffer(object));
}
void release_texture(void *, void *handle, void *object) {
    if (auto *gpu = device(handle)) SDL_ReleaseGPUTexture(gpu, texture(object));
}
void release_transfer(void *, void *handle, void *object) {
    if (auto *gpu = device(handle)) SDL_ReleaseGPUTransferBuffer(gpu, transfer(object));
}
void release_sampler(void *, void *handle, void *object) {
    if (auto *gpu = device(handle)) SDL_ReleaseGPUSampler(gpu, sampler(object));
}
void release_pipeline(void *, void *handle, void *object) {
    if (auto *gpu = device(handle)) SDL_ReleaseGPUGraphicsPipeline(gpu, pipeline(object));
}

void *map_transfer(void *, void *handle, void *object, bool cycle) {
    return SDL_MapGPUTransferBuffer(device(handle), transfer(object), cycle);
}

void unmap_transfer(void *, void *handle, void *object) {
    SDL_UnmapGPUTransferBuffer(device(handle), transfer(object));
}

// --- command buffers --------------------------------------------------------

void *acquire_command(void *, void *handle) {
    return SDL_AcquireGPUCommandBuffer(device(handle));
}

bool submit_command(void *object) { return SDL_SubmitGPUCommandBuffer(command(object)); }

bool cancel_command(void *object) { return SDL_CancelGPUCommandBuffer(command(object)); }

int32_t submit_and_wait(void *, void *handle, void *object) {
    auto *gpu = device(handle);
    auto *fence = SDL_SubmitGPUCommandBufferAndAcquireFence(command(object));
    if (!fence) return 1;
    const bool success = SDL_WaitForGPUFences(gpu, true, &fence, 1);
    SDL_ReleaseGPUFence(gpu, fence);
    return success ? 0 : 2;
}

int32_t acquire_swapchain(void *self, void *object, bool wait, GpuDriverSwapchain *out) {
    auto *window = shim(self).window;
    SDL_GPUTexture *acquired = nullptr;
    Uint32 width = 0, height = 0;
    if (!wait) {
        if (!window) return 1;
        if (!SDL_AcquireGPUSwapchainTexture(command(object), window, &acquired, &width, &height))
            return -1;
        if (!acquired) return 1;
    } else {
        if (window) {
            if (!SDL_WaitAndAcquireGPUSwapchainTexture(command(object), window, &acquired, &width,
                                                       &height))
                return -1;
            if (!acquired) return 1;
        } else {
            return 1;
        }
    }
    out->texture = acquired;
    out->width = width;
    out->height = height;
    return 0;
}

void *begin_copy_pass(void *object) { return SDL_BeginGPUCopyPass(command(object)); }
void end_copy_pass(void *object) { SDL_EndGPUCopyPass(copy_pass(object)); }

void *begin_render_pass(void *object, void *target, bool load) {
    SDL_GPUColorTargetInfo info{};
    info.texture = texture(target);
    info.load_op = load ? SDL_GPU_LOADOP_LOAD : SDL_GPU_LOADOP_CLEAR;
    info.store_op = SDL_GPU_STOREOP_STORE;
    info.clear_color = {0, 0, 0, 1};
    info.cycle = !load;
    return SDL_BeginGPURenderPass(command(object), &info, 1, nullptr);
}
void end_render_pass(void *object) { SDL_EndGPURenderPass(render_pass(object)); }

void upload_buffer(void *pass, void *source, uint32_t source_offset, void *destination,
                   uint32_t size, bool cycle) {
    const SDL_GPUTransferBufferLocation from{transfer(source), source_offset};
    const SDL_GPUBufferRegion to{buffer(destination), 0, size};
    SDL_UploadToGPUBuffer(copy_pass(pass), &from, &to, cycle);
}

void upload_texture(void *pass, void *source, uint32_t source_offset, uint32_t width,
                    uint32_t height, void *destination, bool cycle) {
    const SDL_GPUTextureTransferInfo from{transfer(source), source_offset, width, height};
    const SDL_GPUTextureRegion to{texture(destination), 0, 0, 0, 0, 0, width, height, 1};
    SDL_UploadToGPUTexture(copy_pass(pass), &from, &to, cycle);
}

void download_texture(void *pass, void *source, uint32_t width, uint32_t height,
                      void *destination) {
    const SDL_GPUTextureRegion from{texture(source), 0, 0, 0, 0, 0, width, height, 1};
    const SDL_GPUTextureTransferInfo to{transfer(destination), 0, width, height};
    SDL_DownloadFromGPUTexture(copy_pass(pass), &from, &to);
}

void push_vertex_uniforms(void *object, uint32_t slot, const void *data, size_t size) {
    SDL_PushGPUVertexUniformData(command(object), slot, data, Uint32(size));
}
void push_fragment_uniforms(void *object, uint32_t slot, const void *data, size_t size) {
    SDL_PushGPUFragmentUniformData(command(object), slot, data, Uint32(size));
}

void bind_pipeline(void *pass, void *object) {
    SDL_BindGPUGraphicsPipeline(render_pass(pass), pipeline(object));
}

void bind_vertex_storage_buffers(void *pass, uint32_t first_slot, void *const *buffers,
                                 size_t count) {
    SDL_GPUBuffer *const *typed = reinterpret_cast<SDL_GPUBuffer *const *>(buffers);
    SDL_BindGPUVertexStorageBuffers(render_pass(pass), first_slot, typed, Uint32(count));
}

void bind_fragment_storage_buffers(void *pass, uint32_t first_slot, void *const *buffers,
                                   size_t count) {
    SDL_GPUBuffer *const *typed = reinterpret_cast<SDL_GPUBuffer *const *>(buffers);
    SDL_BindGPUFragmentStorageBuffers(render_pass(pass), first_slot, typed, Uint32(count));
}

void bind_fragment_samplers(void *pass, uint32_t first_slot,
                            const GpuDriverTextureSamplerBinding *bindings, size_t count) {
    SDL_BindGPUFragmentSamplers(render_pass(pass), first_slot,
                                reinterpret_cast<const SDL_GPUTextureSamplerBinding *>(bindings),
                                Uint32(count));
}

void set_scissor(void *pass, int32_t x, int32_t y, int32_t width, int32_t height) {
    const SDL_Rect scissor{x, y, width, height};
    SDL_SetGPUScissor(render_pass(pass), &scissor);
}

void draw(void *pass, uint32_t vertices, uint32_t instances) {
    SDL_DrawGPUPrimitives(render_pass(pass), vertices, instances, 0, 0);
}

void blit(void *object, const GpuDriverBlit *request) {
    SDL_GPUBlitInfo info{};
    info.source = {request->source, 0, 0, request->source_x, request->source_y,
                   request->source_width, request->source_height};
    info.destination = {request->destination, 0, 0, request->x, request->y, request->width,
                        request->height};
    info.load_op = load_op(request->load_op);
    info.clear_color = {request->clear_color[0], request->clear_color[1],
                        request->clear_color[2], request->clear_color[3]};
    info.filter = request->filter == 1 ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
    SDL_BlitGPUTexture(command(object), &info);
}

void invoke_overlay(void *self, void *object, void *target, uint32_t width, uint32_t height) {
    auto &state = shim(self);
    if (state.overlay)
        state.overlay(state.overlay_userdata, command(object), texture(target), width, height);
}

int32_t save_surface(void *, const char *path, const uint32_t *pixels, uint32_t width,
                     uint32_t height, uint32_t pixel_format, bool png) {
    const SDL_PixelFormat format =
        pixel_format == 2 ? SDL_PIXELFORMAT_ABGR8888 : SDL_PIXELFORMAT_ARGB8888;
    std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
        SDL_CreateSurfaceFrom(int(width), int(height), format, const_cast<uint32_t *>(pixels),
                              int(width * 4)),
        SDL_DestroySurface);
    if (!image) return 1;
    const bool saved = png ? SDL_SavePNG(image.get(), path) : SDL_SaveBMP(image.get(), path);
    return saved ? 0 : 2;
}

bool set_frames_in_flight(void *, void *handle, uint32_t count) {
    return SDL_SetGPUAllowedFramesInFlight(device(handle), count);
}

// --- pacing -----------------------------------------------------------------

void *create_pacer(void *self) {
#ifdef __APPLE__
    auto *window = shim(self).window;
    return window ? create_macos_motion_pacing(window) : nullptr;
#else
    (void)self;
    return nullptr;
#endif
}

void destroy_pacer(void *, void *pacer) {
#ifdef __APPLE__
    destroy_macos_motion_pacing(pacer);
#else
    (void)pacer;
#endif
}

bool wait_pacer(void *, void *pacer) {
#ifdef __APPLE__
    return wait_macos_motion_pacing(pacer);
#else
    (void)pacer;
    return false;
#endif
}

bool poll_pacer(void *, void *pacer) {
#ifdef __APPLE__
    return poll_macos_motion_pacing(pacer);
#else
    (void)pacer;
    return false;
#endif
}

double pacer_callback_hz(void *, void *pacer) {
#ifdef __APPLE__
    return macos_motion_callback_hz(pacer);
#else
    (void)pacer;
    return 0;
#endif
}

double pacer_requested_hz(void *, void *pacer) {
#ifdef __APPLE__
    return macos_motion_requested_hz(pacer);
#else
    (void)pacer;
    return 0;
#endif
}

uint64_t monotonic_ns(void *) {
    return uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(
                        std::chrono::steady_clock::now().time_since_epoch())
                        .count());
}

uint64_t ticks_ns(void *) { return SDL_GetTicksNS(); }

const GpuDriverVtable TABLE{
    create_device,
    last_error,
    claim_window,
    set_swapchain_mode,
    release_window,
    destroy_device,
    wait_idle,
    is_metal,
    driver_name,
    display_hz,
    swapchain_format,
    create_sampler,
    create_buffer,
    create_texture,
    create_transfer,
    create_pipeline,
    create_user_pipeline,
    release_buffer,
    release_texture,
    release_transfer,
    release_sampler,
    release_pipeline,
    map_transfer,
    unmap_transfer,
    acquire_command,
    submit_command,
    cancel_command,
    submit_and_wait,
    acquire_swapchain,
    begin_copy_pass,
    end_copy_pass,
    begin_render_pass,
    end_render_pass,
    upload_buffer,
    upload_texture,
    download_texture,
    push_vertex_uniforms,
    push_fragment_uniforms,
    bind_pipeline,
    bind_vertex_storage_buffers,
    bind_fragment_storage_buffers,
    bind_fragment_samplers,
    set_scissor,
    draw,
    blit,
    invoke_overlay,
    save_surface,
    set_frames_in_flight,
    create_pacer,
    destroy_pacer,
    wait_pacer,
    poll_pacer,
    pacer_callback_hz,
    pacer_requested_hz,
    monotonic_ns,
    ticks_ns,
};

} // namespace

const GpuDriverVtable *sdl_driver_vtable() { return &TABLE; }

std::unique_ptr<GpuDriverShim> create_sdl_driver(SDL_Window *window) {
    auto driver = std::make_unique<GpuDriverShim>();
    driver->window = window;
    return driver;
}
} // namespace f3rt
