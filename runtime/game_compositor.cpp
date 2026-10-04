#include "game_compositor.hpp"
#include "game_lines.hpp"
#include "game_text.hpp"
#include "game_tiles.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>

namespace f3rt {
namespace {
struct ClipRanges {
    std::array<SceneClip, 16> ranges{};
    unsigned count = 1;
};
ClipRanges clip_ranges(const SceneRow &row, const SceneLayer &layer) {
    ClipRanges result;
    result.ranges[0] = {46, 366};
    unsigned normal = layer.clip_enabled & ~layer.clip_inverted;
    unsigned inverted = layer.clip_enabled & layer.clip_inverted;
    if (!layer.clip_inverse) std::swap(normal, inverted);
    for (unsigned plane = 0; plane < 4; ++plane) {
        const auto clip = row.clips[plane];
        if (normal & (1u << plane)) {
            unsigned count = 0;
            for (unsigned i = 0; i < result.count; ++i) {
                const auto range = result.ranges[i];
                if (clip.left <= clip.right && range.right >= clip.left && range.left <= clip.right)
                    result.ranges[count++] = {std::max(range.left, clip.left), std::min(range.right, clip.right)};
            }
            result.count = count;
        } else if ((inverted & (1u << plane)) && clip.left <= clip.right) {
            ClipRanges next;
            next.count = 0;
            for (unsigned i = 0; i < result.count * 2; ++i) {
                SceneClip candidate = i < result.count ? SceneClip{46, clip.left} : SceneClip{clip.right, 366};
                bool keep = true;
                for (unsigned j = 0; j < result.count; ++j) {
                    const auto range = result.ranges[j];
                    // Preserve the observed oracle's inverted-window combining rule.
                    candidate.left = std::max(range.left, candidate.left);
                    candidate.right = std::max(range.left, candidate.right);
                    if (candidate.left >= candidate.right) { keep = false; break; }
                }
                if (keep) next.ranges[next.count++] = candidate;
            }
            result = next;
        }
    }
    return result;
}
struct PixelMix {
    uint16_t source = 0, destination = 0;
    uint8_t source_weight = 0, destination_weight = 8;
    uint8_t source_priority = 0, destination_priority = 0, source_mode = 255;
};
void mix(PixelMix &pixel, const SceneLayer &layer, uint16_t color, bool select,
         const std::array<uint8_t, 4> &blend) {
    if (!color || layer.blend_mode == pixel.source_mode) return;
    if (layer.priority > pixel.source_priority) {
        unsigned slot = unsigned(select);
        if (layer.blend_mode == 1 || layer.blend_mode == 2) {
            if (layer.blend_mode == 1) slot += 2;
            if (!blend[slot]) return;
            pixel.source_weight = blend[slot];
        } else {
            if (!blend[slot] && !blend[slot + 2]) return;
            pixel.source_weight = blend[slot + 2];
            pixel.destination_weight = blend[slot];
            pixel.destination_priority = layer.priority;
            pixel.destination = color;
        }
        pixel.source = color;
        pixel.source_mode = layer.blend_mode;
        pixel.source_priority = layer.priority;
    } else if (layer.priority >= pixel.destination_priority) {
        pixel.destination = layer.priority == pixel.destination_priority ? 0 : color;
        pixel.destination_priority = layer.priority;
        pixel.destination_weight = blend[unsigned(select) + (pixel.source_mode == 1 ? 0 : 2)];
    }
}
uint32_t rgb(const PixelMix &pixel, const std::array<uint32_t, 8192> &palette) {
    const uint32_t source = palette[pixel.source & 8191];
    const uint32_t destination = palette[pixel.destination & 8191];
    uint32_t color = 0xff000000;
    for (unsigned shift : {0u, 8u, 16u}) {
        const unsigned value = (((source >> shift) & 255) * pixel.source_weight +
                                ((destination >> shift) & 255) * pixel.destination_weight) >> 3;
        color |= std::min(value, 255u) << shift;
    }
    return color;
}
}

void compose_game_scene(const GameTiles &tiles, const GameText &text, const GameLines &lines,
                        std::span<const uint16_t> sprites, bool flipped,
                        std::span<const uint8_t> tile_pixels, std::span<const uint8_t> palette,
                        std::span<uint32_t> output) {
    if (sprites.size() < 432 * 256 || palette.size() < 32768 || output.size() < 320 * 232)
        throw std::runtime_error("Incomplete game scene render buffers");
    std::array<uint32_t, 8192> colors;
    for (unsigned i = 0; i < colors.size(); ++i)
        colors[i] = (uint32_t(palette[i * 4 + 1]) << 16) | (uint32_t(palette[i * 4 + 2]) << 8) | palette[i * 4 + 3];
    for (unsigned y = 24; y < 256; ++y) {
        const auto &row = lines.row(y);
        if (row.bitmap) throw std::runtime_error("Game scene bitmap pivot is unsupported");
        const auto layer = [&row](unsigned index) -> const SceneLayer & {
            return index < 4 ? row.playfields[index].layer : index < 8 ? row.sprites[index - 4] : row.text;
        };
        std::array<unsigned, 9> order{8, 4, 0, 7, 3, 6, 2, 5, 1};
        for (unsigned i = 1; i < order.size(); ++i) {
            const unsigned item = order[i];
            unsigned j = i;
            while (j && layer(order[j - 1]).priority < layer(item).priority) {
                order[j] = order[j - 1];
                --j;
            }
            order[j] = item;
        }
        std::array<PixelMix, 320> pixels;
        for (auto &pixel : pixels) pixel.destination = row.background;
        for (unsigned index : order) {
            const auto &state = layer(index);
            if (!state.enabled) continue;
            const auto ranges = clip_ranges(row, state);
            for (unsigned r = 0; r < ranges.count; ++r) {
                const int left = std::max<int>(46, ranges.ranges[r].left);
                const int right = std::min<int>(366, ranges.ranges[r].right);
                for (int hardware_x = left; hardware_x < right; ++hardware_x) {
                    auto &pixel = pixels[hardware_x - 46];
                    if (state.blend_mode == pixel.source_mode) continue;
                    int sample_x = hardware_x;
                    if (state.mosaic && row.mosaic_period > 1) {
                        int count = hardware_x + 68;
                        if (count >= 432) count -= 432;
                        sample_x -= count % row.mosaic_period;
                    }
                    ScenePixel source;
                    bool select = state.blend_select;
                    if (index < 4) {
                        const auto &pf = row.playfields[index];
                        const int x = (pf.source_x + (sample_x - 46) * pf.x_step) >> 8;
                        source = tiles.playfield_pixel(index, x, pf.source_y, flipped, tile_pixels);
                        select = (source.flags & 1) != 0;
                        if (!(source.flags & 0x10) || !source.palette) continue;
                        source.palette = uint16_t(source.palette + pf.palette_add);
                    } else if (index < 8) {
                        if (sample_x < 0 || sample_x >= 432) continue;
                        source.palette = sprites[y * 432 + sample_x];
                        if (!source.palette || ((source.palette >> 10) & 3) != index - 4) continue;
                    } else {
                        source = text.pixel(row.text_x + sample_x - 46, row.text_y, flipped);
                        if (!(source.flags & 0x10)) continue;
                    }
                    mix(pixel, state, source.palette, select, row.blend);
                }
            }
        }
        for (unsigned x = 0; x < 320; ++x) output[(y - 24) * 320 + x] = rgb(pixels[x], colors);
    }
}
} // namespace f3rt
