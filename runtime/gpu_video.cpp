#include "gpu_video.hpp"
#include "video_shaders.hpp"
#include <algorithm>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace f3rt {
namespace {
[[noreturn]] void fail(const char *operation) {
    throw std::runtime_error(std::string(operation) + ": " + SDL_GetError());
}
template<class T> T *checked(T *value, const char *operation) {
    if (!value) fail(operation);
    return value;
}
struct Command {
    SDL_GPUCommandBuffer *value;
    bool acquired_swapchain = false;
    explicit Command(SDL_GPUDevice *device) : value(checked(SDL_AcquireGPUCommandBuffer(device), "Acquire GPU command buffer")) {}
    ~Command() {
        if (value) {
            if (acquired_swapchain) SDL_SubmitGPUCommandBuffer(value);
            else SDL_CancelGPUCommandBuffer(value);
        }
    }
    SDL_GPUCommandBuffer *take() { auto *result = value; value = nullptr; return result; }
};
constexpr Uint32 asset_bytes = 32768 * 256;
constexpr Uint32 scene_bytes = GpuScene::word_count * sizeof(uint32_t);
constexpr Uint32 native_bytes = 320 * 232 * sizeof(uint32_t);
}

struct GpuVideo::Impl {
    SDL_GPUDevice *device = nullptr;
    SDL_Window *window = nullptr;
    bool claimed = false, linear = false, rendered = false;
    bool vsync = true;
    GameVideoOptions options{};
    VideoInterpolation interpolation = VideoInterpolation::Off;
    InterpolationStats interpolation_stats{};
    std::vector<uint64_t> tile_pen_masks;
    SDL_GPUBuffer *scene_buffer = nullptr, *pf_assets = nullptr, *sp_assets = nullptr, *native_buffer = nullptr;
    SDL_GPUTexture *sprite_plane = nullptr, *surface = nullptr;
    SDL_GPUSampler *sampler = nullptr;
    SDL_GPUGraphicsPipeline *sprite_pipeline = nullptr, *scene_pipeline = nullptr;
    SDL_GPUTransferBuffer *upload = nullptr, *download = nullptr;
    std::vector<uint32_t> saved_pixels;

    ~Impl() {
        if (!device) return;
        SDL_WaitForGPUIdle(device);
        if (sprite_pipeline) SDL_ReleaseGPUGraphicsPipeline(device, sprite_pipeline);
        if (scene_pipeline) SDL_ReleaseGPUGraphicsPipeline(device, scene_pipeline);
        if (sampler) SDL_ReleaseGPUSampler(device, sampler);
        if (sprite_plane) SDL_ReleaseGPUTexture(device, sprite_plane);
        if (surface) SDL_ReleaseGPUTexture(device, surface);
        for (auto *buffer : {scene_buffer, pf_assets, sp_assets, native_buffer})
            if (buffer) SDL_ReleaseGPUBuffer(device, buffer);
        if (upload) SDL_ReleaseGPUTransferBuffer(device, upload);
        if (download) SDL_ReleaseGPUTransferBuffer(device, download);
        if (claimed) SDL_ReleaseWindowFromGPUDevice(device, window);
        SDL_DestroyGPUDevice(device);
    }
    SDL_GPUBuffer *buffer(Uint32 size) {
        SDL_GPUBufferCreateInfo info{};
        info.usage = SDL_GPU_BUFFERUSAGE_GRAPHICS_STORAGE_READ;
        info.size = size;
        return checked(SDL_CreateGPUBuffer(device, &info), "Create GPU storage buffer");
    }
    SDL_GPUTransferBuffer *transfer(Uint32 size, SDL_GPUTransferBufferUsage usage) {
        SDL_GPUTransferBufferCreateInfo info{};
        info.usage = usage; info.size = size;
        return checked(SDL_CreateGPUTransferBuffer(device, &info), "Create GPU transfer buffer");
    }
    SDL_GPUTexture *texture(SDL_GPUTextureFormat format, bool sprite = false) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D; info.format = format;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        bool native_plane = sprite && !options.expanded();
        info.width = native_plane ? 432 : options.width(); info.height = native_plane ? 256 : options.height();
        info.layer_count_or_depth = 1; info.num_levels = 1; info.sample_count = SDL_GPU_SAMPLECOUNT_1;
        return checked(SDL_CreateGPUTexture(device, &info), "Create GPU render texture");
    }
    SDL_GPUShader *shader(const video_shaders::Shader &source, SDL_GPUShaderStage stage) {
        SDL_GPUShaderCreateInfo info{};
        bool metal = (SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_MSL) != 0;
        info.format = metal ? SDL_GPU_SHADERFORMAT_MSL : SDL_GPU_SHADERFORMAT_SPIRV;
        info.code = metal ? reinterpret_cast<const Uint8 *>(source.msl) : source.spirv;
        info.code_size = metal ? source.msl_size : source.spirv_size;
        info.entrypoint = metal ? source.msl_entry : "main";
        info.stage = stage; info.num_samplers = source.samplers;
        info.num_storage_buffers = source.buffers; info.num_uniform_buffers = source.uniforms;
        return checked(SDL_CreateGPUShader(device, &info), "Create GPU shader");
    }
    SDL_GPUGraphicsPipeline *pipeline(const video_shaders::Shader &vertex, const video_shaders::Shader &fragment,
                                      SDL_GPUTextureFormat format) {
        auto *vs = shader(vertex, SDL_GPU_SHADERSTAGE_VERTEX);
        SDL_GPUShader *fs = nullptr;
        try {
            fs = shader(fragment, SDL_GPU_SHADERSTAGE_FRAGMENT);
            SDL_GPUColorTargetDescription target{}; target.format = format;
            SDL_GPUGraphicsPipelineCreateInfo info{};
            info.vertex_shader = vs; info.fragment_shader = fs;
            info.primitive_type = SDL_GPU_PRIMITIVETYPE_TRIANGLELIST;
            info.rasterizer_state.fill_mode = SDL_GPU_FILLMODE_FILL;
            info.rasterizer_state.cull_mode = SDL_GPU_CULLMODE_NONE;
            info.rasterizer_state.enable_depth_clip = true;
            info.multisample_state.sample_count = SDL_GPU_SAMPLECOUNT_1;
            info.target_info.color_target_descriptions = &target; info.target_info.num_color_targets = 1;
            auto *result = checked(SDL_CreateGPUGraphicsPipeline(device, &info), "Create GPU graphics pipeline");
            SDL_ReleaseGPUShader(device, fs); SDL_ReleaseGPUShader(device, vs);
            return result;
        } catch (...) {
            if (fs) SDL_ReleaseGPUShader(device, fs);
            SDL_ReleaseGPUShader(device, vs);
            throw;
        }
    }
    void upload_buffer(SDL_GPUCopyPass *pass, SDL_GPUTransferBuffer *source, SDL_GPUBuffer *destination,
                       Uint32 offset, Uint32 size, bool cycle) {
        SDL_GPUTransferBufferLocation from{source, offset};
        SDL_GPUBufferRegion to{destination, 0, size};
        SDL_UploadToGPUBuffer(pass, &from, &to, cycle);
    }
    void init(std::span<const uint8_t> tiles, std::span<const uint8_t> sprites) {
        if (!options.scale || options.scale > GameVideoOptions::max_scale || options.border > GameVideoOptions::max_border)
            throw std::runtime_error("Game GPU presentation scale/border out of range");
        if (tiles.size() < asset_bytes || sprites.size() < asset_bytes)
            throw std::runtime_error("Incomplete GPU tile/sprite assets");
        device = checked(SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL, false, nullptr), "Create SDL GPU device");
        if (window) {
            if (!SDL_ClaimWindowForGPUDevice(device, window)) fail("Claim GPU window");
            claimed = true;
            if (!vsync && SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_IMMEDIATE) &&
                !SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, SDL_GPU_PRESENTMODE_IMMEDIATE))
                fail("Set unthrottled GPU presentation");
        }
        scene_buffer = buffer(scene_bytes); native_buffer = buffer(native_bytes);
        pf_assets = buffer(asset_bytes); sp_assets = buffer(asset_bytes);
        sprite_plane = texture(SDL_GPU_TEXTUREFORMAT_R16_UINT, true);
        surface = texture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);
        SDL_GPUSamplerCreateInfo sampling{};
        sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
        sampling.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler = checked(SDL_CreateGPUSampler(device, &sampling), "Create GPU sampler");
        sprite_pipeline = pipeline(video_shaders::sprite_vert, video_shaders::sprite_frag, SDL_GPU_TEXTUREFORMAT_R16_UINT);
        const auto &fragment = interpolation == VideoInterpolation::Off || options.scale == 1
            ? video_shaders::scene_frag : video_shaders::scene_interp_frag;
        scene_pipeline = pipeline(video_shaders::fullscreen_vert, fragment, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);
        upload = transfer(asset_bytes * 2, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
        download = transfer(options.width() * options.height() * 4, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD);
        saved_pixels.resize(size_t(options.width()) * options.height());
        if (interpolation != VideoInterpolation::Off && options.scale > 1) tile_pen_masks.resize(32768);
        auto *mapped = static_cast<uint32_t *>(checked(SDL_MapGPUTransferBuffer(device, upload, false), "Map GPU asset upload"));
        // Explicit low-byte-first packing also works on big-endian hosts.
        for (Uint32 i = 0; i < asset_bytes; i += 4) {
            mapped[i / 4] = uint32_t(tiles[i]) | (uint32_t(tiles[i + 1]) << 8) | (uint32_t(tiles[i + 2]) << 16) | (uint32_t(tiles[i + 3]) << 24);
            mapped[(asset_bytes + i) / 4] = uint32_t(sprites[i]) | (uint32_t(sprites[i + 1]) << 8) | (uint32_t(sprites[i + 2]) << 16) | (uint32_t(sprites[i + 3]) << 24);
            if (!tile_pen_masks.empty())
                tile_pen_masks[i / 256] |= (uint64_t{1} << (tiles[i] & 63)) |
                    (uint64_t{1} << (tiles[i + 1] & 63)) | (uint64_t{1} << (tiles[i + 2] & 63)) |
                    (uint64_t{1} << (tiles[i + 3] & 63));
        }
        SDL_UnmapGPUTransferBuffer(device, upload);
        Command command(device);
        auto *copy = checked(SDL_BeginGPUCopyPass(command.value), "Begin GPU asset copy");
        upload_buffer(copy, upload, pf_assets, 0, asset_bytes, false);
        upload_buffer(copy, upload, sp_assets, asset_bytes, asset_bytes, false);
        SDL_EndGPUCopyPass(copy);
        if (!SDL_SubmitGPUCommandBuffer(command.take())) fail("Submit GPU asset upload");
        SDL_ReleaseGPUTransferBuffer(device, upload);
        upload = nullptr;
        upload = transfer(std::max(scene_bytes, native_bytes), SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
    }
    SDL_GPURenderPass *render_pass(SDL_GPUCommandBuffer *command, SDL_GPUTexture *target) {
        SDL_GPUColorTargetInfo info{};
        info.texture = target; info.load_op = SDL_GPU_LOADOP_CLEAR; info.store_op = SDL_GPU_STOREOP_STORE;
        info.clear_color = {0, 0, 0, 1}; info.cycle = true;
        return checked(SDL_BeginGPURenderPass(command, &info, 1, nullptr), "Begin GPU render pass");
    }
    void queue_download(SDL_GPUCommandBuffer *command) {
        auto *copy = checked(SDL_BeginGPUCopyPass(command), "Begin GPU readback copy");
        SDL_GPUTextureRegion from{surface, 0, 0, 0, 0, 0, options.width(), options.height(), 1};
        SDL_GPUTextureTransferInfo to{download, 0, options.width(), options.height()};
        SDL_DownloadFromGPUTexture(copy, &from, &to);
        SDL_EndGPUCopyPass(copy);
    }
    void finish_readback(Command &command, std::span<uint32_t> output) {
        auto *fence = checked(SDL_SubmitGPUCommandBufferAndAcquireFence(command.take()), "Submit GPU readback");
        bool success = SDL_WaitForGPUFences(device, true, &fence, 1);
        SDL_ReleaseGPUFence(device, fence);
        if (!success) fail("Wait for GPU readback");
        auto *mapped = checked(SDL_MapGPUTransferBuffer(device, download, false), "Map GPU readback");
        std::memcpy(output.data(), mapped, size_t(options.width()) * options.height() * 4);
        SDL_UnmapGPUTransferBuffer(device, download);
    }
    void draw(const GpuScene &scene, std::span<uint32_t> output, unsigned layer_mask) {
        size_t count = size_t(options.width()) * options.height();
        if (!output.empty() && output.size() < count) throw std::runtime_error("Incomplete GPU output buffer");
        if (scene.sprite_count > 1024) throw std::runtime_error("GPU sprite count out of range");
        auto *mapped = static_cast<uint8_t *>(checked(SDL_MapGPUTransferBuffer(device, upload, true), "Map GPU frame upload"));
        if (scene.fallback) {
            std::memcpy(mapped, scene.native_pixels.data(), native_bytes);
            interpolation_stats = {};
            if (interpolation != VideoInterpolation::Off)
                interpolation_stats.reason = options.scale == 1 ? InterpolationReason::NativeScale : InterpolationReason::Oracle;
        } else {
            std::memcpy(mapped, scene.words.data(), scene_bytes);
            if (interpolation != VideoInterpolation::Off)
                interpolation_stats = analyze_gpu_interpolation(scene, options, interpolation,
                    {reinterpret_cast<uint32_t *>(mapped), GpuScene::word_count}, tile_pen_masks);
        }
        SDL_UnmapGPUTransferBuffer(device, upload);
        Command command(device);
        auto *copy = checked(SDL_BeginGPUCopyPass(command.value), "Begin GPU frame copy");
        upload_buffer(copy, upload, scene.fallback ? native_buffer : scene_buffer, 0, scene.fallback ? native_bytes : scene_bytes, true);
        SDL_EndGPUCopyPass(copy);
        GpuUniforms uniforms{options.scale, options.border, options.width(), options.height(), scene.sprite_count,
                             scene.pen_mask, unsigned(scene.fallback), layer_mask};
        if (!scene.fallback) {
            SDL_PushGPUVertexUniformData(command.value, 0, &uniforms, sizeof(uniforms));
            auto *pass = render_pass(command.value, sprite_plane);
            SDL_BindGPUGraphicsPipeline(pass, sprite_pipeline);
            if (!options.expanded()) {
                SDL_Rect scissor{46, 24, 320, 232};
                SDL_SetGPUScissor(pass, &scissor);
            }
            SDL_BindGPUVertexStorageBuffers(pass, 0, &scene_buffer, 1);
            SDL_BindGPUFragmentStorageBuffers(pass, 0, &sp_assets, 1);
            SDL_DrawGPUPrimitives(pass, 256 * 6, scene.sprite_count, 0, 0);
            SDL_EndGPURenderPass(pass);
        }
        // Vertex sprite uniforms already copied; only the compositor sees the mode.
        uniforms.sprite_count |= unsigned(interpolation) << 16;
        SDL_PushGPUFragmentUniformData(command.value, 0, &uniforms, sizeof(uniforms));
        auto *pass = render_pass(command.value, surface);
        SDL_BindGPUGraphicsPipeline(pass, scene_pipeline);
        SDL_GPUTextureSamplerBinding sprite_binding{sprite_plane, sampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &sprite_binding, 1);
        SDL_GPUBuffer *buffers[]{scene_buffer, pf_assets, native_buffer};
        SDL_BindGPUFragmentStorageBuffers(pass, 0, buffers, 3);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
        if (!output.empty()) queue_download(command.value);
        if (window) {
            SDL_GPUTexture *swapchain = nullptr; Uint32 w = 0, h = 0;
            if (!SDL_WaitAndAcquireGPUSwapchainTexture(command.value, window, &swapchain, &w, &h)) fail("Acquire GPU swapchain");
            if (swapchain) {
                command.acquired_swapchain = true;
                Uint32 dest_w = w, dest_h = h;
                if (uint64_t(w) * options.height() > uint64_t(h) * options.width())
                    dest_w = Uint32(uint64_t(h) * options.width() / options.height());
                else dest_h = Uint32(uint64_t(w) * options.height() / options.width());
                SDL_GPUBlitInfo blit{};
                blit.source = {surface, 0, 0, 0, 0, options.width(), options.height()};
                blit.destination = {swapchain, 0, 0, (w - dest_w) / 2, (h - dest_h) / 2, dest_w, dest_h};
                blit.load_op = SDL_GPU_LOADOP_CLEAR; blit.clear_color = {0, 0, 0, 1};
                blit.filter = linear ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
                SDL_BlitGPUTexture(command.value, &blit);
            }
        }
        if (!output.empty()) finish_readback(command, output);
        else if (!SDL_SubmitGPUCommandBuffer(command.take())) fail("Submit GPU frame");
        rendered = true;
    }
};

GpuVideo::GpuVideo(SDL_Window *window, GameVideoOptions options, std::span<const uint8_t> playfield_assets,
                   std::span<const uint8_t> sprite_assets, bool linear, bool vsync,
                   VideoInterpolation interpolation) : impl_(std::make_unique<Impl>()) {
    impl_->window = window; impl_->options = options; impl_->linear = linear;
    impl_->vsync = vsync;
    impl_->interpolation = interpolation;
    impl_->init(playfield_assets, sprite_assets);
}
GpuVideo::~GpuVideo() = default;
const char *GpuVideo::driver() const { return SDL_GetGPUDeviceDriver(impl_->device); }
const InterpolationStats &GpuVideo::last_interpolation() const { return impl_->interpolation_stats; }
void GpuVideo::draw(const GpuScene &scene, std::span<uint32_t> output, unsigned layer_mask) {
    impl_->draw(scene, output, layer_mask);
}
void GpuVideo::save_surface(const char *path) {
    if (!impl_->rendered) throw std::runtime_error("No GPU surface has been drawn");
    Command command(impl_->device);
    impl_->queue_download(command.value);
    impl_->finish_readback(command, impl_->saved_pixels);
    auto *surface = checked(SDL_CreateSurfaceFrom(int(impl_->options.width()), int(impl_->options.height()),
        SDL_PIXELFORMAT_ARGB8888, impl_->saved_pixels.data(), int(impl_->options.width() * 4)), "Create GPU capture surface");
    const char *extension = std::strrchr(path, '.');
    bool saved = extension && SDL_strcasecmp(extension, ".png") == 0 ? SDL_SavePNG(surface, path) : SDL_SaveBMP(surface, path);
    SDL_DestroySurface(surface);
    if (!saved) fail("Save GPU surface");
}
} // namespace f3rt
