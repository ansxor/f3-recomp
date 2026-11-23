#pragma once
#include "renderer/gpu/encode.hpp"
#include "renderer/gpu/sdl_gpu.hpp"
#include <cstdint>
#include <span>
#include <vector>

namespace f3rt {
class GpuTileAssets {
public:
    // 256 bytes per 16x16 tile.
    static constexpr uint32_t TILE_BYTES = 256;
    // Throws std::runtime_error unless both ROMs are non-empty whole tiles that fit 32-bit offsets.
    static void validate(std::span<const uint8_t> pf_rom, std::span<const uint8_t> sp_rom);
    // Packs both ROMs low-byte-first (also correct on big-endian hosts) and uploads them once.
    GpuTileAssets(const GpuContext &ctx, std::span<const uint8_t> pf_rom,
                  std::span<const uint8_t> sp_rom);

    const GpuBuffer &pf_tiles() const noexcept { return m_pf_tiles; }
    const GpuBuffer &sp_tiles() const noexcept { return m_sp_tiles; }
    uint32_t pf_tile_count() const noexcept { return m_pf_tile_count; }
    uint32_t sp_tile_count() const noexcept { return m_sp_tile_count; }
    // One 64-bit pen-presence mask per playfield tile.
    std::span<const uint64_t> tile_pen_masks() const noexcept { return m_tile_pen_masks; }

private:
    GpuBuffer m_pf_tiles, m_sp_tiles;
    uint32_t m_pf_tile_count = 0, m_sp_tile_count = 0;
    std::vector<uint64_t> m_tile_pen_masks;
};

class ScenePass {
public:
    struct Targets {
        GpuTexture sprite_plane, sprite_plane_b, surface;
    };

    ScenePass(const GpuContext &ctx, GameVideoOptions options, bool enable_interpolation);
    // Created on first use, so an interpolation-off session never compiles the shader. Throws
    // before changing any state.
    void ensure_interpolation_pipeline(const GpuContext &ctx);
    const GpuTransfer &upload_buffer() const noexcept { return m_upload; }
    Targets reallocate_targets(const GpuContext &ctx, GameVideoOptions options) const;
    void commit_targets(Targets next) noexcept;
    GpuSurface draw(GpuCommand &cmd, const EncodedScene &encoded, const GpuContext &ctx,
                    const GpuTileAssets &assets, GameVideoOptions options);

private:
    void upload_trail_seed(SDL_GPUCopyPass *copy, const GpuContext &ctx,
                           std::span<const uint16_t> seed, GameVideoOptions options);
    GpuBuffer m_scene_buffer, m_native_buffer;
    GpuTransfer m_upload, m_trail_upload;
    GpuPipeline m_sprite_pipeline, m_scene_pipeline, m_interpolation_pipeline;
    Targets m_targets;
};
}
