#pragma once
#include "f3rt/game_video.hpp"
#include "renderer/game/scene.hpp"
#include <cstdint>
#include <span>

namespace f3rt {
class GameTiles;
class GameText;

struct FrameScene {
    const GameTiles &tiles;
    const GameText &text;
    std::span<const SceneRow, 256> rows;
    std::span<const uint8_t> tile_pixels;
    std::span<const uint32_t> colors;
    uint32_t layer_mask = ALL_LAYERS;
    bool flipped = false;
    bool presented = false;
};
} // namespace f3rt
