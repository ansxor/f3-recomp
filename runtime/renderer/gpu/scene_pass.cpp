#include "renderer/gpu/scene_pass.hpp"
#include <algorithm>
#include <stdexcept>

namespace f3rt {
namespace {
constexpr uint32_t INTERPOLATION_SCENE_BYTES = ENCODED_INTERPOLATION_WORDS * sizeof(uint32_t);
constexpr uint32_t NATIVE_BYTES = ENCODED_NATIVE_WORDS * sizeof(uint32_t);
// Sprite hardware plane at native resolution: 432 x 256 scanout lines.
constexpr unsigned NATIVE_PLANE_WIDTH = 432, NATIVE_PLANE_HEIGHT = 256;
constexpr int NATIVE_FIRST_COLUMN = 46, NATIVE_END_COLUMN = 366;

uint32_t pack_word(std::span<const uint8_t> bytes, size_t at) {
    return uint32_t(bytes[at]) | (uint32_t(bytes[at + 1]) << 8) | (uint32_t(bytes[at + 2]) << 16) |
           (uint32_t(bytes[at + 3]) << 24);
}
GpuTexture sprite_plane(const GpuContext &ctx, GameVideoOptions options) {
    return options.expanded() ? ctx.create_texture(SDL_GPU_TEXTUREFORMAT_R32_UINT, options.width(),
                                                   options.height())
                              : ctx.create_texture(SDL_GPU_TEXTUREFORMAT_R32_UINT,
                                                   NATIVE_PLANE_WIDTH, NATIVE_PLANE_HEIGHT);
}
}

void GpuTileAssets::validate(std::span<const uint8_t> pf_rom, std::span<const uint8_t> sp_rom) {
    if (pf_rom.empty() || sp_rom.empty() || pf_rom.size() % TILE_BYTES ||
        sp_rom.size() % TILE_BYTES || pf_rom.size() > UINT32_MAX / 2 ||
        sp_rom.size() > UINT32_MAX / 2 || pf_rom.size() + sp_rom.size() > UINT32_MAX)
        throw std::runtime_error("Incomplete GPU tile/sprite assets");
}

GpuTileAssets::GpuTileAssets(const GpuContext &ctx, std::span<const uint8_t> pf_rom,
                             std::span<const uint8_t> sp_rom) {
    validate(pf_rom, sp_rom);
    const auto pf_bytes = uint32_t(pf_rom.size()), sp_bytes = uint32_t(sp_rom.size());
    m_pf_tile_count = pf_bytes / TILE_BYTES;
    m_sp_tile_count = sp_bytes / TILE_BYTES;
    m_pf_tiles = ctx.create_buffer(pf_bytes);
    m_sp_tiles = ctx.create_buffer(sp_bytes);
    m_tile_pen_masks.assign(m_pf_tile_count, 0);
    const GpuTransfer staging =
        ctx.create_transfer(pf_bytes + sp_bytes, SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
    {
        const auto mapping = staging.map(false, "Map GPU asset upload");
        const auto words = mapping.words((size_t(pf_bytes) + sp_bytes) / 4);
        for (uint32_t i = 0; i < pf_bytes; i += 4) {
            words[i / 4] = pack_word(pf_rom, i);
            m_tile_pen_masks[i / TILE_BYTES] |=
                (uint64_t{1} << (pf_rom[i] & 63)) | (uint64_t{1} << (pf_rom[i + 1] & 63)) |
                (uint64_t{1} << (pf_rom[i + 2] & 63)) | (uint64_t{1} << (pf_rom[i + 3] & 63));
        }
        for (uint32_t i = 0; i < sp_bytes; i += 4) words[(pf_bytes + i) / 4] = pack_word(sp_rom, i);
    }
    auto cmd = ctx.begin_command();
    auto *copy = cmd.begin_copy_pass("Begin GPU asset copy");
    GpuCommand::upload_buffer(copy, staging, m_pf_tiles, 0, pf_bytes, false);
    GpuCommand::upload_buffer(copy, staging, m_sp_tiles, pf_bytes, sp_bytes, false);
    SDL_EndGPUCopyPass(copy);
    cmd.submit();
}

ScenePass::ScenePass(const GpuContext &ctx, GameVideoOptions options, bool enable_interpolation) {
    m_scene_buffer = ctx.create_buffer(INTERPOLATION_SCENE_BYTES);
    m_native_buffer = ctx.create_buffer(NATIVE_BYTES);
    m_targets = reallocate_targets(ctx, options);
    m_sprite_pipeline = ctx.create_pipeline(video_shaders::sprite_vert, video_shaders::sprite_frag,
                                            SDL_GPU_TEXTUREFORMAT_R32_UINT);
    m_scene_pipeline = ctx.create_pipeline(video_shaders::fullscreen_vert, video_shaders::scene_frag,
                                           SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);
    if (enable_interpolation) ensure_interpolation_pipeline(ctx);
    m_upload = ctx.create_transfer(std::max(INTERPOLATION_SCENE_BYTES, NATIVE_BYTES),
                                   SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
}

void ScenePass::ensure_interpolation_pipeline(const GpuContext &ctx) {
    if (!m_interpolation_pipeline)
        m_interpolation_pipeline =
            ctx.create_pipeline(video_shaders::fullscreen_vert, video_shaders::scene_interp_frag,
                                SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM);
}

ScenePass::Targets ScenePass::reallocate_targets(const GpuContext &ctx,
                                                 GameVideoOptions options) const {
    Targets next;
    next.sprite_plane = sprite_plane(ctx, options);
    next.sprite_plane_b = sprite_plane(ctx, options);
    next.surface = ctx.create_texture(SDL_GPU_TEXTUREFORMAT_B8G8R8A8_UNORM, options.width(),
                                      options.height());
    return next;
}

void ScenePass::commit_targets(Targets next) noexcept {
    m_targets = std::move(next);
    m_trail_upload.reset();
}

void ScenePass::upload_trail_seed(SDL_GPUCopyPass *copy, const GpuContext &ctx,
                                  std::span<const uint16_t> seed, GameVideoOptions options) {
    const bool expanded = options.expanded();
    const unsigned width = expanded ? options.width() : NATIVE_PLANE_WIDTH;
    const unsigned height = expanded ? options.height() : NATIVE_PLANE_HEIGHT;
    if (!m_trail_upload)
        m_trail_upload = ctx.create_transfer(width * height * sizeof(uint32_t),
                                             SDL_GPU_TRANSFERBUFFERUSAGE_UPLOAD);
    {
        const auto mapping = m_trail_upload.map(true, "Map GPU sprite trails");
        const auto mapped = mapping.words(size_t(width) * height);
        if (!expanded) {
            std::copy(seed.begin(), seed.end(), mapped.begin());
        } else {
            for (unsigned y = 0; y < height; ++y) {
                const unsigned native_y = y / options.scale + geometry::FIRST_LINE;
                for (unsigned x = 0; x < width; ++x) {
                    const int native_x =
                        int(x / options.scale) + NATIVE_FIRST_COLUMN - int(options.border);
                    mapped[size_t(y) * width + x] =
                        native_x >= NATIVE_FIRST_COLUMN && native_x < NATIVE_END_COLUMN
                            ? seed[size_t(native_y) * NATIVE_PLANE_WIDTH + native_x]
                            : 0;
                }
            }
        }
    }
    const SDL_GPUTextureTransferInfo source{m_trail_upload.get(), 0, width, height};
    for (auto *plane : {m_targets.sprite_plane.get(), m_targets.sprite_plane_b.get()}) {
        const SDL_GPUTextureRegion destination{plane, 0, 0, 0, 0, 0, width, height, 1};
        SDL_UploadToGPUTexture(copy, &source, &destination, true);
    }
}

GpuSurface ScenePass::draw(GpuCommand &cmd, const EncodedScene &encoded, const GpuContext &ctx,
                           const GpuTileAssets &assets, GameVideoOptions options) {
    auto *copy = cmd.begin_copy_pass("Begin GPU frame copy");
    GpuCommand::upload_buffer(copy, m_upload, encoded.fallback ? m_native_buffer : m_scene_buffer,
                              0, encoded.upload_bytes, true);
    if (encoded.has_trails) upload_trail_seed(copy, ctx, encoded.trail_seed, options);
    SDL_EndGPUCopyPass(copy);

    GpuUniforms uniforms = encoded.uniforms;
    SDL_GPUBuffer *scene_buffer = m_scene_buffer.get();
    SDL_GPUBuffer *sp_tiles = assets.sp_tiles().get();
    if (!encoded.fallback) {
        const auto sprite_pass = [&](SDL_GPUTexture *plane, SDL_GPUTexture *seed, uint32_t mask) {
            auto pass_uniforms = uniforms;
            pass_uniforms.layer_mask |= mask;
            SDL_PushGPUVertexUniformData(cmd.raw(), 0, &pass_uniforms, sizeof(pass_uniforms));
            auto *pass = cmd.begin_render_pass(plane, encoded.has_trails);
            SDL_BindGPUGraphicsPipeline(pass, m_sprite_pipeline.get());
            if (!options.expanded()) {
                SDL_Rect scissor{NATIVE_FIRST_COLUMN, int(geometry::FIRST_LINE),
                                 int(geometry::NATIVE_WIDTH), int(geometry::HEIGHT)};
                SDL_SetGPUScissor(pass, &scissor);
            }
            SDL_BindGPUVertexStorageBuffers(pass, 0, &scene_buffer, 1);
            const SDL_GPUTextureSamplerBinding trail_binding{seed, ctx.sampler()};
            SDL_BindGPUFragmentSamplers(pass, 0, &trail_binding, 1);
            SDL_BindGPUFragmentStorageBuffers(pass, 0, &sp_tiles, 1);
            SDL_DrawGPUPrimitives(pass, 6, uniforms.sprite_count, 0, 0);
            SDL_EndGPURenderPass(pass);
        };
        SDL_GPUTexture *plane = m_targets.sprite_plane.get(), *plane_b = m_targets.sprite_plane_b.get();
        sprite_pass(plane, plane_b, 0);
        if (encoded.blend_weighted) sprite_pass(plane_b, plane, F3_MASK_HIDE_WEIGHTED);
    }
    if (encoded.blend_weighted) uniforms.layer_mask |= F3_MASK_BLEND_WEIGHTED;
    SDL_PushGPUFragmentUniformData(cmd.raw(), 0, &uniforms, sizeof(uniforms));
    auto *pass = cmd.begin_render_pass(m_targets.surface.get());
    SDL_BindGPUGraphicsPipeline(pass, encoded.use_interpolation ? m_interpolation_pipeline.get()
                                                                : m_scene_pipeline.get());
    const SDL_GPUTextureSamplerBinding sprite_bindings[]{
        {m_targets.sprite_plane.get(), ctx.sampler()},
        {encoded.blend_weighted ? m_targets.sprite_plane_b.get() : m_targets.sprite_plane.get(),
         ctx.sampler()}};
    SDL_BindGPUFragmentSamplers(pass, 0, sprite_bindings, 2);
    SDL_GPUBuffer *buffers[]{scene_buffer, assets.pf_tiles().get(), m_native_buffer.get()};
    SDL_BindGPUFragmentStorageBuffers(pass, 0, buffers, 3);
    SDL_DrawGPUPrimitives(pass, 3, 1, 0, 0);
    SDL_EndGPURenderPass(pass);
    return m_targets.surface.surface();
}
}
