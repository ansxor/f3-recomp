#include "renderer/game/captured_frame.hpp"
#include "renderer/game/sprites.hpp"
#include <stdexcept>

namespace f3rt {
void raster_captured_sprites(const CapturedFrame &frame, std::span<const uint8_t> sprite_tiles,
                             std::span<uint16_t> plane, GameVideoOptions options) {
    raster_sprites(frame.sprite_list(), frame.pen_mask, sprite_tiles, plane, {options});
}

void compose_captured_scene(const CapturedFrame &frame, std::span<const uint8_t> tile_pixels,
                            std::span<const uint16_t> plane, std::span<uint32_t> output,
                            GameVideoOptions options, ComposeMode mode, uint32_t layer_mask) {
    if (!options.scale || options.scale > GameVideoOptions::MAX_GPU_SCALE ||
        options.border > GameVideoOptions::MAX_BORDER ||
        output.size() < size_t(options.width()) * options.height())
        throw std::runtime_error("Incomplete captured frame render buffers");
    if (frame.fallback) {
        // Oracle-fallback frames carry only the canonical native picture; present it letterboxed.
        const unsigned left = options.border * options.scale;
        for (unsigned y = 0; y < options.height(); ++y)
            for (unsigned x = 0; x < options.width(); ++x)
                output[size_t(y) * options.width() + x] =
                    x < left || x >= left + geometry::NATIVE_WIDTH * options.scale
                        ? 0xff000000u
                        : frame.native_pixels[(y / options.scale) * geometry::NATIVE_WIDTH +
                                              (x - left) / options.scale];
        return;
    }
    compose_game_scene(frame.scene(tile_pixels, layer_mask), {plane, output, options}, mode);
}
} // namespace f3rt
