#pragma once
#include <cstdint>
#include <span>

namespace f3rt {
class GameTiles;
class GameText;
class GameLines;

// Composes game-owned scene descriptions. No FDP RAM or oracle state enters here.
void compose_game_scene(const GameTiles &tiles, const GameText &text, const GameLines &lines,
                        std::span<const uint16_t> sprites, bool flipped,
                        std::span<const uint8_t> tile_pixels, std::span<const uint8_t> palette,
                        std::span<uint32_t> output);
} // namespace f3rt
