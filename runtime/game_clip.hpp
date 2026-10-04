#pragma once
#include "game_scene.hpp"
#include <algorithm>

namespace f3rt {
struct ClipRanges {
    std::array<SceneClip, 16> ranges{};
    unsigned count = 1;
};
inline ClipRanges clip_ranges(const SceneRow &row, const SceneLayer &layer, int16_t left, int16_t right) {
    ClipRanges result;
    result.ranges[0] = {left, right};
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
                SceneClip candidate = i < result.count ? SceneClip{left, clip.left} : SceneClip{clip.right, right};
                bool keep = true;
                for (unsigned j = 0; j < result.count; ++j) {
                    const auto range = result.ranges[j];
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
inline const SceneLayer &scene_layer(const SceneRow &row, unsigned index) {
    return index < 4 ? row.playfields[index].layer : index < 8 ? row.sprites[index - 4] : row.text;
}
inline std::array<unsigned, 9> scene_order(const SceneRow &row) {
    std::array<unsigned, 9> order{8, 4, 0, 7, 3, 6, 2, 5, 1};
    for (unsigned i = 1; i < order.size(); ++i) {
        const unsigned item = order[i];
        unsigned j = i;
        while (j && scene_layer(row, order[j - 1]).priority < scene_layer(row, item).priority) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = item;
    }
    return order;
}
} // namespace f3rt
