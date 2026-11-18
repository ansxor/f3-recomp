#pragma once
#include "renderer/game/scene.hpp"
#include <algorithm>

namespace f3rt {
struct ClipRanges {
    // Four inside intervals leave at most five gaps. With an outside window,
    // at most three inside intervals can join the two edge intervals.
    static constexpr unsigned capacity = 5;
    std::array<SceneClip, capacity> ranges{};
    unsigned count = 0;

    const SceneClip *begin() const { return ranges.data(); }
    const SceneClip *end() const { return ranges.data() + count; }
};

inline ClipRanges clip_ranges(const std::array<SceneClip, 4> &windows, unsigned enabled,
                              unsigned inside, bool invert, int16_t left, int16_t right) {
    if (left >= right) return {};

    ClipRanges parts;
    if (!enabled) {
        if (invert) {
            parts.ranges[0] = {left, right};
            parts.count = 1;
        }
        return parts;
    }
    // The union of all selected outsides is the complement of their common inside.
    SceneClip common_inside{left, right};
    for (unsigned n = 0; n < windows.size(); ++n) {
        if (!(enabled & (1u << n))) continue;
        const SceneClip window{std::max(left, windows[n].left), std::min(right, windows[n].right)};
        if (inside & (1u << n)) {
            if (window.left < window.right) parts.ranges[parts.count++] = window;
        } else {
            common_inside.left = std::max(common_inside.left, window.left);
            common_inside.right = std::min(common_inside.right, window.right);
        }
    }
    if (common_inside.left >= common_inside.right) {
        if (invert) return {};
        parts.ranges[0] = {left, right};
        parts.count = 1;
        return parts;
    }
    if (left < common_inside.left) parts.ranges[parts.count++] = {left, common_inside.left};
    if (common_inside.right < right) parts.ranges[parts.count++] = {common_inside.right, right};

    for (unsigned i = 1; i < parts.count; ++i) {
        const auto part = parts.ranges[i];
        unsigned j = i;
        while (j && parts.ranges[j - 1].left > part.left) {
            parts.ranges[j] = parts.ranges[j - 1];
            --j;
        }
        parts.ranges[j] = part;
    }
    ClipRanges visible;
    for (const auto part : parts) {
        if (visible.count && part.left <= visible.ranges[visible.count - 1].right) {
            auto &last = visible.ranges[visible.count - 1];
            last.right = std::max(last.right, part.right);
        } else {
            visible.ranges[visible.count++] = part;
        }
    }
    if (!invert) return visible;

    ClipRanges gaps;
    for (const auto part : visible) {
        if (left < part.left) gaps.ranges[gaps.count++] = {left, part.left};
        left = part.right;
    }
    if (left < right) gaps.ranges[gaps.count++] = {left, right};
    return gaps;
}

inline ClipRanges clip_ranges(const SceneRow &row, const SceneLayer &layer, int16_t left,
                              int16_t right) {
    return clip_ranges(row.clips, layer.clip_enabled, layer.clip_inverted, layer.clip_inverse,
                       left, right);
}

inline std::array<LayerId, LAYER_COUNT> scene_order(const SceneRow &row) {
    std::array<LayerId, LAYER_COUNT> order{LayerId::Text, LayerId::Sp0, LayerId::Pf0,
                                           LayerId::Sp3,  LayerId::Pf3, LayerId::Sp2,
                                           LayerId::Pf2,  LayerId::Sp1, LayerId::Pf1};
    for (unsigned i = 1; i < order.size(); ++i) {
        const LayerId item = order[i];
        unsigned j = i;
        while (j && row.layer(order[j - 1]).priority < row.layer(item).priority) {
            order[j] = order[j - 1];
            --j;
        }
        order[j] = item;
    }
    return order;
}
} // namespace f3rt
