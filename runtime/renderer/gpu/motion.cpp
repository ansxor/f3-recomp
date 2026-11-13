#include "renderer/gpu/motion.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace f3rt {
namespace {
int64_t floor_divide(int64_t n, int64_t d) { return n >= 0 ? n / d : -1 - (-1 - n) / d; }
bool motion_delta(int32_t before, int32_t now, int period = 0) {
    return std::abs(int64_t(now) - before) <= int64_t(32) * 256 &&
           (!period || floor_divide(before, period) == floor_divide(now, period));
}
int32_t interpolate(int32_t current, int32_t delta, float alpha) {
    return int32_t(std::llround(double(current) - double(delta) * (1.0 - alpha)));
}
bool same_ranges(const ClipRanges &a, const ClipRanges &b) {
    return a.count == b.count &&
           std::equal(a.ranges.begin(), a.ranges.begin() + a.count, b.ranges.begin());
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
           a.y_step == b.y_step && a.y_fraction == b.y_fraction && a.palette_add == b.palette_add &&
           a.alt_map == b.alt_map;
}
bool valid_sprite(const SceneSprite &v) {
    return v.scale_x > 0 && v.scale_x <= 256 && v.scale_y > 0 && v.scale_y <= 256 &&
           v.tile <
               (1u
                << 17) && // 17-bit sprite code (MAME y-ack/fdp-collapse tc0630fdp.cpp); the ROM wraps it
           std::abs(int64_t(v.x)) <= int64_t(1024) * 256 &&
           std::abs(int64_t(v.y)) <= int64_t(1024) * 256;
}
// Scale and flip changes snap: the group's member offsets are only rigid while the
// whole-object transform is unchanged.
bool same_transform(const SceneSprite &a, const SceneSprite &b) {
    return a.scale_x == b.scale_x && a.scale_y == b.scale_y && a.flip_x == b.flip_x &&
           a.flip_y == b.flip_y;
}
// Rigid sprite groups (HANDOFF.md §3, TLC results §4.2): a sprite carrying a group key pairs by
// that key alone, and the group's origin delta is the delta of every current member. Members may
// appear, vanish or change tile between frames (LoD, flicker, skipped tiles), so no per-sprite
// correspondence is needed or assumed. A key that is absent from either frame, or that occurs with
// more than one origin in a frame, never pairs and snaps.
inline constexpr unsigned NO_SPRITE = ~0u;
// One frame's group keys: sprite indices sorted by key, plus per-run flags. A run is the maximal
// contiguous range of equal keys; its flags are stored at the run's first position and describe
// every member of the run. Fixed arrays: capture() never allocates.
struct GroupTable {
    std::array<unsigned, CapturedFrame::MAX_SPRITES> order{};
    std::array<bool, CapturedFrame::MAX_SPRITES> origin_unique{};
    std::array<bool, CapturedFrame::MAX_SPRITES> transform_unique{};
    unsigned count = 0;
};
uint64_t transform_signature(const SceneSprite &s) {
    return (uint64_t(s.scale_x) << 32) | (uint64_t(s.scale_y) << 16) | (uint64_t(s.flip_x) << 1) |
           uint64_t(s.flip_y);
}
// `at(i)` yields the SceneSprite of sprite slot i of the table's frame.
template <class At> void build_group_table(unsigned count, const At &at, GroupTable &table) {
    table.count = 0;
    for (unsigned i = 0; i < count; ++i)
        if (at(i).group) table.order[table.count++] = i;
    std::sort(table.order.begin(), table.order.begin() + table.count,
              [&at](unsigned a, unsigned b) { return at(a).group < at(b).group; });
    for (unsigned p = 0; p < table.count;) {
        const SceneSprite &first = at(table.order[p]);
        bool same_origin = true, same_signature = true;
        unsigned e = p + 1;
        for (; e < table.count && at(table.order[e]).group == first.group; ++e) {
            const SceneSprite &s = at(table.order[e]);
            same_origin = same_origin && s.group_x == first.group_x && s.group_y == first.group_y;
            same_signature =
                same_signature && transform_signature(s) == transform_signature(first);
        }
        table.origin_unique[p] = same_origin;
        table.transform_unique[p] = same_signature;
        p = e;
    }
}
// Position of the key's run start, or NO_SPRITE when the key does not occur.
template <class At> unsigned find_group(const GroupTable &table, const At &at, uint64_t key) {
    unsigned lo = 0, hi = table.count;
    while (lo < hi) {
        const unsigned mid = lo + (hi - lo) / 2;
        if (at(table.order[mid]).group < key) lo = mid + 1;
        else hi = mid;
    }
    if (lo < table.count && at(table.order[lo]).group == key) return lo;
    return NO_SPRITE;
}
// The previous frame's group origin is the previous-equivalent geometry of every current member;
// it must be a sane 24.8 scanout position for the interpolation to mean anything.
bool valid_origin(const SceneSprite &s) {
    return std::abs(int64_t(s.group_x)) <= int64_t(1024) * 256 &&
           std::abs(int64_t(s.group_y)) <= int64_t(1024) * 256;
}
// Keyless sprites fall back to appearance: same tile and palette.
uint64_t appearance_key(const SceneSprite &s) {
    return (uint64_t(s.tile) << 32) | s.palette;
}
} // namespace
ClipRanges GpuMotionHistory::ranges(const SceneRow &row, LayerId id) const noexcept {
    return clip_ranges(row, row.layer(id), int16_t(46 - m_border), int16_t(366 + m_border));
}
// Layer control equality: priority/blend/enable/select/mosaic, clip enables and
// polarity, and the resulting visible ranges (not the raw clip planes).
bool GpuMotionHistory::same_controls(const SceneRow &a, const SceneRow &b,
                                     LayerId id) const noexcept {
    if (a.layer(id) != b.layer(id)) return false;
    if (a.clips == b.clips) return true;
    return same_ranges(ranges(a, id), ranges(b, id));
}
void GpuMotionHistory::reset() noexcept {
    m_captured = m_paired = false;
    m_frame = 0;
}
// Pair the current frame's sprites against the previous frame.
// Group sprites: pair by key; delta = group origin delta; eligible while the key is unambiguous on
// both sides, the previous members share one transform signature that the current sprite still
// has, and the origin moved at most 32 px per axis. No group pair -> snap.
// Keyless sprites: appearance fallback only, (tile, palette) groups with mutually unique nearest
// positions within 32 px; their delta is their own position delta.
// Rejection counters separate "no pairing" (sprite_group_rejections) from paired-but-ineligible
// sprites (transform / jump).
void GpuMotionHistory::pair_sprites(const CapturedFrame &scene, int32_t *dx, int32_t *dy,
                                    bool *eligible) noexcept {
    const auto &current = scene.sprites;
    const auto previous = [this](unsigned i) -> const SceneSprite & { return m_sprites[i].value; };
    const auto now = [&current](unsigned i) -> const SceneSprite & { return current[i]; };

    GroupTable old_groups, new_groups;
    build_group_table(m_sprite_count, previous, old_groups);
    build_group_table(scene.sprite_count, now, new_groups);

    // Group pass. Every grouped sprite is evaluated, including members whose slot changed.
    for (unsigned i = 0; i < scene.sprite_count; ++i) {
        const SceneSprite &v = current[i];
        if (!v.group) continue;
        const unsigned old_run = find_group(old_groups, previous, v.group);
        const unsigned new_run = find_group(new_groups, now, v.group);
        const bool paired = old_run != NO_SPRITE && new_run != NO_SPRITE &&
                            old_groups.origin_unique[old_run] && new_groups.origin_unique[new_run];
        if (!paired) {
            // A sprite that is unchanged in its old slot is not evidence of a moving reject.
            if (i < m_sprite_count && v == m_sprites[i].value) continue;
            ++m_pair_stats.rejected_sprites;
            ++m_pair_stats.sprite_group_rejections;
            if (scene.sprite_count != m_sprite_count) ++m_pair_stats.sprite_count_rejections;
            continue;
        }
        const SceneSprite &origin = previous(old_groups.order[old_run]);
        const int32_t gx = v.group_x - origin.group_x;
        const int32_t gy = v.group_y - origin.group_y;
        if (gx || gy) ++m_pair_stats.moving_sprites;
        const bool geometry = m_pen_mask == scene.pen_mask && valid_sprite(v) &&
                              valid_origin(origin) && old_groups.transform_unique[old_run] &&
                              same_transform(v, origin);
        const bool jump = motion_delta(origin.group_x, v.group_x) &&
                          motion_delta(origin.group_y, v.group_y);
        eligible[i] = geometry && jump;
        if (eligible[i]) {
            dx[i] = gx;
            dy[i] = gy;
        } else if (gx || gy) {
            ++m_pair_stats.rejected_sprites;
            if (!geometry) ++m_pair_stats.sprite_transform_rejections;
            else ++m_pair_stats.sprite_jump_rejections;
        }
    }

    // Appearance pass for keyless sprites: (tile, palette) groups with mutually unique nearest
    // positions within 32 px. Tile animation, births and vanishing keyless sprites snap.
    std::array<unsigned, CapturedFrame::MAX_SPRITES> old_order, new_order, matches;
    unsigned old_n = 0, new_n = 0;
    matches.fill(NO_SPRITE);
    for (unsigned i = 0; i < m_sprite_count; ++i)
        if (!previous(i).group) old_order[old_n++] = i;
    for (unsigned i = 0; i < scene.sprite_count; ++i)
        if (!current[i].group) new_order[new_n++] = i;
    std::sort(old_order.begin(), old_order.begin() + old_n, [&](unsigned a, unsigned b) {
        return appearance_key(previous(a)) < appearance_key(previous(b));
    });
    std::sort(new_order.begin(), new_order.begin() + new_n, [&](unsigned a, unsigned b) {
        return appearance_key(current[a]) < appearance_key(current[b]);
    });
    std::array<unsigned, CapturedFrame::MAX_SPRITES> old_nearest, new_nearest, old_distance,
        new_distance;
    unsigned old = 0, fresh = 0;
    while (old < old_n && fresh < new_n) {
        const uint64_t a = appearance_key(previous(old_order[old]));
        const uint64_t b = appearance_key(current[new_order[fresh]]);
        if (a < b) {
            ++old;
            continue;
        }
        if (b < a) {
            ++fresh;
            continue;
        }
        unsigned old_end = old + 1, new_end = fresh + 1;
        while (old_end < old_n && appearance_key(previous(old_order[old_end])) == a) ++old_end;
        while (new_end < new_n && appearance_key(current[new_order[new_end]]) == b) ++new_end;
        if (old_end - old == 1 && new_end - fresh == 1) {
            matches[new_order[fresh]] = old_order[old];
        } else {
            for (unsigned p = old; p < old_end; ++p) {
                old_nearest[p] = NO_SPRITE;
                old_distance[p] = std::numeric_limits<unsigned>::max();
            }
            for (unsigned n = fresh; n < new_end; ++n) {
                new_nearest[n] = NO_SPRITE;
                new_distance[n] = std::numeric_limits<unsigned>::max();
                const auto &v = current[new_order[n]];
                for (unsigned p = old; p < old_end; ++p) {
                    const auto &partner = previous(old_order[p]);
                    const int64_t ddx = int64_t(v.x) - partner.x;
                    const int64_t ddy = int64_t(v.y) - partner.y;
                    if (std::abs(ddx) > int64_t(32) * 256 || std::abs(ddy) > int64_t(32) * 256)
                        continue;
                    const unsigned distance = unsigned(ddx * ddx + ddy * ddy);
                    if (distance < old_distance[p]) {
                        old_distance[p] = distance;
                        old_nearest[p] = n;
                    } else if (distance == old_distance[p]) old_nearest[p] = NO_SPRITE;
                    if (distance < new_distance[n]) {
                        new_distance[n] = distance;
                        new_nearest[n] = p;
                    } else if (distance == new_distance[n]) new_nearest[n] = NO_SPRITE;
                }
            }
            for (unsigned n = fresh; n < new_end; ++n)
                if (new_nearest[n] != NO_SPRITE && old_nearest[new_nearest[n]] == n)
                    matches[new_order[n]] = old_order[new_nearest[n]];
        }
        old = old_end;
        fresh = new_end;
    }

    // Keyless sprites: matched pairs interpolate by their own position delta; unmatched ones snap.
    for (unsigned i = 0; i < scene.sprite_count; ++i) {
        const SceneSprite &v = current[i];
        if (v.group) continue;
        const unsigned match = matches[i];
        if (match == NO_SPRITE) {
            if (i < m_sprite_count && v == m_sprites[i].value) continue;
            ++m_pair_stats.rejected_sprites;
            ++m_pair_stats.sprite_group_rejections;
            if (scene.sprite_count != m_sprite_count) ++m_pair_stats.sprite_count_rejections;
            continue;
        }
        const SceneSprite &partner = previous(match);
        const bool moving = v.x != partner.x || v.y != partner.y;
        if (moving) ++m_pair_stats.moving_sprites;
        const bool geometry =
            m_pen_mask == scene.pen_mask && valid_sprite(v) && valid_sprite(partner) &&
            same_transform(v, partner);
        const bool jump = motion_delta(partner.x, v.x) && motion_delta(partner.y, v.y);
        eligible[i] = geometry && jump;
        if (eligible[i]) {
            dx[i] = v.x - partner.x;
            dy[i] = v.y - partner.y;
        } else if (moving) {
            ++m_pair_stats.rejected_sprites;
            if (!geometry) ++m_pair_stats.sprite_transform_rejections;
            else ++m_pair_stats.sprite_jump_rejections;
        }
    }
}
void GpuMotionHistory::capture(const CapturedFrame &scene, uint64_t frame) noexcept {
    if (scene.fallback || scene.sprite_count > m_sprites.size()) {
        reset();
        return;
    }
    m_paired =
        m_captured && m_frame != std::numeric_limits<uint64_t>::max() && frame == m_frame + 1;
    m_pair_stats = {};
    const auto &current = scene.sprites;
    // Compact render slots are not identities, so the slot-by-slot comparison only detects
    // whether the sprite list changed at all; pairing never uses slots.
    bool sprites_changed = m_sprite_count != scene.sprite_count;
    for (unsigned i = 0; m_paired && !sprites_changed && i < scene.sprite_count; ++i)
        sprites_changed = current[i] != m_sprites[i].value;
    // Compute all deltas before overwriting the previous slots.
    std::array<int32_t, CapturedFrame::MAX_SPRITES> dx{}, dy{};
    std::array<bool, CapturedFrame::MAX_SPRITES> eligible{};
    if (m_paired && sprites_changed) pair_sprites(scene, dx.data(), dy.data(), eligible.data());
    for (unsigned i = 0; i < scene.sprite_count; ++i) {
        auto &s = m_sprites[i];
        s.value = current[i];
        s.dx = dx[i];
        s.dy = dy[i];
        s.eligible = eligible[i];
    }
    for (unsigned y = geometry::FIRST_LINE; y < geometry::END_LINE; ++y) {
        auto &before = m_rows[y - geometry::FIRST_LINE];
        Row now;
        now.scene = scene.rows[y];
        now.order = scene_order(now.scene);
        for (unsigned i = 0; i < 5; ++i)
            now.valid[i] = valid_layer(now.scene.layer(SCROLLED_LAYERS[i]),
                                       ranges(now.scene, SCROLLED_LAYERS[i]));
        for (unsigned i = 0; i < 5; ++i) {
            const LayerId id = SCROLLED_LAYERS[i];
            const bool playfield = kind(id) == LayerKind::Playfield;
            // Blend weights and ordering remain current discrete state. Changes
            // to another layer do not invalidate this layer's source geometry.
            const bool controls =
                m_paired && (playfield || (!before.scene.bitmap && !now.scene.bitmap)) &&
                same_controls(now.scene, before.scene, id) && now.valid[i] && before.valid[i];
            int32_t x = 0, px = 0, phase = 0, previous_phase = 0;
            bool valid_x = true, valid_y = true;
            if (playfield) {
                const auto &g = now.scene.playfields[i];
                const auto &p = before.scene.playfields[i];
                valid_x = std::abs(int64_t(g.source_x)) <= (int64_t{1} << 24) &&
                          std::abs(int64_t(p.source_x)) <= (int64_t{1} << 24) &&
                          uint32_t(g.x_step) > 0 && uint32_t(g.x_step) <= 256 &&
                          g.x_step == p.x_step && g.alt_map == p.alt_map;
                valid_y = uint32_t(g.source_y) <= 511 && uint32_t(p.source_y) <= 511 &&
                          g.y_fraction <= 255 && p.y_fraction <= 255 && uint32_t(g.y_step) > 0 &&
                          uint32_t(g.y_step) <= 510 && g.y_step == p.y_step &&
                          g.alt_map == p.alt_map;
                x = g.source_x;
                px = p.source_x;
                phase = int32_t(uint32_t(g.source_y) * 256u + g.y_fraction);
                previous_phase = int32_t(uint32_t(p.source_y) * 256u + p.y_fraction);
            } else {
                const int32_t tx = now.scene.text_x, ty = now.scene.text_y;
                const int32_t ptx = before.scene.text_x, pty = before.scene.text_y;
                valid_x = uint32_t(tx) <= 511 && uint32_t(ptx) <= 511;
                valid_y = uint32_t(ty) <= 511 && uint32_t(pty) <= 511;
                x = tx * 256;
                px = ptx * 256;
                phase = ty * 256;
                previous_phase = pty * 256;
            }
            const bool jump_x = motion_delta(
                px, x, (playfield ? (game_config::video.extend ? 1024 : 512) : 512) * 256);
            const bool jump_y = motion_delta(previous_phase, phase, 512 * 256);
            now.eligible[size_t(i) * 2] = controls && valid_x && jump_x;
            now.eligible[i * 2 + 1] = controls && valid_y && jump_y;
            if (now.eligible[size_t(i) * 2]) now.delta[size_t(i) * 2] = x - px;
            if (now.eligible[i * 2 + 1]) now.delta[i * 2 + 1] = phase - previous_phase;
            // Disabled channels and unchanged geometry are neither moving nor rejected.
            const bool moving_x = x != px, moving_y = phase != previous_phase;
            if (!m_paired || !now.scene.layer(id).enabled || (!moving_x && !moving_y)) continue;
            if (playfield) ++m_pair_stats.moving_playfield_rows;
            else ++m_pair_stats.moving_text_rows;
            if ((moving_x && !now.eligible[size_t(i) * 2]) ||
                (moving_y && !now.eligible[i * 2 + 1])) {
                ++m_pair_stats.rejected_rows;
                if (!controls) ++m_pair_stats.row_control_rejections;
                else if ((moving_x && !valid_x) || (moving_y && !valid_y))
                    ++m_pair_stats.row_transform_rejections;
                else ++m_pair_stats.row_jump_rejections;
            }
        }
        before = now;
    }
    m_frame = frame;
    m_sprite_count = scene.sprite_count;
    m_pen_mask = scene.pen_mask;
    m_captured = true;
}
MotionResult GpuMotionHistory::apply(const CapturedFrame &scene, float alpha) const noexcept {
    if (!std::isfinite(alpha) || alpha < 0 || alpha > 1 || !m_paired || scene.fallback ||
        scene.sprite_count != m_sprite_count || scene.pen_mask != m_pen_mask)
        return {};
    MotionResult result;
    MotionInterpolationStats &stats = result.stats;
    MotionState &state = result.state;
    stats = m_pair_stats;
    stats.paired = true;
    stats.alpha = alpha;
    for (unsigned i = 0; i < m_sprite_count; ++i) {
        state.sprite_x[i] = scene.sprites[i].x;
        state.sprite_y[i] = scene.sprites[i].y;
    }
    for (unsigned y = 0; y < 256; ++y) {
        auto &out = state.rows[y];
        const auto &row = scene.rows[y];
        for (unsigned i = 0; i < 4; ++i) {
            out.source_x[i] = row.playfields[i].source_x;
            out.phase[i] =
                int32_t(uint32_t(row.playfields[i].source_y) * 256u + row.playfields[i].y_fraction);
        }
        // Every motion-marked text row has current fixed-point defaults, even snapped rows.
        out.text_x = int32_t(row.text_x) * 256;
        out.text_y = int32_t(row.text_y) * 256;
    }
    // dx/dy are the group origin delta, so a member's interpolated position is its current
    // position minus the shared delta. Only current members are drawn.
    for (unsigned i = 0; i < m_sprite_count; ++i) {
        const auto &s = m_sprites[i];
        if (!s.eligible || (!s.dx && !s.dy) || s.value != scene.sprites[i]) continue;
        ++stats.sprites;
        if (alpha == 1) continue;
        state.sprite_x[i] = interpolate(s.value.x, s.dx, alpha);
        state.sprite_y[i] = interpolate(s.value.y, s.dy, alpha);
    }
    for (unsigned y = geometry::FIRST_LINE; y < geometry::END_LINE; ++y) {
        const auto &r = m_rows[y - geometry::FIRST_LINE];
        const auto &row = scene.rows[y];
        auto &out = state.rows[y];
        if (row.bitmap != r.scene.bitmap || row.mosaic_period != r.scene.mosaic_period ||
            row.blend != r.scene.blend || scene_order(row) != r.order)
            continue;
        for (unsigned i = 0; i < 5; ++i) {
            const LayerId id = SCROLLED_LAYERS[i];
            const bool playfield = kind(id) == LayerKind::Playfield;
            if ((!r.eligible[size_t(i) * 2] && !r.eligible[i * 2 + 1]) ||
                (!r.delta[size_t(i) * 2] && !r.delta[i * 2 + 1]) ||
                !same_controls(r.scene, row, id))
                continue;
            if (playfield) {
                if (!same_geometry(r.scene.playfields[i], row.playfields[i])) continue;
            } else if (r.scene.text_x != row.text_x || r.scene.text_y != row.text_y) continue;
            if (playfield) ++stats.playfield_rows;
            else ++stats.text_rows;
            if (alpha == 1) continue;
            if (playfield) {
                const auto &g = r.scene.playfields[i];
                out.source_x[i] = interpolate(g.source_x, r.delta[size_t(i) * 2], alpha);
                out.phase[i] = interpolate(int32_t(uint32_t(g.source_y) * 256u + g.y_fraction),
                                           r.delta[i * 2 + 1], alpha);
            } else {
                out.text_x = interpolate(int32_t(r.scene.text_x) * 256, r.delta[8], alpha);
                out.text_y = interpolate(int32_t(r.scene.text_y) * 256, r.delta[9], alpha);
            }
        }
    }
    return result;
}
} // namespace f3rt
