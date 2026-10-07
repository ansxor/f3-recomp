#include "renderer/gpu/motion.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace f3rt {
namespace {
int64_t floor_divide(int64_t n, int64_t d) { return n >= 0 ? n / d : -1 - (-1 - n) / d; }
bool motion_delta(int32_t before, int32_t now, int period = 0) {
    return std::abs(int64_t(now) - before) <= 32 * 256 &&
        (!period || floor_divide(before, period) == floor_divide(now, period));
}
int32_t interpolate(int32_t current, int32_t delta, float alpha) {
    return int32_t(std::llround(double(current) - double(delta) * (1.0 - alpha)));
}
bool same_ranges(const ClipRanges &a, const ClipRanges &b) {
    return a.count == b.count && std::equal(a.ranges.begin(), a.ranges.begin() + a.count, b.ranges.begin());
}
// A scrolled layer may move only while enabled, unmosaiced and with non-empty,
// bounded visible ranges.
bool valid_layer(const SceneLayer &layer, const ClipRanges &clips) {
    if (!layer.enabled || layer.mosaic || clips.count == 0 || clips.count > 16) return false;
    for (unsigned i = 0; i < clips.count; ++i) {
        const int32_t lo = clips.ranges[i].left, hi = clips.ranges[i].right;
        if (lo >= hi || lo < -4096 || hi > 4096) return false;
    }
    return true;
}
bool same_geometry(const ScenePlayfield &a, const ScenePlayfield &b) {
    return a.source_x == b.source_x && a.source_y == b.source_y && a.x_step == b.x_step &&
        a.y_step == b.y_step && a.y_fraction == b.y_fraction && a.palette_add == b.palette_add;
}
bool valid_sprite(const SceneSprite &v) {
    return v.scale_x > 0 && v.scale_x <= 256 && v.scale_y > 0 && v.scale_y <= 256 &&
        v.tile < 32768 &&
        std::abs(int64_t(v.x)) <= 1024 * 256 && std::abs(int64_t(v.y)) <= 1024 * 256;
}
bool same_transform(const SceneSprite &a, const SceneSprite &b) {
    return a.scale_x == b.scale_x && a.scale_y == b.scale_y && a.tile == b.tile &&
        a.palette == b.palette && a.flip_x == b.flip_x && a.flip_y == b.flip_y;
}
// Appearance identity: sprites are matched by tile and palette.
uint64_t key(const SceneSprite &s) { return (uint64_t(s.tile) << 32) | s.palette; }
} // namespace
ClipRanges GpuMotionHistory::ranges(const SceneRow &row, LayerId id) const noexcept {
    return clip_ranges(row, row.layer(id), int16_t(46 - border_), int16_t(366 + border_));
}
// Layer control equality: priority/blend/enable/select/mosaic, clip enables and
// polarity, and the resulting visible ranges (not the raw clip planes).
bool GpuMotionHistory::same_controls(const SceneRow &a, const SceneRow &b, LayerId id) const noexcept {
    if (a.layer(id) != b.layer(id)) return false;
    if (a.clips == b.clips) return true;
    return same_ranges(ranges(a, id), ranges(b, id));
}
void GpuMotionHistory::reset() noexcept {
    captured_ = paired_ = false;
    frame_ = 0;
}
void GpuMotionHistory::capture(const CapturedFrame &scene, uint64_t frame) noexcept {
    if (scene.fallback || scene.sprite_count > sprites_.size()) { reset(); return; }
    paired_ = captured_ && frame_ != std::numeric_limits<uint64_t>::max() && frame == frame_ + 1;
    pair_stats_ = {};
    // Compact render slots are not identities: tile-zero births/deaths shift them.
    // Match appearance groups with mutually unique nearest positions.
    // Tile animation deliberately snaps; the render snapshot has no object ID.
    std::array<unsigned, 1024> matches;
    const auto &current = scene.sprites;
    bool sprites_changed = sprite_count_ != scene.sprite_count;
    for (unsigned i = 0; paired_ && !sprites_changed && i < scene.sprite_count; ++i)
        sprites_changed = current[i] != sprites_[i].value;
    if (paired_ && sprites_changed) {
        std::array<unsigned, 1024> old_order, new_order;
        matches.fill(1024);
        for (unsigned i = 0; i < sprite_count_; ++i) old_order[i] = i;
        for (unsigned i = 0; i < scene.sprite_count; ++i) new_order[i] = i;
        std::sort(old_order.begin(), old_order.begin() + sprite_count_,
                  [&](unsigned a, unsigned b) { return key(sprites_[a].value) < key(sprites_[b].value); });
        std::sort(new_order.begin(), new_order.begin() + scene.sprite_count,
                  [&](unsigned a, unsigned b) { return key(current[a]) < key(current[b]); });
        unsigned old = 0, now = 0;
        std::array<unsigned, 1024> old_nearest, new_nearest, old_distance, new_distance;
        while (old < sprite_count_ && now < scene.sprite_count) {
            const uint64_t a = key(sprites_[old_order[old]].value), b = key(current[new_order[now]]);
            if (a < b) { ++old; continue; }
            if (b < a) { ++now; continue; }
            unsigned old_end = old + 1, now_end = now + 1;
            while (old_end < sprite_count_ && key(sprites_[old_order[old_end]].value) == a) ++old_end;
            while (now_end < scene.sprite_count && key(current[new_order[now_end]]) == b) ++now_end;
            if (old_end - old == 1 && now_end - now == 1) {
                matches[new_order[now]] = old_order[old];
            } else {
                for (unsigned p = old; p < old_end; ++p) {
                    old_nearest[p] = 1024; old_distance[p] = std::numeric_limits<unsigned>::max();
                }
                for (unsigned n = now; n < now_end; ++n) {
                    new_nearest[n] = 1024; new_distance[n] = std::numeric_limits<unsigned>::max();
                    const auto &v = current[new_order[n]];
                    for (unsigned p = old; p < old_end; ++p) {
                        const auto &previous = sprites_[old_order[p]].value;
                        const int64_t dx = int64_t(v.x) - previous.x;
                        const int64_t dy = int64_t(v.y) - previous.y;
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
    for (unsigned i = 0; paired_ && sprites_changed && i < scene.sprite_count; ++i) {
        const auto &v = current[i];
        const unsigned match = matches[i];
        if (match == 1024) {
            // Identical stationary duplicates are not evidence of moving rejects.
            if (i < sprite_count_ && v == sprites_[i].value) continue;
            ++pair_stats_.rejected_sprites;
            ++pair_stats_.sprite_identity_rejections;
            if (scene.sprite_count != sprite_count_) ++pair_stats_.sprite_count_rejections;
            continue;
        }
        const auto &previous = sprites_[match].value;
        const bool moving = v.x != previous.x || v.y != previous.y;
        if (moving) ++pair_stats_.moving_sprites;
        const bool transform = pen_mask_ == scene.pen_mask && valid_sprite(v) && valid_sprite(previous) &&
            same_transform(v, previous);
        const bool jump = motion_delta(previous.x, v.x) && motion_delta(previous.y, v.y);
        eligible[i] = transform && jump;
        if (eligible[i]) {
            dx[i] = v.x - previous.x;
            dy[i] = v.y - previous.y;
        } else if (moving) {
            ++pair_stats_.rejected_sprites;
            if (!transform) ++pair_stats_.sprite_transform_rejections;
            else ++pair_stats_.sprite_jump_rejections;
        }
    }
    for (unsigned i = 0; i < scene.sprite_count; ++i) {
        auto &s = sprites_[i];
        s.value = current[i];
        s.dx = dx[i]; s.dy = dy[i]; s.eligible = eligible[i];
    }
    for (unsigned y = 24; y < 256; ++y) {
        auto &before = rows_[y - 24];
        Row now;
        now.scene = scene.rows[y];
        now.order = scene_order(now.scene);
        for (unsigned i = 0; i < 5; ++i)
            now.valid[i] = valid_layer(now.scene.layer(scrolled_layers[i]), ranges(now.scene, scrolled_layers[i]));
        for (unsigned i = 0; i < 5; ++i) {
            const LayerId id = scrolled_layers[i];
            const bool playfield = kind(id) == LayerKind::Playfield;
            // Blend weights and ordering remain current discrete state. Changes
            // to another layer do not invalidate this layer's source geometry.
            const bool controls = paired_ && (playfield || (!before.scene.bitmap && !now.scene.bitmap)) &&
                same_controls(now.scene, before.scene, id) && now.valid[i] && before.valid[i];
            int32_t x = 0, px = 0, phase = 0, previous_phase = 0;
            bool valid_x = true, valid_y = true;
            if (playfield) {
                const auto &g = now.scene.playfields[i]; const auto &p = before.scene.playfields[i];
                valid_x = std::abs(int64_t(g.source_x)) <= (int64_t{1} << 24) &&
                    std::abs(int64_t(p.source_x)) <= (int64_t{1} << 24) &&
                    uint32_t(g.x_step) > 0 && uint32_t(g.x_step) <= 256 && g.x_step == p.x_step;
                valid_y = uint32_t(g.source_y) <= 511 && uint32_t(p.source_y) <= 511 &&
                    g.y_fraction <= 255 && p.y_fraction <= 255 &&
                    uint32_t(g.y_step) > 0 && uint32_t(g.y_step) <= 510 && g.y_step == p.y_step;
                x = g.source_x; px = p.source_x;
                phase = int32_t(uint32_t(g.source_y) * 256u + g.y_fraction);
                previous_phase = int32_t(uint32_t(p.source_y) * 256u + p.y_fraction);
            } else {
                const int32_t tx = now.scene.text_x, ty = now.scene.text_y;
                const int32_t ptx = before.scene.text_x, pty = before.scene.text_y;
                valid_x = uint32_t(tx) <= 511 && uint32_t(ptx) <= 511;
                valid_y = uint32_t(ty) <= 511 && uint32_t(pty) <= 511;
                x = tx * 256; px = ptx * 256;
                phase = ty * 256; previous_phase = pty * 256;
            }
            const bool jump_x = motion_delta(px, x, (playfield ? 1024 : 512) * 256);
            const bool jump_y = motion_delta(previous_phase, phase, 512 * 256);
            now.eligible[i * 2] = controls && valid_x && jump_x;
            now.eligible[i * 2 + 1] = controls && valid_y && jump_y;
            if (now.eligible[i * 2]) now.delta[i * 2] = x - px;
            if (now.eligible[i * 2 + 1]) now.delta[i * 2 + 1] = phase - previous_phase;
            // Disabled channels and unchanged geometry are neither moving nor rejected.
            const bool moving_x = x != px, moving_y = phase != previous_phase;
            if (!paired_ || !now.scene.layer(id).enabled || (!moving_x && !moving_y)) continue;
            if (playfield) ++pair_stats_.moving_playfield_rows; else ++pair_stats_.moving_text_rows;
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
MotionResult GpuMotionHistory::apply(const CapturedFrame &scene, float alpha) const noexcept {
    if (!std::isfinite(alpha) || alpha < 0 || alpha > 1 || !paired_ || scene.fallback ||
        scene.sprite_count != sprite_count_ || scene.pen_mask != pen_mask_) return {};
    MotionResult result;
    MotionInterpolationStats &stats = result.stats;
    MotionState &state = result.state;
    stats = pair_stats_;
    stats.paired = true; stats.alpha = alpha;
    for (unsigned i = 0; i < sprite_count_; ++i) {
        state.sprite_x[i] = scene.sprites[i].x;
        state.sprite_y[i] = scene.sprites[i].y;
    }
    for (unsigned y = 0; y < 256; ++y) {
        auto &out = state.rows[y];
        const auto &row = scene.rows[y];
        for (unsigned i = 0; i < 4; ++i) {
            out.source_x[i] = row.playfields[i].source_x;
            out.phase[i] = int32_t(uint32_t(row.playfields[i].source_y) * 256u + row.playfields[i].y_fraction);
        }
        // Every motion-marked text row has current fixed-point defaults, even snapped rows.
        out.text_x = int32_t(row.text_x) * 256;
        out.text_y = int32_t(row.text_y) * 256;
    }
    for (unsigned i = 0; i < sprite_count_; ++i) {
        const auto &s = sprites_[i];
        if (!s.eligible || (!s.dx && !s.dy) || s.value != scene.sprites[i]) continue;
        ++stats.sprites;
        if (alpha == 1) continue;
        state.sprite_x[i] = interpolate(s.value.x, s.dx, alpha);
        state.sprite_y[i] = interpolate(s.value.y, s.dy, alpha);
    }
    for (unsigned y = 24; y < 256; ++y) {
        const auto &r = rows_[y - 24];
        const auto &row = scene.rows[y];
        auto &out = state.rows[y];
        if (row.bitmap != r.scene.bitmap || row.mosaic_period != r.scene.mosaic_period ||
            row.blend != r.scene.blend || scene_order(row) != r.order) continue;
        for (unsigned i = 0; i < 5; ++i) {
            const LayerId id = scrolled_layers[i];
            const bool playfield = kind(id) == LayerKind::Playfield;
            if ((!r.eligible[i * 2] && !r.eligible[i * 2 + 1]) || (!r.delta[i * 2] && !r.delta[i * 2 + 1]) ||
                !same_controls(r.scene, row, id)) continue;
            if (playfield) {
                if (!same_geometry(r.scene.playfields[i], row.playfields[i])) continue;
            } else if (r.scene.text_x != row.text_x || r.scene.text_y != row.text_y) continue;
            if (playfield) ++stats.playfield_rows; else ++stats.text_rows;
            if (alpha == 1) continue;
            if (playfield) {
                const auto &g = r.scene.playfields[i];
                out.source_x[i] = interpolate(g.source_x, r.delta[i * 2], alpha);
                out.phase[i] = interpolate(int32_t(uint32_t(g.source_y) * 256u + g.y_fraction), r.delta[i * 2 + 1], alpha);
            } else {
                out.text_x = interpolate(int32_t(r.scene.text_x) * 256, r.delta[8], alpha);
                out.text_y = interpolate(int32_t(r.scene.text_y) * 256, r.delta[9], alpha);
            }
        }
    }
    return result;
}
} // namespace f3rt
