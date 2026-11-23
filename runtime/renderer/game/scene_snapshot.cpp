#include "renderer/game/scene_snapshot.hpp"
#include "f3rt/renderer/game/enhancements.hpp"
#include "renderer/game/lines.hpp"
#include "renderer/game/row_workers.hpp"
#include "renderer/game/sprites.hpp"
#include <algorithm>
#include <stdexcept>

namespace f3rt {

std::span<const SceneSprite> SceneSnapshot::sprite_list(SpriteList list) const {
    return list == SpriteList::Canonical ? canonical_sprite_list() : sprite_list();
}

bool SceneSnapshot::presents_enhanced_sprites() const {
    return !fallback && (!std::ranges::equal(sprite_list(), canonical_sprite_list()) ||
                         std::ranges::any_of(sprite_list(), &SceneSprite::weighted));
}

void SceneSnapshot::sync_versioned_layers(const GameTiles &source_tiles,
                                          const GameText &source_text, const GamePalette &palette,
                                          const GameLines &lines) {
    if (tiles.version() != source_tiles.version()) tiles = source_tiles;
    if (text.version() != source_text.version()) text = source_text;
    if (rows_version != lines.rows_version()) {
        std::ranges::copy(lines.rows(), rows.begin());
        rows_version = lines.rows_version();
    }
    if (colors_version != palette.version()) {
        colors = palette.colors();
        colors_version = palette.version();
    }
}

void SceneSnapshot::copy_frame_sprites(const GameSprites &source_sprites,
                                       const RenderEnhancements &enhancements) {
    const auto presented = source_sprites.presented_for(enhancements);
    sprite_count = unsigned(presented.size());
    std::ranges::copy(presented, sprites.begin());
    const auto canonical = source_sprites.sprites();
    canonical_sprite_count = unsigned(canonical.size());
    std::ranges::copy(canonical, canonical_sprites.begin());
    pen_mask = source_sprites.pen_mask();
    flipped = source_sprites.scanout_flipped();
    sprite_flipped = source_sprites.flipped();
    const auto trail_seed = source_sprites.trail_seed();
    sprite_trail_seed.assign(trail_seed.begin(), trail_seed.end());
}

SceneSnapshot &SceneSnapshot::capture(const GameTiles &source_tiles, const GameText &source_text,
                                      const GamePalette &palette, const GameSprites &source_sprites,
                                      const GameLines &lines,
                                      const RenderEnhancements &enhancements) {
    fallback = false;
    sync_versioned_layers(source_tiles, source_text, palette, lines);
    copy_frame_sprites(source_sprites, enhancements);
    return *this;
}

SceneSnapshot &SceneSnapshot::capture_fallback(std::span<const uint32_t> native) {
    fallback = true;
    if (native.empty()) native_pixels.fill(0xff000000);
    else std::ranges::copy_n(native.begin(), std::min(native.size(), native_pixels.size()),
                             native_pixels.begin());
    return *this;
}

void SceneSnapshot::compose(std::span<uint32_t> output, size_t stride, PixelFormat format,
                            unsigned border, const TileRowTable &playfield_tiles,
                            std::span<const uint8_t> sprite_tiles, IndexedScratch &scratch,
                            SpriteList list, RowWorkers *workers, WorkWindow *window) const {
    if (border > GameVideoOptions::MAX_BORDER)
        throw std::runtime_error("Indexed composition border out of range");
    const GameVideoOptions options{1, border};
    if (stride < options.width() || output.size() < stride * options.height())
        throw std::runtime_error("Incomplete indexed frame render buffers");
    if (fallback) {
        const unsigned left = options.border;
        for (unsigned y = 0; y < options.height(); ++y) {
            uint32_t *row = output.data() + size_t(y) * stride;
            for (unsigned x = 0; x < options.width(); ++x)
                row[x] =
                    x < left || x >= left + geometry::NATIVE_WIDTH
                        ? to_pixel_format(0xff000000u, format)
                        : to_pixel_format(native_pixels[y * geometry::NATIVE_WIDTH + (x - left)],
                                          format);
        }
        return;
    }
    scratch.configure(options);
    const bool palette_15bit = scratch.prepare(*this, format);
    scratch.stats.reset();
    const auto sprite_view = sprite_list(list);
    scratch.bins.bin(sprite_view, SpriteRasterOptions{options, sprite_flipped});
    IndexedComposeJob job{*this,
                          playfield_tiles.pixels(),
                          scratch.plane,
                          scratch.coverage(options),
                          &scratch.bins,
                          sprite_view,
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
    if (!workers) {
        for (unsigned chunk = 0; chunk < compose_chunk_count(); ++chunk)
            compose_indexed_chunk(job, chunk);
        return;
    }
    workers->run(
        compose_chunk_count(), [&job](unsigned chunk) { compose_indexed_chunk(job, chunk); },
        window);
}
} // namespace f3rt
