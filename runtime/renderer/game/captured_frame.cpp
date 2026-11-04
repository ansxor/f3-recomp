#include "renderer/game/captured_frame.hpp"
#include "renderer/game/row_workers.hpp"
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

void compose_captured_indexed(const CapturedFrame &frame, const TileRowTable &playfield_tiles,
                              std::span<const uint8_t> sprite_tiles, IndexedScratch &scratch,
                              std::span<uint32_t> output, size_t stride, PixelFormat format,
                              GameVideoOptions options, WorkWindow *window) {
    if (options.scale != 1 || options.border > GameVideoOptions::MAX_BORDER)
        throw std::runtime_error("Indexed composition needs the expanded scale-1 frame");
    if (stride < options.width() || output.size() < stride * options.height())
        throw std::runtime_error("Incomplete indexed frame render buffers");
    if (frame.fallback) {
        const unsigned left = options.border;
        for (unsigned y = 0; y < options.height(); ++y) {
            uint32_t *row = output.data() + size_t(y) * stride;
            for (unsigned x = 0; x < options.width(); ++x)
                row[x] =
                    x < left || x >= left + geometry::NATIVE_WIDTH
                        ? to_pixel_format(0xff000000u, format)
                        : to_pixel_format(
                              frame.native_pixels[y * geometry::NATIVE_WIDTH + (x - left)], format);
        }
        return;
    }
    scratch.configure(options);
    const FrameScene scene = frame.scene(playfield_tiles.pixels());
    const bool palette_15bit = scratch.prepare(scene, format);
    scratch.stats.reset();
    scratch.bins.bin(frame.sprite_list(), SpriteRasterOptions{options});
    const auto sprite_list = frame.sprite_list();
    IndexedComposeJob job{scene,
                          scratch.plane,
                          scratch.coverage(options),
                          &scratch.bins,
                          sprite_list,
                          frame.pen_mask,
                          sprite_tiles,
                          playfield_tiles.rows(),
                          scratch.glyph_rows,
                          scratch.palette.colors,
                          palette_15bit ? std::span<const uint32_t>(scratch.palette.colors15)
                                        : std::span<const uint32_t>{},
                          output,
                          stride,
                          format,
                          options,
                          &scratch.stats};
    compose_workers().run(
        compose_chunk_count(), [&job](unsigned chunk) { compose_indexed_chunk(job, chunk); },
        window);
}
} // namespace f3rt
