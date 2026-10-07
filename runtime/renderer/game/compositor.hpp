#pragma once
#include "f3rt/game_video.hpp"
#include "renderer/game/scene.hpp"
#include <cstdint>
#include <span>

namespace f3rt {
class GameTiles;
class GameText;

// Immutable inputs for one frame: views only; they must outlive the call.
// No FDP RAM or oracle state enters here. Inputs must remain immutable until
// compose_game_scene returns.
struct FrameScene {
    const GameTiles &tiles;
    const GameText &text;
    std::span<const SceneRow, 256> rows;
    std::span<const uint8_t> tile_pixels; // playfield asset ROM
    std::span<const uint32_t> colors;     // 8192 xRGB
    uint32_t layer_mask = all_layers;     // layers outside the mask are treated as disabled
    bool flipped = false;
};

// Where to draw. The sprite plane is rasterized at `geometry` (432x256 native,
// width*height expanded), so it belongs to the target rather than the scene.
struct SceneTarget {
    std::span<const uint16_t> sprites;
    std::span<uint32_t> output;
    GameVideoOptions geometry{};
};

// Parallel dispatches expanded frames to the row workers; Serial runs the same
// row kernel on the caller (measurement/reference use).
enum class ComposeMode { Parallel, Serial };

// Neither mode changes emulated state. Concurrent calls must provide disjoint
// output storage.
void compose_game_scene(const FrameScene &scene, const SceneTarget &target,
                        ComposeMode mode = ComposeMode::Parallel);
} // namespace f3rt
