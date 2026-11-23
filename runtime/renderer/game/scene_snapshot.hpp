#pragma once
#include "f3rt/game_video.hpp"
#include "renderer/game/indexed_compositor.hpp"
#include "renderer/game/palette.hpp"
#include "renderer/game/scene.hpp"
#include "renderer/game/text.hpp"
#include "renderer/game/tiles.hpp"
#include <array>
#include <cstdint>
#include <span>
#include <vector>

namespace f3rt {
class GameLines;
class GameSprites;
class RenderEnhancements;
class RowWorkers;
class WorkWindow;

// Host-only, CPU-typed snapshot of one decoded scanout frame; never serialized
// into machine state. GameVideo overwrites its single instance at
// VBSTART after the layer components decode video RAM. Everything downstream
// encoder (runtime/renderer/gpu/encode.hpp) into its own word layout once per
// present. Nothing in renderer/game knows about the GPU buffer layout.
struct SceneSnapshot {
    static constexpr size_t MAX_SPRITES = MAX_PRESENTED_SPRITES, PALETTE_SIZE = GamePalette::SIZE;
    GameTiles tiles;
    GameText text;
    std::array<SceneRow, 256> rows{};
    std::array<SceneSprite, MAX_SPRITES> sprites{};
    unsigned sprite_count = 0;
    std::array<SceneSprite, MAX_HARDWARE_SPRITES> canonical_sprites{};
    unsigned canonical_sprite_count = 0;
    std::array<uint32_t, PALETTE_SIZE> colors{}; // 0x00RRGGBB, indexed by palette entry
    uint64_t rows_version = 0, colors_version = 0;
    uint8_t pen_mask = 15;
    bool flipped = false;
    bool sprite_flipped = false;
    std::vector<uint16_t> sprite_trail_seed;
    // Oracle-fallback frame: only native_pixels (320 x geometry::HEIGHT ARGB) is meaningful.
    bool fallback = true;
    std::array<uint32_t, geometry::NATIVE_PIXELS> native_pixels{};

    std::span<const SceneSprite> sprite_list() const { return {sprites.data(), sprite_count}; }
    std::span<const SceneSprite> sprite_list(SpriteList list) const;
    bool presents_enhanced_sprites() const;
    std::span<const SceneSprite> canonical_sprite_list() const {
        return {canonical_sprites.data(), canonical_sprite_count};
    }

    SceneSnapshot &capture(const GameTiles &tiles, const GameText &text, const GamePalette &palette,
                           const GameSprites &sprites, const GameLines &lines,
                           const RenderEnhancements &enhancements);
    // Make this an oracle-fallback frame: `native` (320 x geometry::HEIGHT ARGB) becomes
    // native_pixels, or opaque black when empty.
    SceneSnapshot &capture_fallback(std::span<const uint32_t> native = {});
    // Scale-1 CPU indexed composition with optional symmetric horizontal `border` columns.
    void compose(std::span<uint32_t> output, size_t stride, PixelFormat format, unsigned border,
                 const TileRowTable &playfield_tiles, std::span<const uint8_t> sprite_tiles,
                 IndexedScratch &scratch, SpriteList list = SpriteList::Presented,
                 RowWorkers *workers = nullptr, WorkWindow *window = nullptr) const;

private:
    void sync_versioned_layers(const GameTiles &tiles, const GameText &text,
                               const GamePalette &palette, const GameLines &lines);
    void copy_frame_sprites(const GameSprites &sprites, const RenderEnhancements &enhancements);
};

} // namespace f3rt
