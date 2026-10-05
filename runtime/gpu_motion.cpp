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
    const bool sprite_pair = paired_ && sprite_count_ == scene.sprite_count && pen_mask_ == scene.pen_mask;
    for (unsigned i = 0; i < scene.sprite_count; ++i) {
        auto &s = sprites_[i];
        std::array<uint32_t, 7> now;
        std::copy_n(scene.words.begin() + GpuScene::sprites + i * GpuScene::sprite_stride, 7, now.begin());
        const auto valid = [](const auto &v) {
            return v[2] > 0 && v[2] <= 256 && v[3] > 0 && v[3] <= 256 &&
                v[4] < 32768 && v[5] <= 255 && v[6] <= 3 &&
                std::abs(int64_t(int32_t(v[0]))) <= 1024 * 256 &&
                std::abs(int64_t(int32_t(v[1]))) <= 1024 * 256;
        };
        s.eligible = sprite_pair && valid(now) && valid(s.words) &&
            std::equal(now.begin() + 2, now.end(), s.words.begin() + 2) &&
            motion_delta(int32_t(s.words[0]), int32_t(now[0])) &&
            motion_delta(int32_t(s.words[1]), int32_t(now[1]));
        s.dx = s.eligible ? int32_t(now[0]) - int32_t(s.words[0]) : 0;
        s.dy = s.eligible ? int32_t(now[1]) - int32_t(s.words[1]) : 0;
        s.words = now;
    }
    for (unsigned y = 24; y < 256; ++y) {
        auto &before = rows_[y - 24];
        Row now = read_row(scene, y);
        bool controls = paired_ && !before.bitmap && !now.bitmap && before.controls == now.controls;
        for (unsigned channel = 0; channel < 4; ++channel)
            controls &= ((now.controls[1] >> (channel * 8)) & 255) <= 8;
        for (unsigned i = 0; i < 5; ++i) {
            bool eligible = controls && now.layers[i] == before.layers[i] &&
                valid_layer(now.layers[i]) && valid_layer(before.layers[i]);
            int32_t x = 0, px = 0, phase = 0, previous_phase = 0;
            if (i < 4) {
                const unsigned at = i * 6;
                const auto valid = [at](const Row &r) {
                    const auto &g = r.geometry;
                    return std::abs(int64_t(int32_t(g[at]))) <= (int64_t{1} << 24) &&
                        g[at + 1] <= 511 && g[at + 2] > 0 && g[at + 2] <= 256 &&
                        g[at + 3] > 0 && g[at + 3] <= 510 && g[at + 4] <= 255;
                };
                eligible &= valid(now) && valid(before) && now.geometry[at + 2] == before.geometry[at + 2] &&
                    now.geometry[at + 3] == before.geometry[at + 3];
                x = int32_t(now.geometry[at]); px = int32_t(before.geometry[at]);
                phase = int32_t(now.geometry[at + 1] * 256 + now.geometry[at + 4]);
                previous_phase = int32_t(before.geometry[at + 1] * 256 + before.geometry[at + 4]);
            } else {
                eligible &= now.geometry[24] <= 511 && now.geometry[25] <= 511 &&
                    before.geometry[24] <= 511 && before.geometry[25] <= 511;
                x = int32_t(now.geometry[24] * 256); px = int32_t(before.geometry[24] * 256);
                phase = int32_t(now.geometry[25] * 256); previous_phase = int32_t(before.geometry[25] * 256);
            }
            eligible &= motion_delta(px, x, (i < 4 ? 1024 : 512) * 256) &&
                motion_delta(previous_phase, phase, 512 * 256);
            now.eligible[i] = eligible;
            if (eligible) { now.delta[i * 2] = x - px; now.delta[i * 2 + 1] = phase - previous_phase; }
        }
        before = now;
    }
    frame_ = frame; sprite_count_ = scene.sprite_count; pen_mask_ = scene.pen_mask;
    captured_ = true;
}
MotionInterpolationStats GpuMotionHistory::apply(const GpuScene &scene, float alpha,
                                                 std::span<uint32_t> words) const noexcept {
    MotionInterpolationStats stats;
    if (!std::isfinite(alpha) || alpha < 0 || alpha > 1 || !paired_ || scene.fallback ||
        words.size() < GpuScene::word_count || scene.sprite_count != sprite_count_ || scene.pen_mask != pen_mask_) return stats;
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
            if (!r.eligible[i] || (!r.delta[i * 2] && !r.delta[i * 2 + 1]) ||
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
