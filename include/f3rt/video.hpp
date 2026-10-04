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

// Standalone TC0630FDP Software Renderer for Taito F3 (Land Maker)
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

    // Decode ROMs into internal tile caches (called once on startup)
    // - sprites: 4MB (low 4 bpp packed LSB)
    // - sprites_hi: 2MB (high 2 bpp)
    // - tilemap: 4MB (low 4 bpp packed LSB)
    // - tilemap_hi: 2MB (high 2 bpp)
    bool load_roms(std::span<const uint8_t> sprites,
                   std::span<const uint8_t> sprites_hi,
                   std::span<const uint8_t> tilemap,
                   std::span<const uint8_t> tilemap_hi);

    // Render one 320x232 frame into output buffer (ARGB8888, 320*232 uint32_t pixels)
    // - palette_ram: 0x8000 bytes BE (8192 colors x 32-bit dword at 0x440000)
    // - graphics_ram: 0x40000 bytes BE (0x600000..0x63ffff)
    // - control_regs: 0x20 bytes BE (0x660000..0x66001f, 16 words)
    // - output_argb: span of at least 320 * 232 uint32_t elements
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

    // Diagnostic source-layer readback; does not advance sprite/frame state.
    // The returned line is invalidated by the next renderer/inspection call.
    VideoLine inspect_playfield_line(unsigned layer, int y,
                                     std::span<const uint8_t> graphics_ram);
    // 432x256 indexed plane currently prepared for the next render_frame call.
    std::span<const uint16_t> sprite_plane() const;

    bool roms_loaded() const;
    bool flipscreen() const;

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace f3rt
