#include "renderer/game/scene_snapshot.hpp"
#include "renderer/game/row_workers.hpp"
#include "renderer/game/sprites.hpp"
#include <stdexcept>

namespace f3rt {
void compose_snapshot_indexed(const SceneSnapshot &frame, const TileRowTable &playfield_tiles,
                              std::span<const uint8_t> sprite_tiles, IndexedScratch &scratch,
                              std::span<uint32_t> output, size_t stride, PixelFormat format,
                              GameVideoOptions options, ComposeMode mode, SpriteList list,
                              WorkWindow *window) {
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
    const auto sprite_list =
        list == SpriteList::Canonical ? frame.canonical_sprite_list() : frame.sprite_list();
    scratch.bins.bin(sprite_list, SpriteRasterOptions{options});
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
    if (mode == ComposeMode::Serial) {
        for (unsigned chunk = 0; chunk < compose_chunk_count(); ++chunk)
            compose_indexed_chunk(job, chunk);
        return;
    }
    compose_workers().run(
        compose_chunk_count(), [&job](unsigned chunk) { compose_indexed_chunk(job, chunk); },
        window);
}
} // namespace f3rt
