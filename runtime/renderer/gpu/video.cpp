#include "renderer/gpu/video.hpp"
#include "renderer/gpu/encode.hpp"
#include "video_shaders.hpp"
#ifdef __APPLE__
#include "renderer/gpu/pacing_macos.hpp"
#endif
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iostream>
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
struct WindowCapture {
    SDL_GPUDevice *device;
    SDL_GPUTexture *texture = nullptr;
    SDL_GPUTransferBuffer *download = nullptr;
    SDL_GPUFence *fence = nullptr;
    bool mapped = false;
    SDL_PixelFormat pixel_format = SDL_PIXELFORMAT_UNKNOWN;
    explicit WindowCapture(SDL_GPUDevice *value) : device(value) {}
    ~WindowCapture() {
        if (mapped) SDL_UnmapGPUTransferBuffer(device, download);
        if (fence) SDL_ReleaseGPUFence(device, fence);
        if (download) SDL_ReleaseGPUTransferBuffer(device, download);
        if (texture) SDL_ReleaseGPUTexture(device, texture);
    }
};
constexpr Uint32 scene_bytes = encoded_scene_words * sizeof(uint32_t);
constexpr Uint32 interpolation_scene_bytes = encoded_interpolation_words * sizeof(uint32_t);
constexpr Uint32 native_bytes = encoded_native_words * sizeof(uint32_t);
constexpr size_t max_user_shader_bytes = 1024 * 1024;

// Validate the fixed descriptor/interface ABI before handing user SPIR-V to the
// driver. Compilation/pipeline creation performs the remaining shader validation.
void validate_user_spirv(const std::vector<uint32_t> &words) {
    auto invalid = [] { throw std::runtime_error("User SPIR-V: invalid module or postprocess ABI"); };
    if (words.size() < 5 || words[0] != 0x07230203 || words[4] != 0 ||
        words[3] == 0 || words[3] > max_user_shader_bytes / 4) invalid();
    struct Id {
        uint32_t opcode = 0, a = 0, b = 0, c = 0;
        uint32_t set = UINT32_MAX, binding = UINT32_MAX;
        bool builtin = false, block = false, zero_offset = false;
    };
    std::vector<Id> ids(words[3]);
    auto id = [&](uint32_t n) -> Id & {
        if (!n || n >= ids.size()) invalid();
        return ids[n];
    };
    bool entry = false;
    for (size_t pos = 5; pos < words.size();) {
        const auto *p = words.data() + pos;
        uint32_t count = p[0] >> 16, op = p[0] & 0xffff;
        if (!count || count > words.size() - pos) invalid();
        auto need = [&](uint32_t n) { if (count < n) invalid(); };
        if (op == 15) { // OpEntryPoint: Fragment main, no alternate entries.
            need(5);
            if (entry || p[1] != 4 || p[3] != 0x6e69616d || p[4] != 0) invalid();
            id(p[2]); entry = true;
        } else if (op == 71) { // OpDecorate
            need(3); auto &v = id(p[1]);
            if (p[2] == 33 || p[2] == 34) {
                need(4); (p[2] == 33 ? v.binding : v.set) = p[3];
            } else if (p[2] == 11) v.builtin = true;
            else if (p[2] == 2) v.block = true;
        } else if (op == 72) { // OpMemberDecorate
            need(4);
            if (p[3] == 35) {
                need(5);
                if (p[2] == 0 && p[4] == 0) id(p[1]).zero_offset = true;
            }
        } else if (op == 22 || op == 23 || op == 25 || op == 27 || op == 30 || op == 32 || op == 59) {
            need(op == 22 || op == 27 || op == 30 ? 3 : 4);
            auto &v = id(op == 59 ? p[2] : p[1]);
            v.opcode = op;
            v.a = op == 59 ? p[1] : p[2];
            v.b = (count > 3) ? p[3] : 0;
            v.c = count;
            if (op == 25) { // Only a non-arrayed sampled 2D float image.
                need(9);
                if (p[3] != 1 || p[4] != 0 || p[5] != 0 || p[6] != 0 || p[7] != 1) invalid();
            }
        }
        pos += count;
    }
    if (!entry) invalid();
    unsigned samplers = 0, uniforms = 0;
    for (const auto &v : ids) {
        if (v.opcode != 59) continue;
        const auto &pointer = id(v.a);
        if (pointer.opcode != 32 || pointer.a != v.b) invalid();
        const auto &type = id(pointer.b);
        if (v.b == 0) { // UniformConstant: combined sampler, set2/binding0.
            if (v.set != 2 || v.binding != 0 || type.opcode != 27) invalid();
            const auto &image = id(type.a);
            if (image.opcode != 25 || id(image.a).opcode != 22 || id(image.a).a != 32) invalid();
            ++samplers;
        } else if (v.b == 2) { // Uniform: one struct member, a float4 at offset zero.
            if (v.set != 3 || v.binding != 0 || type.opcode != 30 || type.c != 3 ||
                !type.block || !type.zero_offset) invalid();
            const auto &vector = id(type.a);
            if (vector.opcode != 23 || vector.b != 4 ||
                id(vector.a).opcode != 22 || id(vector.a).a != 32) invalid();
            ++uniforms;
        } else if (v.b == 1) {
            if (!v.builtin) invalid(); // Fullscreen vertex shader exports no varyings.
        } else if (v.b != 3 && v.b != 6 && v.b != 7) invalid();
    }
    if (samplers != 1 || uniforms != 1) invalid();
}
}

struct GpuVideo::Impl {
    SDL_GPUDevice *device = nullptr;
    SDL_Window *window = nullptr;
    bool claimed = false, linear = false, rendered = false;
    bool vsync = true;
    bool last_presented = false;
    bool motion_pace_prepared = false;
#ifdef __APPLE__
    void *motion_pacing = nullptr;
#endif
    GpuVideo::Overlay overlay = nullptr;
    void *overlay_userdata = nullptr;
    GameVideoOptions options{};
    VideoScaleMode scale_mode = VideoScaleMode::Fixed;
    VideoInterpolation interpolation = VideoInterpolation::Off;
    InterpolationFields fields = InterpolationFields::Geometry;
    InterpolationStats interpolation_stats{};
    std::unique_ptr<GpuMotionHistory> motion;
    MotionInterpolationStats motion_stats{};
    std::array<LayerInterpolationStats, 4> logged_layers{};
    InterpolationReason logged_reason = InterpolationReason::Off;
    bool have_interpolation_log = false;
    std::vector<uint64_t> tile_pen_masks;
    // Asset ROM sizes in bytes (256 per 16x16 tile): fixed by the loaded ROM set, not the hardware maximum.
    Uint32 pf_asset_bytes = 0, sp_asset_bytes = 0;
    SDL_GPUBuffer *scene_buffer = nullptr, *pf_assets = nullptr, *sp_assets = nullptr, *native_buffer = nullptr;
    SDL_GPUTexture *sprite_plane = nullptr, *surface = nullptr;
    SDL_GPUSampler *sampler = nullptr;
    SDL_GPUGraphicsPipeline *sprite_pipeline = nullptr, *scene_pipeline = nullptr, *interpolation_pipeline = nullptr;
    SDL_GPUTransferBuffer *upload = nullptr, *download = nullptr;
    std::vector<uint32_t> saved_pixels;
    Postprocess postprocess = Postprocess::Off;
    SDL_GPUGraphicsPipeline *post_pipeline = nullptr;
    SDL_GPUTexture *post_surface = nullptr;
    SDL_GPUTexture *comparison_native = nullptr;
    bool comparison_native_valid = false;
    bool rendered_post = false;
    Uint64 post_start = 0;

    ~Impl() {
#ifdef __APPLE__
        destroy_macos_motion_pacing(motion_pacing);
#endif
        if (!device) return;
        SDL_WaitForGPUIdle(device);
        if (sprite_pipeline) SDL_ReleaseGPUGraphicsPipeline(device, sprite_pipeline);
        if (scene_pipeline) SDL_ReleaseGPUGraphicsPipeline(device, scene_pipeline);
        if (interpolation_pipeline) SDL_ReleaseGPUGraphicsPipeline(device, interpolation_pipeline);
        if (post_pipeline) SDL_ReleaseGPUGraphicsPipeline(device, post_pipeline);
        if (sampler) SDL_ReleaseGPUSampler(device, sampler);
        if (sprite_plane) SDL_ReleaseGPUTexture(device, sprite_plane);
        if (surface) SDL_ReleaseGPUTexture(device, surface);
        if (post_surface) SDL_ReleaseGPUTexture(device, post_surface);
        if (comparison_native) SDL_ReleaseGPUTexture(device, comparison_native);
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
    SDL_GPUTexture *texture(SDL_GPUTextureFormat format, GameVideoOptions geometry, bool sprite = false) {
        SDL_GPUTextureCreateInfo info{};
        info.type = SDL_GPU_TEXTURETYPE_2D; info.format = format;
        info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
        bool native_plane = sprite && !geometry.expanded();
        info.width = native_plane ? 432 : geometry.width(); info.height = native_plane ? 256 : geometry.height();
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
    void set_postprocess(Postprocess preset, const std::filesystem::path &path) {
        if (preset != Postprocess::Off && preset != Postprocess::Crt && preset != Postprocess::User)
            throw std::runtime_error("Unknown GPU postprocess preset");
        if (preset == Postprocess::Off) {
            if (post_pipeline) SDL_ReleaseGPUGraphicsPipeline(device, post_pipeline);
            if (post_surface) SDL_ReleaseGPUTexture(device, post_surface);
            post_pipeline = nullptr; post_surface = nullptr;
            postprocess = preset; rendered = false; rendered_post = false;
            return;
        }
        SDL_GPUGraphicsPipeline *next_pipeline = nullptr;
        SDL_GPUTexture *next_surface = nullptr;
        try {
            if (preset == Postprocess::Crt) {
                next_pipeline = pipeline(video_shaders::fullscreen_vert, video_shaders::postprocess_frag,
                                         SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);
            } else {
                const bool metal = (SDL_GetGPUShaderFormats(device) & SDL_GPU_SHADERFORMAT_MSL) != 0;
                if (path.extension() != (metal ? ".metal" : ".spv"))
                    throw std::runtime_error(metal ? "Metal user shader requires a .metal file" :
                                                   "Vulkan user shader requires a .spv file");
                std::ifstream file(path, std::ios::binary | std::ios::ate);
                if (!file) throw std::runtime_error("Cannot open user shader: " + path.string());
                const auto end = file.tellg();
                if (end <= 0 || end > std::streamoff(max_user_shader_bytes))
                    throw std::runtime_error("User shader must be 1 byte to 1 MiB");
                const size_t size = size_t(end);
                // Aligned storage for SPIR-V; one trailing zero for native MSL.
                std::vector<uint32_t> code((size + 4) / 4, 0);
                file.seekg(0);
                if (!file.read(reinterpret_cast<char *>(code.data()), std::streamsize(size)))
                    throw std::runtime_error("Cannot read user shader: " + path.string());
                if (metal) {
                    if (std::memchr(code.data(), 0, size))
                        throw std::runtime_error("User Metal shader contains an embedded NUL");
                } else {
                    if (size % 4) throw std::runtime_error("User SPIR-V size must be a multiple of four");
                    code.resize(size / 4);
                    validate_user_spirv(code);
                }
                video_shaders::Shader source{reinterpret_cast<const uint8_t *>(code.data()), size,
                    reinterpret_cast<const char *>(code.data()), size, "f3_postprocess", 1, 0, 1};
                next_pipeline = pipeline(video_shaders::fullscreen_vert, source, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);
            }
            if (!post_surface) next_surface = texture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, options);
        } catch (...) {
            if (next_pipeline) SDL_ReleaseGPUGraphicsPipeline(device, next_pipeline);
            if (next_surface) SDL_ReleaseGPUTexture(device, next_surface);
            throw;
        }
        // SDL releases are deferred until pending commands finish using resources.
        if (post_pipeline) SDL_ReleaseGPUGraphicsPipeline(device, post_pipeline);
        post_pipeline = next_pipeline;
        if (next_surface) post_surface = next_surface;
        postprocess = preset;
        post_start = SDL_GetTicksNS();
        rendered = false; rendered_post = false;
    }
    void upload_buffer(SDL_GPUCopyPass *pass, SDL_GPUTransferBuffer *source, SDL_GPUBuffer *destination,
                       Uint32 offset, Uint32 size, bool cycle) {
        SDL_GPUTransferBufferLocation from{source, offset};
        SDL_GPUBufferRegion to{destination, 0, size};
        SDL_UploadToGPUBuffer(pass, &from, &to, cycle);
    }
    void init(std::span<const uint8_t> tiles, std::span<const uint8_t> sprites) {
        if (!options.scale || options.scale > GameVideoOptions::max_gpu_scale || options.border > GameVideoOptions::max_border)
            throw std::runtime_error("Game GPU presentation scale/border out of range");
        if (tiles.empty() || sprites.empty() || tiles.size() % 256 || sprites.size() % 256 ||
            tiles.size() > UINT32_MAX / 2 || sprites.size() > UINT32_MAX / 2 || tiles.size() + sprites.size() > UINT32_MAX)
            throw std::runtime_error("Incomplete GPU tile/sprite assets");
        pf_asset_bytes = Uint32(tiles.size()); sp_asset_bytes = Uint32(sprites.size());
        device = checked(SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL | SDL_GPU_SHADERFORMAT_METALLIB, false, nullptr), "Create SDL GPU device");
        if (window) {
            if (!SDL_ClaimWindowForGPUDevice(device, window)) fail("Claim GPU window");
            claimed = true;
            // FIFO can make a slow display throttle native simulation/audio.
            // Mailbox remains tear-free without queuing obsolete display frames.
            const auto mode = !vsync && SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_IMMEDIATE)
                ? SDL_GPU_PRESENTMODE_IMMEDIATE
                : SDL_WindowSupportsGPUPresentMode(device, window, SDL_GPU_PRESENTMODE_MAILBOX)
                    ? SDL_GPU_PRESENTMODE_MAILBOX : SDL_GPU_PRESENTMODE_VSYNC;
            if (mode != SDL_GPU_PRESENTMODE_VSYNC &&
                !SDL_SetGPUSwapchainParameters(device, window, SDL_GPU_SWAPCHAINCOMPOSITION_SDR, mode))
                fail("Set GPU presentation mode");
        }
        scene_buffer = buffer(interpolation == VideoInterpolation::Off ? scene_bytes : interpolation_scene_bytes);
        native_buffer = buffer(native_bytes);
        pf_assets = buffer(pf_asset_bytes); sp_assets = buffer(sp_asset_bytes);
        sprite_plane = texture(SDL_GPU_TEXTUREFORMAT_R16_UINT, options, true);
        surface = texture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, options);
        SDL_GPUSamplerCreateInfo sampling{};
        sampling.min_filter = sampling.mag_filter = SDL_GPU_FILTER_NEAREST;
        sampling.mipmap_mode = SDL_GPU_SAMPLERMIPMAPMODE_NEAREST;
        sampling.address_mode_u = sampling.address_mode_v = sampling.address_mode_w = SDL_GPU_SAMPLERADDRESSMODE_CLAMP_TO_EDGE;
        sampler = checked(SDL_CreateGPUSampler(device, &sampling), "Create GPU sampler");
        sprite_pipeline = pipeline(video_shaders::sprite_vert, video_shaders::sprite_frag, SDL_GPU_TEXTUREFORMAT_R16_UINT);
        scene_pipeline = pipeline(video_shaders::fullscreen_vert, video_shaders::scene_frag, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);
        if (interpolation != VideoInterpolation::Off)
            interpolation_pipeline = pipeline(video_shaders::fullscreen_vert, video_shaders::scene_interp_frag, SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);
        upload = transfer(pf_asset_bytes + sp_asset_bytes, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
        if (interpolation != VideoInterpolation::Off && (unsigned(fields) & unsigned(InterpolationFields::Palette)))
            tile_pen_masks.resize(pf_asset_bytes / 256);
        auto *mapped = static_cast<uint32_t *>(checked(SDL_MapGPUTransferBuffer(device, upload, false), "Map GPU asset upload"));
        // Explicit low-byte-first packing also works on big-endian hosts.
        for (Uint32 i = 0; i < pf_asset_bytes; i += 4) {
            mapped[i / 4] = uint32_t(tiles[i]) | (uint32_t(tiles[i + 1]) << 8) | (uint32_t(tiles[i + 2]) << 16) | (uint32_t(tiles[i + 3]) << 24);
            if (!tile_pen_masks.empty())
                tile_pen_masks[i / 256] |= (uint64_t{1} << (tiles[i] & 63)) |
                    (uint64_t{1} << (tiles[i + 1] & 63)) | (uint64_t{1} << (tiles[i + 2] & 63)) |
                    (uint64_t{1} << (tiles[i + 3] & 63));
        }
        for (Uint32 i = 0; i < sp_asset_bytes; i += 4)
            mapped[(pf_asset_bytes + i) / 4] = uint32_t(sprites[i]) | (uint32_t(sprites[i + 1]) << 8) | (uint32_t(sprites[i + 2]) << 16) | (uint32_t(sprites[i + 3]) << 24);
        SDL_UnmapGPUTransferBuffer(device, upload);
        Command command(device);
        auto *copy = checked(SDL_BeginGPUCopyPass(command.value), "Begin GPU asset copy");
        upload_buffer(copy, upload, pf_assets, 0, pf_asset_bytes, false);
        upload_buffer(copy, upload, sp_assets, pf_asset_bytes, sp_asset_bytes, false);
        SDL_EndGPUCopyPass(copy);
        if (!SDL_SubmitGPUCommandBuffer(command.take())) fail("Submit GPU asset upload");
        SDL_ReleaseGPUTransferBuffer(device, upload);
        upload = nullptr;
        upload = transfer(std::max(interpolation == VideoInterpolation::Off ? scene_bytes : interpolation_scene_bytes,
                                   native_bytes), SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
    }
    void set_scale(unsigned scale) {
        if (!scale || scale > GameVideoOptions::max_gpu_scale)
            throw std::runtime_error("GPU scale must be 1..8");
        if (scale == options.scale) return;
        auto geometry = options;
        geometry.scale = scale;
        SDL_GPUTexture *next_plane = nullptr, *next_surface = nullptr, *next_post = nullptr;
        SDL_GPUTransferBuffer *next_download = nullptr;
        std::vector<uint32_t> next_pixels;
        try {
            next_plane = texture(SDL_GPU_TEXTUREFORMAT_R16_UINT, geometry, true);
            next_surface = texture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, geometry);
            if (post_surface) next_post = texture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, geometry);
            if (download) next_download = transfer(geometry.width() * geometry.height() * 4, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD);
            if (!saved_pixels.empty()) next_pixels.resize(size_t(geometry.width()) * geometry.height());
        } catch (...) {
            if (next_plane) SDL_ReleaseGPUTexture(device, next_plane);
            if (next_surface) SDL_ReleaseGPUTexture(device, next_surface);
            if (next_post) SDL_ReleaseGPUTexture(device, next_post);
            if (next_download) SDL_ReleaseGPUTransferBuffer(device, next_download);
            throw;
        }
        // SDL defers destruction until queued users finish: no GPU-idle stall.
        SDL_ReleaseGPUTexture(device, sprite_plane);
        SDL_ReleaseGPUTexture(device, surface);
        if (post_surface) SDL_ReleaseGPUTexture(device, post_surface);
        if (comparison_native) SDL_ReleaseGPUTexture(device, comparison_native);
        comparison_native = nullptr;
        comparison_native_valid = false;
        if (download) SDL_ReleaseGPUTransferBuffer(device, download);
        sprite_plane = next_plane; surface = next_surface; download = next_download;
        post_surface = next_post;
        saved_pixels.swap(next_pixels);
        options = geometry;
        rendered = false;
        interpolation_stats = {};
    }
    SDL_GPURenderPass *render_pass(SDL_GPUCommandBuffer *command, SDL_GPUTexture *target) {
        SDL_GPUColorTargetInfo info{};
        info.texture = target; info.load_op = SDL_GPU_LOADOP_CLEAR; info.store_op = SDL_GPU_STOREOP_STORE;
        info.clear_color = {0, 0, 0, 1}; info.cycle = true;
        return checked(SDL_BeginGPURenderPass(command, &info, 1, nullptr), "Begin GPU render pass");
    }
    void queue_download(SDL_GPUCommandBuffer *command, SDL_GPUTexture *target) {
        if (!download) download = transfer(options.width() * options.height() * 4, SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD);
        auto *copy = checked(SDL_BeginGPUCopyPass(command), "Begin GPU readback copy");
        SDL_GPUTextureRegion from{target, 0, 0, 0, 0, 0, options.width(), options.height(), 1};
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
    void report_interpolation() {
        if (interpolation == VideoInterpolation::Off) return;
        const auto &stats = interpolation_stats;
        if (have_interpolation_log && logged_reason == stats.reason && logged_layers == stats.layers) return;
        std::cout << "video_interp fields=" << interpolation_fields_name(fields)
                  << " reason=" << interpolation_reason_name(stats.reason);
        for (unsigned i = 0; i < stats.layers.size(); ++i) {
            const auto &layer = stats.layers[i];
            std::cout << " pf" << i << '=' << interpolation_reason_name(layer.reason)
                      << ':' << layer.source_rows << '/' << layer.zoom_rows << '/' << layer.vertical_rows << '/' << layer.palette_rows
                      << " invalid" << i << '=' << layer.invalid_rows
                      << " boundaries" << i << '=' << layer.discontinuities
                      << " unsafe_palette" << i << '=' << layer.unsafe_palette_pairs;
        }
        std::cout << '\n';
        logged_reason = stats.reason; logged_layers = stats.layers; have_interpolation_log = true;
    }
    enum class Presentation { Wait, SkipBusy, Offscreen };
    void draw(const CapturedFrame &scene, std::span<uint32_t> output, unsigned layer_mask, bool process_diagnostic,
              bool temporal = false, float alpha = 1.0f,
              const std::chrono::steady_clock::time_point *frame_start = nullptr,
              std::chrono::nanoseconds period = {}, Presentation presentation = Presentation::Wait,
              bool comparison = false, const char *capture_path = nullptr) {
        const bool no_present = presentation == Presentation::Offscreen;
        const bool skip_busy = presentation == Presentation::SkipBusy;
        size_t count = size_t(options.width()) * options.height();
        if (!output.empty() && output.size() < count) throw std::runtime_error("Incomplete GPU output buffer");
        if (scene.sprite_count > F3_SCENE_SPRITE_COUNT) throw std::runtime_error("GPU sprite count out of range");
        [[maybe_unused]] const bool pace_prepared = !no_present && motion_pace_prepared;
        if (!no_present) motion_pace_prepared = false;
        if (comparison && !comparison_native_valid) {
            // Cache only the current canonical scene, never temporally modified
            // words. This offscreen pass does not acquire or pace a swapchain.
            draw(scene, {}, layer_mask, process_diagnostic, false, 1.0f, nullptr, {}, Presentation::Offscreen);
            if (!comparison_native)
                comparison_native = texture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, options);
            Command cache(device);
            SDL_GPUBlitInfo blit{};
            blit.source = {rendered_post ? post_surface : surface, 0, 0, 0, 0, options.width(), options.height()};
            blit.destination = {comparison_native, 0, 0, 0, 0, options.width(), options.height()};
            blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
            blit.filter = SDL_GPU_FILTER_NEAREST;
            SDL_BlitGPUTexture(cache.value, &blit);
            if (!SDL_SubmitGPUCommandBuffer(cache.take())) fail("Submit native comparison cache");
            comparison_native_valid = true;
        }
        last_presented = false;
        Command command(device);
        SDL_GPUTexture *swapchain = nullptr;
        Uint32 w = 0, h = 0;
        if (skip_busy) {
            if (!window) return;
            if (!SDL_AcquireGPUSwapchainTexture(command.value, window, &swapchain, &w, &h))
                fail("Try acquiring GPU swapchain");
            if (!swapchain) return; // Command cancels; no uploads/resource cycling.
            command.acquired_swapchain = true;
        }
        if (frame_start) {
#ifdef __APPLE__
            if (vsync && !pace_prepared && !skip_busy) wait_macos_motion_pacing(motion_pacing);
#endif
            if (window && !no_present && !skip_busy) {
                if (!SDL_WaitAndAcquireGPUSwapchainTexture(command.value, window, &swapchain, &w, &h))
                    fail("Acquire timed GPU swapchain");
                command.acquired_swapchain = swapchain != nullptr;
            }
            alpha = period.count() > 0 ? std::clamp(float(
                std::chrono::duration<double>(std::chrono::steady_clock::now() - *frame_start).count() /
                std::chrono::duration<double>(period).count()), 0.0f, 1.0f) : 1.0f;
        }
        const bool active_interpolation = interpolation != VideoInterpolation::Off && options.scale > 1;
        const Uint32 frame_scene_bytes = active_interpolation ? interpolation_scene_bytes : scene_bytes;
        auto *mapped = static_cast<uint8_t *>(checked(SDL_MapGPUTransferBuffer(device, upload, true), "Map GPU frame upload"));
        std::span<uint32_t> words{reinterpret_cast<uint32_t *>(mapped),
            size_t(scene.fallback ? native_bytes : frame_scene_bytes) / sizeof(uint32_t)};
        if (scene.fallback) {
            if (motion) motion->reset();
            encode(scene, options, nullptr, nullptr, words);
            interpolation_stats = {};
            if (interpolation != VideoInterpolation::Off)
                interpolation_stats.reason = options.scale == 1 ? InterpolationReason::NativeScale : InterpolationReason::Oracle;
            for (auto &layer : interpolation_stats.layers) layer.reason = interpolation_stats.reason;
        } else {
            // Per present: temporal geometry -> interpolation analysis -> one-way encode.
            MotionResult temporal_state;
            if (temporal && motion) {
                temporal_state = motion->apply(scene, alpha);
                motion_stats = temporal_state.stats;
            }
            const MotionState *motion_state = temporal && motion && motion_stats.paired ? &temporal_state.state : nullptr;
            InterpolationAnalysis analysis;
            if (active_interpolation) {
                analysis = analyze_gpu_interpolation(scene, options, interpolation, fields, motion_state, tile_pen_masks);
                interpolation_stats = analysis.stats;
            } else {
                interpolation_stats = {};
                if (interpolation != VideoInterpolation::Off) interpolation_stats.reason = InterpolationReason::NativeScale;
                for (auto &layer : interpolation_stats.layers) layer.reason = interpolation_stats.reason;
            }
            encode(scene, options, motion_state, active_interpolation ? &analysis.coefficients : nullptr, words);
        }
        SDL_UnmapGPUTransferBuffer(device, upload);
        report_interpolation();
        auto *copy = checked(SDL_BeginGPUCopyPass(command.value), "Begin GPU frame copy");
        upload_buffer(copy, upload, scene.fallback ? native_buffer : scene_buffer, 0,
            scene.fallback ? native_bytes : frame_scene_bytes, true);
        SDL_EndGPUCopyPass(copy);
        GpuUniforms uniforms{options.scale, options.border, options.width(), options.height(), scene.sprite_count,
                             scene.pen_mask, unsigned(scene.fallback), layer_mask & all_layers};
        uniforms.pf_tile_count = pf_asset_bytes / 256; uniforms.sp_tile_count = sp_asset_bytes / 256;
        if (temporal && motion_stats.paired && motion_stats.alpha < 1.0f)
            uniforms.layer_mask |= motion_layer_mask;
        if (!scene.fallback) {
            SDL_PushGPUVertexUniformData(command.value, 0, &uniforms, sizeof(uniforms));
            auto *pass = render_pass(command.value, sprite_plane);
            SDL_BindGPUGraphicsPipeline(pass, sprite_pipeline);
            if (!options.expanded()) {
                SDL_Rect scissor{46, int(geometry::first_line), int(geometry::native_width), int(geometry::height)};
                SDL_SetGPUScissor(pass, &scissor);
            }
            SDL_BindGPUVertexStorageBuffers(pass, 0, &scene_buffer, 1);
            SDL_BindGPUFragmentStorageBuffers(pass, 0, &sp_assets, 1);
            SDL_DrawGPUPrimitives(pass, 6, scene.sprite_count, 0, 0);
            SDL_EndGPURenderPass(pass);
        }
        SDL_PushGPUFragmentUniformData(command.value, 0, &uniforms, sizeof(uniforms));
        auto *pass = render_pass(command.value, surface);
        SDL_BindGPUGraphicsPipeline(pass, options.scale > 1 && interpolation_pipeline ? interpolation_pipeline : scene_pipeline);
        SDL_GPUTextureSamplerBinding sprite_binding{sprite_plane, sampler};
        SDL_BindGPUFragmentSamplers(pass, 0, &sprite_binding, 1);
        SDL_GPUBuffer *buffers[]{scene_buffer, pf_assets, native_buffer};
        SDL_BindGPUFragmentStorageBuffers(pass, 0, buffers, 3);
        SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
        SDL_EndGPURenderPass(pass);
        SDL_GPUTexture *present_surface = surface;
        const bool apply_post = postprocess != Postprocess::Off &&
            (process_diagnostic || (output.empty() && layer_mask == all_layers));
        if (apply_post) {
            const float parameters[]{float(options.width()), float(options.height()), float(options.scale),
                float(double(SDL_GetTicksNS() - post_start) / 1.0e9)};
            SDL_PushGPUFragmentUniformData(command.value, 0, parameters, sizeof(parameters));
            auto *post_pass = render_pass(command.value, post_surface);
            SDL_BindGPUGraphicsPipeline(post_pass, post_pipeline);
            SDL_GPUTextureSamplerBinding binding{surface, sampler};
            SDL_BindGPUFragmentSamplers(post_pass, 0, &binding, 1);
            SDL_DrawGPUPrimitives(post_pass, 3, 1, 0, 0);
            SDL_EndGPURenderPass(post_pass);
            present_surface = post_surface;
        }
        if (!output.empty()) queue_download(command.value, present_surface);
        WindowCapture capture(device);
        if (window && !no_present) {
            if (!frame_start && !skip_busy &&
                !SDL_WaitAndAcquireGPUSwapchainTexture(command.value, window, &swapchain, &w, &h))
                fail("Acquire GPU swapchain");
            if (swapchain) {
                command.acquired_swapchain = true;
                SDL_GPUTexture *composition = swapchain;
                if (capture_path) {
                    const auto format = SDL_GetGPUSwapchainTextureFormat(device, window);
                    if (format == SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM)
                        capture.pixel_format = SDL_PIXELFORMAT_ARGB8888;
                    else if (format == SDL_GPU_TEXTUREFORMAT_R8G8B8A8_UNORM)
                        capture.pixel_format = SDL_PIXELFORMAT_ABGR8888;
                    else throw std::runtime_error("Window capture requires an 8-bit SDR swapchain");
                    SDL_GPUTextureCreateInfo info{};
                    info.type = SDL_GPU_TEXTURETYPE_2D;
                    info.format = format;
                    info.usage = SDL_GPU_TEXTUREUSAGE_COLOR_TARGET | SDL_GPU_TEXTUREUSAGE_SAMPLER;
                    info.width = w; info.height = h; info.layer_count_or_depth = 1; info.num_levels = 1;
                    capture.texture = checked(SDL_CreateGPUTexture(device, &info), "Create window capture composition");
                    composition = capture.texture;
                    const uint64_t bytes = uint64_t(w) * h * 4;
                    if (bytes > UINT32_MAX) throw std::runtime_error("Window capture exceeds GPU transfer size");
                    capture.download = transfer(Uint32(bytes), SDL_GPU_TRANSFERBUFFERUSAGE_DOWNLOAD);
                }
                const unsigned panes = comparison && w >= 2 ? 2 : 1;
                for (unsigned pane = 0; pane < panes; ++pane) {
                    const Uint32 offset = panes == 2 && pane == 1 ? w / 2 : 0;
                    const Uint32 pane_width = panes == 1 ? w : (pane == 0 ? w / 2 : w - w / 2);
                    const auto viewport = video_blit_for_window(scale_mode, options, pane_width, h);
                    SDL_GPUBlitInfo blit{};
                    blit.source = {panes == 2 && pane == 0 ? comparison_native : present_surface, 0, 0,
                        viewport.source_x, viewport.source_y, viewport.source_width, viewport.source_height};
                    blit.destination = {composition, 0, 0, offset + viewport.x, viewport.y, viewport.width, viewport.height};
                    blit.load_op = pane == 0 ? SDL_GPU_LOADOP_CLEAR : SDL_GPU_LOADOP_LOAD;
                    blit.clear_color = {0, 0, 0, 1};
                    blit.filter = linear && scale_mode != VideoScaleMode::AutoInteger ? SDL_GPU_FILTER_LINEAR : SDL_GPU_FILTER_NEAREST;
                    SDL_BlitGPUTexture(command.value, &blit);
                }
                if (overlay) overlay(overlay_userdata, command.value, composition, w, h);
                if (capture.texture) {
                    // Download the exact full-window composition submitted to
                    // the drawable. Do not assume the backend's swapchain
                    // permits readback or reconfigure its native layer.
                    SDL_GPUBlitInfo final_blit{};
                    final_blit.source = {composition, 0, 0, 0, 0, w, h};
                    final_blit.destination = {swapchain, 0, 0, 0, 0, w, h};
                    final_blit.load_op = SDL_GPU_LOADOP_DONT_CARE;
                    final_blit.filter = SDL_GPU_FILTER_NEAREST;
                    SDL_BlitGPUTexture(command.value, &final_blit);
                    auto *copy = checked(SDL_BeginGPUCopyPass(command.value), "Begin window capture download");
                    SDL_GPUTextureRegion from{composition, 0, 0, 0, 0, 0, w, h, 1};
                    SDL_GPUTextureTransferInfo to{capture.download, 0, w, h};
                    SDL_DownloadFromGPUTexture(copy, &from, &to);
                    SDL_EndGPUCopyPass(copy);
                }
            }
        }
        if (capture.texture) {
            capture.fence = checked(SDL_SubmitGPUCommandBufferAndAcquireFence(command.take()), "Submit window capture");
            last_presented = command.acquired_swapchain;
            if (!SDL_WaitForGPUFences(device, true, &capture.fence, 1)) fail("Wait for window capture");
            auto *pixels = checked(SDL_MapGPUTransferBuffer(device, capture.download, false), "Map window capture");
            capture.mapped = true;
            std::unique_ptr<SDL_Surface, decltype(&SDL_DestroySurface)> image(
                checked(SDL_CreateSurfaceFrom(int(w), int(h), capture.pixel_format, pixels, int(w * 4)),
                        "Create window capture image"), SDL_DestroySurface);
            if (!SDL_SavePNG(image.get(), capture_path)) fail("Save window capture PNG");
        } else {
            if (!output.empty()) finish_readback(command, output);
            else if (!SDL_SubmitGPUCommandBuffer(command.take())) fail("Submit GPU frame");
            last_presented = command.acquired_swapchain;
        }
        rendered = true;
        rendered_post = apply_post;
    }
};

GpuVideo::GpuVideo(SDL_Window *window, GameVideoOptions options, std::span<const uint8_t> playfield_assets,
                   std::span<const uint8_t> sprite_assets, bool linear, bool vsync,
                   VideoInterpolation interpolation, InterpolationFields fields) : impl_(std::make_unique<Impl>()) {
    impl_->window = window; impl_->options = options; impl_->linear = linear;
    impl_->vsync = vsync;
    impl_->interpolation = interpolation;
    impl_->fields = fields;
    impl_->init(playfield_assets, sprite_assets);
}
GpuVideo::~GpuVideo() = default;
const char *GpuVideo::driver() const { return SDL_GetGPUDeviceDriver(impl_->device); }
const InterpolationStats &GpuVideo::last_interpolation() const { return impl_->interpolation_stats; }
void GpuVideo::set_scale(unsigned scale) { impl_->set_scale(scale); }
void GpuVideo::set_scale_mode(VideoScaleMode mode) { impl_->scale_mode = mode; }
SDL_GPUDevice *GpuVideo::device() const { return impl_->device; }
void GpuVideo::set_linear(bool linear) { impl_->linear = linear; }
void GpuVideo::set_postprocess(Postprocess preset, const std::filesystem::path &path) {
    impl_->set_postprocess(preset, path);
    impl_->comparison_native_valid = false;
}
Postprocess GpuVideo::postprocess() const { return impl_->postprocess; }
void GpuVideo::set_overlay(Overlay callback, void *userdata) {
    impl_->overlay = callback;
    impl_->overlay_userdata = userdata;
}
void GpuVideo::draw(const CapturedFrame &scene, std::span<uint32_t> output, unsigned layer_mask, bool process_diagnostic) {
    impl_->draw(scene, output, layer_mask, process_diagnostic);
}
bool GpuVideo::present(const CapturedFrame &scene) {
    impl_->draw(scene, {}, all_layers, false, false, 1.0f, nullptr, {}, Impl::Presentation::SkipBusy);
    return impl_->last_presented;
}
void GpuVideo::capture_motion(const CapturedFrame &scene, uint64_t frame) {
    if (!impl_->motion) {
        if (impl_->window && !SDL_SetGPUAllowedFramesInFlight(impl_->device, 1))
            fail("Set temporal GPU queue depth");
        impl_->motion = std::make_unique<GpuMotionHistory>(impl_->options);
#ifdef __APPLE__
        if (impl_->window && impl_->vsync)
            impl_->motion_pacing = create_macos_motion_pacing(impl_->window);
#endif
    }
    impl_->motion->capture(scene, frame);
    impl_->comparison_native_valid = false;
}
void GpuVideo::reset_motion() {
    if (impl_->motion) impl_->motion->reset();
    impl_->motion_stats = {};
    impl_->comparison_native_valid = false;
    impl_->motion_pace_prepared = false;
}
const MotionInterpolationStats &GpuVideo::last_motion() const { return impl_->motion_stats; }
bool GpuVideo::pace_motion() {
#ifdef __APPLE__
    if (impl_->motion_pacing && impl_->vsync && !impl_->motion_pace_prepared) {
        wait_macos_motion_pacing(impl_->motion_pacing);
        impl_->motion_pace_prepared = true;
    }
#endif
    return impl_->motion_pace_prepared;
}
void GpuVideo::draw_motion(const CapturedFrame &scene, float alpha, std::span<uint32_t> output,
                           unsigned layer_mask, bool process_diagnostic) {
    impl_->motion_stats = {};
    impl_->draw(scene, output, layer_mask, process_diagnostic, true, alpha);
}
bool GpuVideo::present_motion(const CapturedFrame &scene, std::chrono::steady_clock::time_point frame_start,
                              std::chrono::nanoseconds period, bool wait) {
    impl_->motion_stats = {};
    impl_->draw(scene, {}, all_layers, false, true, 1.0f, &frame_start, period,
                wait ? Impl::Presentation::Wait : Impl::Presentation::SkipBusy);
    return impl_->last_presented;
}
void GpuVideo::draw_motion_comparison_timed(const CapturedFrame &scene,
                                           std::chrono::steady_clock::time_point frame_start,
                                           std::chrono::nanoseconds period, const char *capture_path) {
    impl_->motion_stats = {};
    impl_->draw(scene, {}, all_layers, false, true, 1.0f, &frame_start, period, Impl::Presentation::Wait, true, capture_path);
}
bool GpuVideo::last_presented() const { return impl_->last_presented; }
double GpuVideo::display_hz() const {
    if (!impl_->window) return 0;
    const auto *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(impl_->window));
    return mode ? mode->refresh_rate : 0;
}
double GpuVideo::display_callback_hz() const {
#ifdef __APPLE__
    return macos_motion_callback_hz(impl_->motion_pacing);
#else
    return 0;
#endif
}
double GpuVideo::requested_display_hz() const {
#ifdef __APPLE__
    return macos_motion_requested_hz(impl_->motion_pacing);
#else
    return 0;
#endif
}
void GpuVideo::save_surface(const char *path) {
    if (!impl_->rendered) throw std::runtime_error("No GPU surface has been drawn");
    impl_->saved_pixels.resize(size_t(impl_->options.width()) * impl_->options.height());
    Command command(impl_->device);
    impl_->queue_download(command.value, impl_->rendered_post ? impl_->post_surface : impl_->surface);
    impl_->finish_readback(command, impl_->saved_pixels);
    auto *surface = checked(SDL_CreateSurfaceFrom(int(impl_->options.width()), int(impl_->options.height()),
        SDL_PIXELFORMAT_ARGB8888, impl_->saved_pixels.data(), int(impl_->options.width() * 4)), "Create GPU capture surface");
    const char *extension = std::strrchr(path, '.');
    bool saved = extension && SDL_strcasecmp(extension, ".png") == 0 ? SDL_SavePNG(surface, path) : SDL_SaveBMP(surface, path);
    SDL_DestroySurface(surface);
    if (!saved) fail("Save GPU surface");
}
} // namespace f3rt
