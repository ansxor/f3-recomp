#pragma once
#include "f3rt/game_video.hpp"
#include "game_scene.hpp"
#include <array>
#include <cstdint>

namespace f3rt {
// Host-only scanout snapshot; never serialized into machine/netplay state.
// Word layout is mirrored by shader constants in runtime/shaders/scene.glsl.
struct GpuScene {
    static constexpr unsigned pf_cells = 0;
    static constexpr unsigned text_cells = 16384;
    static constexpr unsigned glyphs = 20480;
    static constexpr unsigned palette = 24576;
    static constexpr unsigned rows = 32768;
    static constexpr unsigned row_stride = 352;
    static constexpr unsigned row_layers = 16;
    static constexpr unsigned layer_stride = 34;
    static constexpr unsigned row_pf = row_layers + 9 * layer_stride;
    static constexpr unsigned sprites = rows + 256 * row_stride;
    static constexpr unsigned sprite_stride = 8;
    static constexpr unsigned word_count = sprites + 1024 * sprite_stride;
    std::array<uint32_t, word_count> words{};
    // Original clip/control rows retained for lazy CPU capture/snapshot rendering.
    // Not uploaded: the GPU reads the normalized packed rows in words.
    std::array<SceneRow, 256> reference_rows{};
    std::array<uint32_t, 320 * 232> native_pixels{};
    unsigned sprite_count = 0;
    unsigned pen_mask = 15;
    bool fallback = true;
};
struct GpuUniforms {
    uint32_t scale, border, width, height;
    uint32_t sprite_count, pen_mask, fallback, layer_mask;
};
} // namespace f3rt
