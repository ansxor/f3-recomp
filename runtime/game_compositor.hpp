#pragma once
#include "f3rt/game_video.hpp"
#include <cstdint>
#include <span>

namespace f3rt {
class GameTiles;
class GameText;
class GameLines;

// Composes game-owned scene descriptions. No FDP RAM or oracle state enters here.
void compose_game_scene(const GameTiles &tiles, const GameText &text, const GameLines &lines,
                        std::span<const uint16_t> sprites, bool flipped,
                        std::span<const uint8_t> tile_pixels, std::span<const uint32_t> colors,
                        std::span<uint32_t> output, GameVideoOptions options = {});

// Measurement/reference entry point; same row kernel, no worker dispatch.
// Neither entry point changes emulated state. Scene inputs must remain immutable
// until return, and concurrent calls must provide disjoint output storage.
void compose_game_scene_serial(const GameTiles &tiles, const GameText &text, const GameLines &lines,
                               std::span<const uint16_t> sprites, bool flipped,
                               std::span<const uint8_t> tile_pixels, std::span<const uint32_t> colors,
                               std::span<uint32_t> output, GameVideoOptions options = {});
} // namespace f3rt
