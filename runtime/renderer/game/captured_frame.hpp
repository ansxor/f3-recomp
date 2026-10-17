#pragma once
#include "f3rt/game_video.hpp"
#include "renderer/game/compositor.hpp"
#include "renderer/game/scene.hpp"
#include "renderer/game/text.hpp"
#include "renderer/game/tiles.hpp"
#include <array>
#include <cstdint>
#include <span>

namespace f3rt {

// Host-only, CPU-typed snapshot of one decoded scanout frame; never serialized
// into machine state. GameVideo overwrites its single instance at
// VBSTART after the layer components decode video RAM. Everything downstream
// reads this value: the CPU compositor directly (scene()), and the GPU
// encoder (runtime/renderer/gpu/encode.hpp) into its own word layout once per
// present. Nothing in renderer/game knows about the GPU buffer layout.
struct CapturedFrame {
    static constexpr size_t MAX_SPRITES = MAX_PRESENTED_SPRITES, PALETTE_SIZE = 8192;
    GameTiles tiles;
    GameText text;
    std::array<SceneRow, 256> rows{};
    std::array<SceneSprite, MAX_SPRITES> sprites{};
    unsigned sprite_count = 0;
    unsigned shadow_count = 0; // sprites tagged SceneSprite::shadow (flicker shadows)
    std::array<uint32_t, PALETTE_SIZE> colors{}; // 0x00RRGGBB, indexed by palette entry
    uint8_t pen_mask = 15;
    bool flipped = false;
    // Oracle-fallback frame: only native_pixels (320 x geometry::HEIGHT ARGB) is meaningful.
    bool fallback = true;
    std::array<uint32_t, geometry::NATIVE_PIXELS> native_pixels{};

    std::span<const SceneSprite> sprite_list() const { return {sprites.data(), sprite_count}; }
    // CPU compositor view. `tile_pixels` is the playfield asset ROM. Expanded outputs of this view are
    // presented (FrameScene::presented); the native frame composed from it is canonical.
    FrameScene scene(std::span<const uint8_t> tile_pixels, uint32_t layer_mask = ALL_LAYERS) const {
        return {tiles, text, rows, tile_pixels, colors, layer_mask, flipped, true};
    }
};

} // namespace f3rt
