// license:BSD-3-Clause
// copyright-holders:Bryan McPhail, ywy, 12Me21, f3rt authors
#pragma once

#include <cstdint>
#include <memory>
#include <span>

namespace f3rt {

constexpr int SCREEN_WIDTH  = 320;
constexpr int SCREEN_HEIGHT = 232;

struct VideoLine {
    std::span<const uint16_t> palette;
    std::span<const uint8_t> flags;
};
struct SceneRow;
struct VideoConfig;


// Standalone TC0630FDP software renderer for configured Taito F3 games
class Video {
public:
    Video();
    ~Video();

    Video(const Video &) = delete;
    Video &operator=(const Video &) = delete;
    Video(Video &&) noexcept;
    Video &operator=(Video &&) noexcept;

    // Reset video state (framebuffers, sprite list, latches, etc.)
    void reset();

    // Decode 16x16 low 4-bpp tiles; optional high 2-bpp data has half the low size.
    // Region lengths determine the independent sprite/playfield tile counts.
    bool load_roms(std::span<const uint8_t> sprites,
                   std::span<const uint8_t> sprites_hi,
                   std::span<const uint8_t> tilemap,
                   std::span<const uint8_t> tilemap_hi,
                   const VideoConfig &config);

    // Render one 320-wide frame into output buffer (ARGB8888, configured visible height)
    // - palette_ram: 0x8000 bytes BE (8192 colors x 32-bit dword at 0x440000)
    // - graphics_ram: 0x40000 bytes BE (0x600000..0x63ffff)
    // - control_regs: 0x20 bytes BE (0x660000..0x66001f, 16 words)
    // - output_argb: span of at least 320 * configured visible_height uint32_t elements
    void render_frame(std::span<const uint8_t> palette_ram,
                      std::span<const uint8_t> graphics_ram,
                      std::span<const uint8_t> control_regs,
                      std::span<uint32_t> output_argb);

    // Advance / vblank hook:
    // If not called separately, render_frame automatically handles the sprite_lag=1
    // frame sequence (render using previous frame's sprites, then buffer current spriteram).
    void vblank(std::span<const uint8_t> graphics_ram);

    // Direct injection of active spriteram (for single-frame capture tests)
    void set_active_spriteram(std::span<const uint8_t> spriteram);

    // Immutable decoded assets can be shared with the independent game renderer.
    std::span<const uint8_t> sprite_tiles() const;
    std::span<const uint8_t> playfield_tiles() const;

    // Diagnostic physical-map readback; does not advance sprite/frame state.
    // Extended mode exposes maps 0..3 at width 1024, or 0..5 with
    // extended_alt_maps. Nonextended mode exposes maps 0..7 at width 512.
    // PF2/PF3 alternate maps are 4/5 in either capable layout.
    // The returned line is invalidated by the next renderer/inspection call.
    VideoLine inspect_playfield_line(unsigned layer, int y,
                                     std::span<const uint8_t> graphics_ram);
    void prepare_text_inspection(std::span<const uint8_t> graphics_ram);
    VideoLine inspect_text_line(int y, std::span<const uint8_t> graphics_ram);
    // 432x256 indexed plane currently prepared for the next render_frame call.
    std::span<const uint16_t> sprite_plane() const;

    bool roms_loaded() const;
    bool flipscreen() const;
    const SceneRow &inspect_scene_row(unsigned scanout_y) const;
    void enable_scene_inspection(bool enable = true);

    // State snapshot serialization
    size_t state_size() const;
    void save_state(std::span<uint8_t> dst) const;
    void load_state(std::span<const uint8_t> src);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace f3rt
