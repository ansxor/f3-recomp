#pragma once
#include "f3rt/memory_map.hpp"
#include "renderer/game/ram_shadow.hpp"
#include "renderer/game/scene.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace f3rt {

// Palette RAM at VBSTART, decoded to 0x00RRGGBB per entry. Palette RAM stores one big-endian
// 32-bit word per entry with the color in bytes 1..3.
class GamePalette {
public:
    static constexpr size_t SIZE = PALETTE_RAM_SIZE / 4;

    void reset() {
        m_colors.fill(0);
        m_shadow.invalidate();
        m_version = fresh_content_version();
    }
    // Re-decode only when palette RAM changed since the previous decode. A VideoRam without
    // full palette RAM leaves the colors untouched.
    void decode(const VideoRam &vram) {
        if (vram.palette.size() < PALETTE_RAM_SIZE || !m_shadow.changed(vram.palette.data())) return;
        for (size_t i = 0; i < SIZE; ++i)
            m_colors[i] = (uint32_t(vram.palette[i * 4 + 1]) << 16) |
                          (uint32_t(vram.palette[i * 4 + 2]) << 8) | vram.palette[i * 4 + 3];
        m_version = fresh_content_version();
    }
    const std::array<uint32_t, SIZE> &colors() const { return m_colors; }
    uint64_t version() const { return m_version; }

private:
    RamShadow<PALETTE_RAM_SIZE> m_shadow;
    std::array<uint32_t, SIZE> m_colors{};
    uint64_t m_version = fresh_content_version();
};
static_assert(SceneSource<GamePalette>);

}
