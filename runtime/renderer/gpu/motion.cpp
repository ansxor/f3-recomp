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
// Scale and flip changes reject even identity matches.
bool same_transform(const SceneSprite &a, const SceneSprite &b) {
    return a.scale_x == b.scale_x && a.scale_y == b.scale_y && a.flip_x == b.flip_x &&
           a.flip_y == b.flip_y;
}
bool same_appearance(const SceneSprite &a, const SceneSprite &b) {
    return a.tile == b.tile && a.palette == b.palette;
}
// Appearance identity: sprites are matched by tile and palette. Bit 63 separates identity-bearing
// sprites (births/vanished fallbacks) from identity-less ones; tile is 17 bits, so it is free.
uint64_t key(const SceneSprite &s) {
    return (uint64_t(s.identity != 0) << 63) | (uint64_t(s.tile) << 32) | s.palette;
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
void GpuMotionHistory::capture(const CapturedFrame &scene, uint64_t frame) noexcept {
    if (scene.fallback || scene.sprite_count > m_sprites.size()) {
        reset();
        return;
    }
    m_paired =
        m_captured && m_frame != std::numeric_limits<uint64_t>::max() && frame == m_frame + 1;
    m_pair_stats = {};
    // Compact render slots are not identities: tile-zero births/deaths shift them.
    // Sprites carrying a nonzero identity (stable per-entry id within an object/invocation) match
    // exactly by it. A same-tile identity match is final. An identity match whose tile/palette
    // changed is demoted below appearance matching: both sprites join the appearance pass (with
    // births and vanished sprites), because entry k may now be a neighbouring cell of a zoomed
    // grid while the sprite's static twin (same tile at the same spot) is still present. After the
    // appearance pass, a demoted current sprite still unmatched whose previous partner is also
    // still unmatched gets its identity pairing restored (by_identity); it interpolates only when
    // its delta agrees with its object's reference delta (see the rigid check below). An identity
    // value occurring on both sides but duplicated is ambiguous: unmatched, no fallback. An
    // identity value absent from the other frame entirely (a birth in the current frame, a
    // vanished sprite in the previous) may be the same sprite re-keyed (some games change the
    // identity every other frame while the sprite stays on screen), so births and vanished
    // sprites enter the appearance pass, in groups separate from identity-less sprites (key()
    // bit 63): births pair only with vanished identity sprites. The appearance pass matches
    // (tile, palette) groups with mutually unique nearest positions within 32 px; tile
    // animation deliberately snaps there. Those fallback matches are not identity matches
    // (by_identity), so they also go through the per-object rigid check below.
    std::array<unsigned, CapturedFrame::MAX_SPRITES> matches;
    std::array<bool, CapturedFrame::MAX_SPRITES> by_identity{};
    const auto &current = scene.sprites;
    bool sprites_changed = m_sprite_count != scene.sprite_count;
    for (unsigned i = 0; m_paired && !sprites_changed && i < scene.sprite_count; ++i)
        sprites_changed = current[i] != m_sprites[i].value;
    if (m_paired && sprites_changed) {
        std::array<unsigned, CapturedFrame::MAX_SPRITES> old_order, new_order, old_ids, new_ids;
        unsigned old_n = 0, new_n = 0, old_id_n = 0, new_id_n = 0;
        matches.fill(CapturedFrame::MAX_SPRITES);
        std::array<unsigned, CapturedFrame::MAX_SPRITES> demoted;
        demoted.fill(CapturedFrame::MAX_SPRITES);
        for (unsigned i = 0; i < m_sprite_count; ++i) {
            if (m_sprites[i].value.identity) old_ids[old_id_n++] = i;
            else old_order[old_n++] = i;
        }
        for (unsigned i = 0; i < scene.sprite_count; ++i) {
            if (current[i].identity) new_ids[new_id_n++] = i;
            else new_order[new_n++] = i;
        }
        // Identity pass: exact, one-to-one; duplicated identities on either side are ambiguous and unmatched.
        std::sort(old_ids.begin(), old_ids.begin() + old_id_n, [&](unsigned a, unsigned b) {
            return m_sprites[a].value.identity < m_sprites[b].value.identity;
        });
        std::sort(new_ids.begin(), new_ids.begin() + new_id_n, [&](unsigned a, unsigned b) {
            return current[a].identity < current[b].identity;
        });
        // Births (identity absent from the previous frame) and vanished sprites (absent from the
        // current frame) join the appearance pass; ambiguous duplicates do not.
        // (by_identity marks matches made by the identity pass.)
        auto vanished = [&](unsigned k) { old_order[old_n++] = old_ids[k]; };
        auto born = [&](unsigned k) { new_order[new_n++] = new_ids[k]; };
        unsigned old = 0, now = 0;
        while (old < old_id_n && now < new_id_n) {
            const uint64_t a = m_sprites[old_ids[old]].value.identity,
                           b = current[new_ids[now]].identity;
            if (a < b) {
                vanished(old++);
                continue;
            }
            if (b < a) {
                born(now++);
                continue;
            }
            unsigned old_end = old + 1, now_end = now + 1;
            while (old_end < old_id_n && m_sprites[old_ids[old_end]].value.identity == a) ++old_end;
            while (now_end < new_id_n && current[new_ids[now_end]].identity == b) ++now_end;
            if (old_end - old == 1 && now_end - now == 1) {
                const unsigned i = new_ids[now], p = old_ids[old];
                if (same_appearance(current[i], m_sprites[p].value)) {
                    matches[i] = p;
                    by_identity[i] = true;
                } else {
                    // Tile/palette changed: demote to the appearance pass; restored below if unmatched.
                    demoted[i] = p;
                    old_order[old_n++] = p;
                    new_order[new_n++] = i;
                }
            }
            old = old_end;
            now = now_end;
        }
        while (old < old_id_n) vanished(old++);
        while (now < new_id_n) born(now++);
        // Appearance pass over the identity-less remainder plus identity births/vanished.
        std::sort(old_order.begin(), old_order.begin() + old_n, [&](unsigned a, unsigned b) {
            return key(m_sprites[a].value) < key(m_sprites[b].value);
        });
        std::sort(new_order.begin(), new_order.begin() + new_n,
                  [&](unsigned a, unsigned b) { return key(current[a]) < key(current[b]); });
        old = 0;
        now = 0;
        std::array<unsigned, CapturedFrame::MAX_SPRITES> old_nearest, new_nearest, old_distance,
            new_distance;
        while (old < old_n && now < new_n) {
            const uint64_t a = key(m_sprites[old_order[old]].value),
                           b = key(current[new_order[now]]);
            if (a < b) {
                ++old;
                continue;
            }
            if (b < a) {
                ++now;
                continue;
            }
            unsigned old_end = old + 1, now_end = now + 1;
            while (old_end < old_n && key(m_sprites[old_order[old_end]].value) == a) ++old_end;
            while (now_end < new_n && key(current[new_order[now_end]]) == b) ++now_end;
            if (old_end - old == 1 && now_end - now == 1) {
                matches[new_order[now]] = old_order[old];
            } else {
                for (unsigned p = old; p < old_end; ++p) {
                    old_nearest[p] = CapturedFrame::MAX_SPRITES;
                    old_distance[p] = std::numeric_limits<unsigned>::max();
                }
                for (unsigned n = now; n < now_end; ++n) {
                    new_nearest[n] = CapturedFrame::MAX_SPRITES;
                    new_distance[n] = std::numeric_limits<unsigned>::max();
                    const auto &v = current[new_order[n]];
                    for (unsigned p = old; p < old_end; ++p) {
                        const auto &previous = m_sprites[old_order[p]].value;
                        const int64_t dx = int64_t(v.x) - previous.x;
                        const int64_t dy = int64_t(v.y) - previous.y;
                        if (std::abs(dx) > int64_t(32) * 256 || std::abs(dy) > int64_t(32) * 256)
                            continue;
                        const unsigned distance = unsigned(dx * dx + dy * dy);
                        if (distance < old_distance[p]) {
                            old_distance[p] = distance;
                            old_nearest[p] = n;
                        } else if (distance == old_distance[p])
                            old_nearest[p] = CapturedFrame::MAX_SPRITES;
                        if (distance < new_distance[n]) {
                            new_distance[n] = distance;
                            new_nearest[n] = p;
                        } else if (distance == new_distance[n])
                            new_nearest[n] = CapturedFrame::MAX_SPRITES;
                    }
                }
                for (unsigned n = now; n < now_end; ++n)
                    if (new_nearest[n] != CapturedFrame::MAX_SPRITES &&
                        old_nearest[new_nearest[n]] == n)
                        matches[new_order[n]] = old_order[new_nearest[n]];
            }
            old = old_end;
            now = now_end;
        }
        // Restore demoted identity pairs whose both sides stayed unmatched.
        std::array<bool, CapturedFrame::MAX_SPRITES> old_used{};
        for (unsigned i = 0; i < scene.sprite_count; ++i)
            if (matches[i] != CapturedFrame::MAX_SPRITES) old_used[matches[i]] = true;
        for (unsigned i = 0; i < scene.sprite_count; ++i)
            if (demoted[i] != CapturedFrame::MAX_SPRITES &&
                matches[i] == CapturedFrame::MAX_SPRITES && !old_used[demoted[i]]) {
                matches[i] = demoted[i];
                by_identity[i] = true;
                old_used[demoted[i]] = true;
            }
    }
    // Per-object rigid check. Entries that must agree with their object's reference delta: identity
    // matches whose tile/palette changed and appearance-fallback matches (identity sprites only;
    // identity-less sprites are never checked). Same-tile identity matches are unchecked.
    // The reference delta of an object (the SceneSprite::object shared by its entries) is the mode
    // of (dx, dy) over its same-tile identity matches (must be a unique mode); if it has none, the
    // mode over all its matched entries (identity and fallback) provided a strict majority holds it.
    // A checked entry is rigid when its delta is within +-1 native px (256) of that reference per
    // axis. No reference or object 0: not rigid (snaps). A single-entry object passes trivially.
    // Sort-based, O(n log n) over fixed arrays.
    std::array<bool, CapturedFrame::MAX_SPRITES> rigid{};
    if (m_paired && sprites_changed) {
        std::array<unsigned, CapturedFrame::MAX_SPRITES> order;
        unsigned n = 0;
        for (unsigned i = 0; i < scene.sprite_count; ++i)
            if (matches[i] != CapturedFrame::MAX_SPRITES && current[i].identity &&
                current[i].object)
                order[n++] = i;
        // Reference-forming entries: same-tile identity matches.
        auto changed = [&](unsigned i) {
            return !(by_identity[i] && same_appearance(current[i], m_sprites[matches[i]].value));
        };
        auto delta = [&](unsigned i) {
            const auto &p = m_sprites[matches[i]].value;
            return std::pair<int64_t, int64_t>{int64_t(current[i].x) - p.x,
                                               int64_t(current[i].y) - p.y};
        };
        std::sort(order.begin(), order.begin() + n, [&](unsigned a, unsigned b) {
            if (current[a].object != current[b].object)
                return current[a].object < current[b].object;
            if (changed(a) != changed(b)) return !changed(a);
            return delta(a) < delta(b);
        });
        for (unsigned g = 0, e; g < n; g = e) {
            e = g + 1;
            while (e < n && current[order[e]].object == current[order[g]].object) ++e;
            unsigned c = g;
            while (c < e && !changed(order[c])) ++c;
            const unsigned lo = g, hi = c > g ? c : e;
            unsigned best = 0, best_at = lo;
            bool tie = false;
            for (unsigned r = lo, re; r < hi; r = re) {
                re = r + 1;
                while (re < hi && delta(order[re]) == delta(order[r])) ++re;
                if (re - r > best) {
                    best = re - r;
                    best_at = r;
                    tie = false;
                } else if (re - r == best) tie = true;
            }
            const bool has = !tie && (c > g || best * 2 > hi - lo);
            if (!has) continue;
            const auto ref = delta(order[best_at]);
            for (unsigned k = c; k < e; ++k) {
                const auto d = delta(order[k]);
                rigid[order[k]] =
                    std::abs(d.first - ref.first) <= 256 && std::abs(d.second - ref.second) <= 256;
            }
        }
    }
    // Compute all deltas before overwriting old compact slots.
    std::array<int32_t, CapturedFrame::MAX_SPRITES> dx{}, dy{};
    std::array<bool, CapturedFrame::MAX_SPRITES> eligible{};
    for (unsigned i = 0; m_paired && sprites_changed && i < scene.sprite_count; ++i) {
        const auto &v = current[i];
        const unsigned match = matches[i];
        if (match == CapturedFrame::MAX_SPRITES) {
            // Identical stationary duplicates are not evidence of moving rejects.
            if (i < m_sprite_count && v == m_sprites[i].value) continue;
            ++m_pair_stats.rejected_sprites;
            ++m_pair_stats.sprite_identity_rejections;
            if (scene.sprite_count != m_sprite_count) ++m_pair_stats.sprite_count_rejections;
            continue;
        }
        const auto &previous = m_sprites[match].value;
        const bool moving = v.x != previous.x || v.y != previous.y;
        if (moving) ++m_pair_stats.moving_sprites;
        const bool checked = v.identity && !(by_identity[i] && same_appearance(v, previous));
        const bool transform = m_pen_mask == scene.pen_mask && valid_sprite(v) &&
                               valid_sprite(previous) && same_transform(v, previous) &&
                               (!checked || rigid[i]);
        const bool jump = motion_delta(previous.x, v.x) && motion_delta(previous.y, v.y);
        eligible[i] = transform && jump;
        if (eligible[i]) {
            dx[i] = v.x - previous.x;
            dy[i] = v.y - previous.y;
        } else if (moving) {
            ++m_pair_stats.rejected_sprites;
            if (!transform) ++m_pair_stats.sprite_transform_rejections;
            else ++m_pair_stats.sprite_jump_rejections;
        }
    }
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
