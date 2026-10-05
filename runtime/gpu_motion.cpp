#include "gpu_motion.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace f3rt {
namespace {
constexpr unsigned row_at(unsigned y) { return GpuScene::rows + y * GpuScene::row_stride; }
int64_t floor_divide(int64_t n, int64_t d) { return n >= 0 ? n / d : -1 - (-1 - n) / d; }
bool motion_delta(int32_t before, int32_t now, int period = 0) {
    return std::abs(int64_t(now) - before) <= 32 * 256 &&
        (!period || floor_divide(before, period) == floor_divide(now, period));
}
uint32_t interpolate(uint32_t current, int32_t delta, float alpha) {
    return uint32_t(int32_t(std::llround(double(int32_t(current)) - double(delta) * (1.0 - alpha))));
}
bool valid_layer(const std::array<uint32_t, 35> &layer) {
    if (!(layer[0] & 64u) || (layer[0] & ~255u) || layer[1] == 0 || layer[1] > 16) return false;
    for (unsigned i = 0; i < layer[1]; ++i) {
        const int32_t lo = int32_t(layer[2 + i * 2]), hi = int32_t(layer[3 + i * 2]);
        if (lo >= hi || lo < -4096 || hi > 4096) return false;
    }
    return true;
}
}
GpuMotionHistory::Row GpuMotionHistory::read_row(const GpuScene &scene, unsigned y) noexcept {
    Row result;
    const auto &w = scene.words;
    const unsigned at = row_at(y);
    result.controls[0] = w[at + 1];
    std::copy_n(w.begin() + at + 4, 10, result.controls.begin() + 1);
    for (unsigned i = 0; i < 5; ++i) {
        const unsigned index = i == 4 ? 8 : i;
        const unsigned la = at + GpuScene::row_layers + index * GpuScene::layer_stride;
        auto &layer = result.layers[i];
        layer[0] = w[la]; layer[1] = w[la + 1];
        std::copy_n(w.begin() + la + 2, std::min(32u, layer[1] * 2), layer.begin() + 2);
        const auto &reference = i == 4 ? scene.reference_rows[y].text : scene.reference_rows[y].playfields[i].layer;
        layer[34] = reference.clip_enabled | (uint32_t(reference.clip_inverted) << 8) |
            (uint32_t(reference.clip_inverse) << 16);
    }
    std::copy_n(w.begin() + at + GpuScene::row_pf, 24, result.geometry.begin());
    result.geometry[24] = w[at + 2]; result.geometry[25] = w[at + 3];
    result.bitmap = scene.reference_rows[y].bitmap;
    return result;
}
void GpuMotionHistory::reset() noexcept {
    captured_ = paired_ = false;
    frame_ = 0;
}
void GpuMotionHistory::capture(const GpuScene &scene, uint64_t frame) noexcept {
    if (scene.fallback || scene.sprite_count > sprites_.size()) { reset(); return; }
    paired_ = captured_ && frame_ != std::numeric_limits<uint64_t>::max() && frame == frame_ + 1;
    pair_stats_ = {};
    // Compact render slots are not identities: tile-zero births/deaths shift them.
    // Match appearance groups with mutually unique nearest positions.
    // Tile animation deliberately snaps; the render snapshot has no object ID.
    std::array<unsigned, 1024> matches;
    const auto current = [&](unsigned i) {
        return scene.words.data() + GpuScene::sprites + i * GpuScene::sprite_stride;
    };
    const auto key = [](const auto &v) { return (uint64_t(v[4]) << 32) | v[5]; };
    bool sprites_changed = sprite_count_ != scene.sprite_count;
    for (unsigned i = 0; paired_ && !sprites_changed && i < scene.sprite_count; ++i)
        sprites_changed = !std::equal(current(i), current(i) + 7, sprites_[i].words.begin());
    if (paired_ && sprites_changed) {
        std::array<unsigned, 1024> old_order, new_order;
        matches.fill(1024);
        for (unsigned i = 0; i < sprite_count_; ++i) old_order[i] = i;
        for (unsigned i = 0; i < scene.sprite_count; ++i) new_order[i] = i;
        std::sort(old_order.begin(), old_order.begin() + sprite_count_,
                  [&](unsigned a, unsigned b) { return key(sprites_[a].words) < key(sprites_[b].words); });
        std::sort(new_order.begin(), new_order.begin() + scene.sprite_count,
                  [&](unsigned a, unsigned b) { return key(current(a)) < key(current(b)); });
        unsigned old = 0, now = 0;
        std::array<unsigned, 1024> old_nearest, new_nearest, old_distance, new_distance;
        while (old < sprite_count_ && now < scene.sprite_count) {
            const uint64_t a = key(sprites_[old_order[old]].words), b = key(current(new_order[now]));
            if (a < b) { ++old; continue; }
            if (b < a) { ++now; continue; }
            unsigned old_end = old + 1, now_end = now + 1;
            while (old_end < sprite_count_ && key(sprites_[old_order[old_end]].words) == a) ++old_end;
            while (now_end < scene.sprite_count && key(current(new_order[now_end])) == b) ++now_end;
            if (old_end - old == 1 && now_end - now == 1) {
                matches[new_order[now]] = old_order[old];
            } else {
                for (unsigned p = old; p < old_end; ++p) {
                    old_nearest[p] = 1024; old_distance[p] = std::numeric_limits<unsigned>::max();
                }
                for (unsigned n = now; n < now_end; ++n) {
                    new_nearest[n] = 1024; new_distance[n] = std::numeric_limits<unsigned>::max();
                    const auto v = current(new_order[n]);
                    for (unsigned p = old; p < old_end; ++p) {
                        const auto &previous = sprites_[old_order[p]].words;
                        const int64_t dx = int64_t(int32_t(v[0])) - int32_t(previous[0]);
                        const int64_t dy = int64_t(int32_t(v[1])) - int32_t(previous[1]);
                        if (std::abs(dx) > 32 * 256 || std::abs(dy) > 32 * 256) continue;
                        const unsigned distance = unsigned(dx * dx + dy * dy);
                        if (distance < old_distance[p]) {
                            old_distance[p] = distance; old_nearest[p] = n;
                        } else if (distance == old_distance[p]) old_nearest[p] = 1024;
                        if (distance < new_distance[n]) {
                            new_distance[n] = distance; new_nearest[n] = p;
                        } else if (distance == new_distance[n]) new_nearest[n] = 1024;
                    }
                }
                for (unsigned n = now; n < now_end; ++n)
                    if (new_nearest[n] != 1024 && old_nearest[new_nearest[n]] == n)
                        matches[new_order[n]] = old_order[new_nearest[n]];
            }
            old = old_end; now = now_end;
        }
    }
    // Compute all deltas before overwriting old compact slots.
    std::array<int32_t, 1024> dx{}, dy{};
    std::array<bool, 1024> eligible{};
    const auto valid_sprite = [](const auto &v) {
        return v[2] > 0 && v[2] <= 256 && v[3] > 0 && v[3] <= 256 &&
            v[4] < 32768 && v[5] <= 255 && v[6] <= 3 &&
            std::abs(int64_t(int32_t(v[0]))) <= 1024 * 256 &&
            std::abs(int64_t(int32_t(v[1]))) <= 1024 * 256;
    };
    for (unsigned i = 0; paired_ && sprites_changed && i < scene.sprite_count; ++i) {
        const auto v = current(i);
        const unsigned match = matches[i];
        if (match == 1024) {
            // Identical stationary duplicates are not evidence of moving rejects.
            if (i < sprite_count_ && std::equal(v, v + 7, sprites_[i].words.begin())) continue;
            ++pair_stats_.rejected_sprites;
            ++pair_stats_.sprite_identity_rejections;
            if (scene.sprite_count != sprite_count_) ++pair_stats_.sprite_count_rejections;
            continue;
        }
        const auto &previous = sprites_[match].words;
        const bool moving = v[0] != previous[0] || v[1] != previous[1];
        if (moving) ++pair_stats_.moving_sprites;
        const bool transform = pen_mask_ == scene.pen_mask && valid_sprite(v) && valid_sprite(previous) &&
            std::equal(v + 2, v + 7, previous.begin() + 2);
        const bool jump = motion_delta(int32_t(previous[0]), int32_t(v[0])) &&
            motion_delta(int32_t(previous[1]), int32_t(v[1]));
        eligible[i] = transform && jump;
        if (eligible[i]) {
            dx[i] = int32_t(v[0]) - int32_t(previous[0]);
            dy[i] = int32_t(v[1]) - int32_t(previous[1]);
        } else if (moving) {
            ++pair_stats_.rejected_sprites;
            if (!transform) ++pair_stats_.sprite_transform_rejections;
            else ++pair_stats_.sprite_jump_rejections;
        }
    }
    for (unsigned i = 0; i < scene.sprite_count; ++i) {
        auto &s = sprites_[i];
        std::copy_n(current(i), 7, s.words.begin());
        s.dx = dx[i]; s.dy = dy[i]; s.eligible = eligible[i];
    }
    for (unsigned y = 24; y < 256; ++y) {
        auto &before = rows_[y - 24];
        Row now = read_row(scene, y);
        for (unsigned i = 0; i < 5; ++i) {
            // Blend weights and ordering remain current discrete state. Changes
            // to another layer do not invalidate this layer's source geometry.
            const bool controls = paired_ && (i < 4 || (!before.bitmap && !now.bitmap)) &&
                now.layers[i] == before.layers[i] &&
                valid_layer(now.layers[i]) && valid_layer(before.layers[i]);
            int32_t x = 0, px = 0, phase = 0, previous_phase = 0;
            bool valid_x = true, valid_y = true;
            if (i < 4) {
                const unsigned at = i * 6;
                const auto &g = now.geometry; const auto &p = before.geometry;
                valid_x = std::abs(int64_t(int32_t(g[at]))) <= (int64_t{1} << 24) &&
                    std::abs(int64_t(int32_t(p[at]))) <= (int64_t{1} << 24) &&
                    g[at + 2] > 0 && g[at + 2] <= 256 && g[at + 2] == p[at + 2];
                valid_y = g[at + 1] <= 511 && p[at + 1] <= 511 &&
                    g[at + 4] <= 255 && p[at + 4] <= 255 &&
                    g[at + 3] > 0 && g[at + 3] <= 510 && g[at + 3] == p[at + 3];
                x = int32_t(g[at]); px = int32_t(p[at]);
                phase = int32_t(g[at + 1] * 256 + g[at + 4]);
                previous_phase = int32_t(p[at + 1] * 256 + p[at + 4]);
            } else {
                valid_x = now.geometry[24] <= 511 && before.geometry[24] <= 511;
                valid_y = now.geometry[25] <= 511 && before.geometry[25] <= 511;
                x = int32_t(now.geometry[24] * 256); px = int32_t(before.geometry[24] * 256);
                phase = int32_t(now.geometry[25] * 256); previous_phase = int32_t(before.geometry[25] * 256);
            }
            const bool jump_x = motion_delta(px, x, (i < 4 ? 1024 : 512) * 256);
            const bool jump_y = motion_delta(previous_phase, phase, 512 * 256);
            now.eligible[i * 2] = controls && valid_x && jump_x;
            now.eligible[i * 2 + 1] = controls && valid_y && jump_y;
            if (now.eligible[i * 2]) now.delta[i * 2] = x - px;
            if (now.eligible[i * 2 + 1]) now.delta[i * 2 + 1] = phase - previous_phase;
            // Disabled channels and unchanged geometry are neither moving nor rejected.
            const bool moving_x = x != px, moving_y = phase != previous_phase;
            if (!paired_ || !(now.layers[i][0] & 64u) || (!moving_x && !moving_y)) continue;
            if (i < 4) ++pair_stats_.moving_playfield_rows; else ++pair_stats_.moving_text_rows;
            if ((moving_x && !now.eligible[i * 2]) || (moving_y && !now.eligible[i * 2 + 1])) {
                ++pair_stats_.rejected_rows;
                if (!controls) ++pair_stats_.row_control_rejections;
                else if ((moving_x && !valid_x) || (moving_y && !valid_y)) ++pair_stats_.row_transform_rejections;
                else ++pair_stats_.row_jump_rejections;
            }
        }
        before = now;
    }
    frame_ = frame; sprite_count_ = scene.sprite_count; pen_mask_ = scene.pen_mask;
    captured_ = true;
}
MotionInterpolationStats GpuMotionHistory::apply(const GpuScene &scene, float alpha,
                                                 std::span<uint32_t> words) const noexcept {
    MotionInterpolationStats stats = pair_stats_;
    if (!std::isfinite(alpha) || alpha < 0 || alpha > 1 || !paired_ || scene.fallback ||
        words.size() < GpuScene::word_count || scene.sprite_count != sprite_count_ || scene.pen_mask != pen_mask_) return {};
    stats.paired = true; stats.alpha = alpha;
    for (unsigned i = 0; i < sprite_count_; ++i) {
        const auto &s = sprites_[i];
        const unsigned at = GpuScene::sprites + i * GpuScene::sprite_stride;
        if (!s.eligible || (!s.dx && !s.dy) ||
            !std::equal(s.words.begin(), s.words.end(), scene.words.begin() + at)) continue;
        ++stats.sprites;
        if (alpha == 1) continue;
        words[at] = interpolate(s.words[0], s.dx, alpha);
        words[at + 1] = interpolate(s.words[1], s.dy, alpha);
    }
    for (unsigned y = 24; y < 256; ++y) {
        const auto &r = rows_[y - 24];
        const unsigned ra = row_at(y);
        // Every motion-marked text row has current fixed-point defaults, even snapped rows.
        words[ra + 14] = scene.words[ra + 2] * 256;
        words[ra + 15] = scene.words[ra + 3] * 256;
        if (scene.reference_rows[y].bitmap != r.bitmap ||
            scene.words[ra + 1] != r.controls[0] ||
            !std::equal(r.controls.begin() + 1, r.controls.end(), scene.words.begin() + ra + 4)) continue;
        for (unsigned i = 0; i < 5; ++i) {
            const unsigned index = i == 4 ? 8 : i;
            const unsigned la = ra + GpuScene::row_layers + index * GpuScene::layer_stride;
            const auto &layer = r.layers[i];
            const auto &reference = i == 4 ? scene.reference_rows[y].text : scene.reference_rows[y].playfields[i].layer;
            const uint32_t clip = reference.clip_enabled | (uint32_t(reference.clip_inverted) << 8) |
                (uint32_t(reference.clip_inverse) << 16);
            if ((!r.eligible[i * 2] && !r.eligible[i * 2 + 1]) || (!r.delta[i * 2] && !r.delta[i * 2 + 1]) ||
                clip != layer[34] || scene.words[la] != layer[0] || scene.words[la + 1] != layer[1] ||
                !std::equal(layer.begin() + 2, layer.begin() + 2 + layer[1] * 2, scene.words.begin() + la + 2)) continue;
            const unsigned geometry_at = i == 4 ? ra + 2 : ra + GpuScene::row_pf + i * 6;
            const unsigned stored_at = i == 4 ? 24 : i * 6;
            if (!std::equal(r.geometry.begin() + stored_at, r.geometry.begin() + stored_at + (i == 4 ? 2 : 6),
                            scene.words.begin() + geometry_at)) continue;
            if (i < 4) ++stats.playfield_rows; else ++stats.text_rows;
            if (alpha == 1) continue;
            if (i < 4) {
                const unsigned at = ra + GpuScene::row_pf + i * 6;
                words[at] = interpolate(r.geometry[i * 6], r.delta[i * 2], alpha);
                const uint32_t phase = interpolate(r.geometry[i * 6 + 1] * 256 + r.geometry[i * 6 + 4], r.delta[i * 2 + 1], alpha);
                words[at + 1] = phase >> 8; words[at + 4] = phase & 255;
            } else {
                words[ra + 14] = interpolate(r.geometry[24] * 256, r.delta[8], alpha);
                words[ra + 15] = interpolate(r.geometry[25] * 256, r.delta[9], alpha);
            }
        }
    }
    return stats;
}
} // namespace f3rt
