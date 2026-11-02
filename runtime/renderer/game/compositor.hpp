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
    uint32_t layer_mask = ALL_LAYERS;     // layers outside the mask are treated as disabled
    bool flipped = false;
    // Expanded presentation: playfield rows go through presented_playfield() (full-resolution
    // alternate maps). Ignored by the fixed native frame, which is always canonical.
    bool presented = false;
};

// Where to draw. The sprite plane is rasterized at `geometry` (432x256 native,
// width*height expanded), so it belongs to the target rather than the scene.
struct SceneTarget {
    std::span<const uint16_t> sprites;
    std::span<uint32_t> output;
    GameVideoOptions geometry{};
};

// Parallel dispatches any geometry to the row workers; Serial runs the same
// row kernels on the caller (measurement/reference use).
enum class ComposeMode { Parallel, Serial };

// Neither mode changes emulated state. Concurrent calls must provide disjoint
// output storage.
void compose_game_scene(const FrameScene &scene, const SceneTarget &target,
                        ComposeMode mode = ComposeMode::Parallel);

// The canonical row kernel for one row range, on the caller. Row `y` lands at
// output[(y - row_base) * scale + sub_y] so a caller composing chunk-local output (the indexed
// compositor's fallback rows) can address its own rows, while the sprite plane stays indexed by
// the frame-wide row. compose_game_scene passes row_base = geometry::FIRST_LINE.
// The target must satisfy the same buffer requirements; the scene is not validated here.
void compose_scene_rows(const FrameScene &scene, const SceneTarget &target, unsigned row_base,
                        unsigned begin, unsigned end);
} // namespace f3rt
