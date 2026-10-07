#include "renderer/gpu/motion.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>

namespace {
using f3rt::CapturedFrame;
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
void sprite(CapturedFrame &s, unsigned slot, int x, unsigned tile = 10) {
    s.sprites[slot] = {x * 256, 20 * 256, 256, 256, tile, 0, false, false};
    s.sprite_count = std::max(s.sprite_count, slot + 1);
}
f3rt::SceneRow &row24(CapturedFrame &s) { return s.rows[24]; }
f3rt::ScenePlayfield &pf0(CapturedFrame &s) { return row24(s).playfields[0]; }
void layer(CapturedFrame &s) {
    auto &p = pf0(s);
    p.layer.enabled = true;
    p.source_x = 100 * 256; p.source_y = 100;
    p.x_step = 256; p.y_step = 256;
}
f3rt::MotionResult pair(const CapturedFrame &a, const CapturedFrame &b) {
    f3rt::GpuMotionHistory history;
    history.capture(a, 10); history.capture(b, 11);
    return history.apply(b, 0.5f);
}
}
int main() {
    try {
        auto a = std::make_unique<CapturedFrame>(); a->fallback = false;
        sprite(*a, 0, 20); sprite(*a, 1, 80, 11);
        auto b = std::make_unique<CapturedFrame>(*a);
        // An unrelated insertion shifts both compact slots, but neither object identity.
        sprite(*b, 0, 150, 12); sprite(*b, 1, 24); sprite(*b, 2, 84, 11);
        auto result = pair(*a, *b);
        auto stats = result.stats;
        require(stats.sprites == 2 && stats.moving_sprites == 2, "count insertion lost stable motion");
        require(result.state.sprite_x[1] == 22 * 256, "shifted sprite midpoint wrong");
        stats = pair(*b, *a).stats;
        require(stats.sprites == 2, "count removal lost stable motion");
        // Equal-distance identity ties must snap, rather than choose a neighbor.
        *b = *a; sprite(*a, 1, 24); sprite(*b, 0, 22); sprite(*b, 1, 26);
        stats = pair(*a, *b).stats;
        require(stats.sprites == 0 && stats.sprite_identity_rejections == 2, "ambiguous duplicates interpolated");
        // A dense, identically textured group still has unambiguous small motion.
        *b = *a; sprite(*a, 1, 36); sprite(*b, 0, 24); sprite(*b, 1, 40);
        result = pair(*a, *b);
        require(result.stats.sprites == 2 && result.state.sprite_x[0] == 22 * 256 &&
            result.state.sprite_x[1] == 38 * 256,
            "dense duplicate group lost uniquely nearest motion");
        a->sprite_count = 1; *b = *a; sprite(*b, 0, 24);
        for (unsigned field : {2u, 6u}) {
            const auto original = b->sprites[0];
            if (field == 2) b->sprites[0].scale_x = 128; else b->sprites[0].flip_x = true;
            stats = pair(*a, *b).stats;
            require(stats.sprites == 0 && stats.sprite_transform_rejections == 1, "zoom/flip interpolated");
            b->sprites[0] = original;
        }
        sprite(*b, 0, 24, 99);
        stats = pair(*a, *b).stats;
        require(stats.sprites == 0 && stats.sprite_identity_rejections == 1, "replacement tile interpolated");
        sprite(*b, 0, 100);
        stats = pair(*a, *b).stats;
        require(stats.sprites == 0 && stats.sprite_jump_rejections == 1, "large sprite jump interpolated");
        // An unchanged pair must not inflate movement or rejection statistics.
        stats = pair(*a, *a).stats;
        require(!stats.sprites && !stats.moving_sprites && !stats.rejected_sprites, "static sprite counted as motion");
        layer(*a); *b = *a;
        pf0(*b).source_x += 4 * 256;
        row24(*b).blend = {8, 8, 8, 8};
        row24(*b).playfields[1].layer.enabled = true; row24(*b).playfields[1].layer.priority = 9;
        result = pair(*a, *b);
        require(result.stats.playfield_rows == 1 && result.state.rows[24].source_x[0] == 102 * 256,
                "unrelated controls vetoed PF scroll");
        pf0(*b).source_y = 400;
        result = pair(*a, *b);
        require(result.state.rows[24].source_x[0] == 102 * 256 && result.state.rows[24].phase[0] == 400 * 256 &&
                result.stats.row_jump_rejections == 1, "Y jump vetoed valid X motion or failed to snap");
        pf0(*b).source_y = 104; pf0(*b).y_step = 128;
        result = pair(*a, *b);
        require(result.state.rows[24].source_x[0] == 102 * 256 && result.state.rows[24].phase[0] == 104 * 256 &&
                result.stats.row_transform_rejections == 1, "Y zoom vetoed valid X motion or failed to snap");
        pf0(*b).y_step = 256; pf0(*a).source_x = 1022 * 256; pf0(*b).source_x = 2 * 256;
        result = pair(*a, *b);
        require(result.state.rows[24].source_x[0] == 2 * 256 && result.state.rows[24].phase[0] == 102 * 256,
                "X wrap failed to snap independently");
        // The fixed-point jump boundary is inclusive, without integer rounding.
        sprite(*a, 0, 20); *b = *a; sprite(*b, 0, 52);
        result = pair(*a, *b);
        require(result.stats.sprites == 1 && result.state.sprite_x[0] == 36 * 256, "exact 32px boundary rejected");
        ++b->sprites[0].x;
        stats = pair(*a, *b).stats;
        require(!stats.sprites && stats.sprite_jump_rejections == 1, "32px plus subunit accepted");
        sprite(*b, 0, 24); ++b->sprites[0].palette;
        result = pair(*a, *b);
        require(!result.stats.sprites && result.state.sprite_x[0] == b->sprites[0].x, "palette replacement interpolated");
        b->sprites[0].palette = a->sprites[0].palette; b->pen_mask = 63;
        stats = pair(*a, *b).stats;
        require(!stats.sprites && stats.sprite_transform_rejections == 1, "pen mask change interpolated");
        b->pen_mask = a->pen_mask;
        // PF Y phase is fractional; palette adjustment remains current discrete state.
        pf0(*a).source_x = 100 * 256; pf0(*a).source_y = 100; pf0(*a).y_fraction = 255;
        *b = *a; pf0(*b).source_x += 1; pf0(*b).source_y = 101; pf0(*b).y_fraction = 1;
        pf0(*b).palette_add = 0x1234; // current discrete state; not part of MotionState
        result = pair(*a, *b);
        require(result.state.rows[24].source_x[0] == 100 * 256 + 1 &&
                result.state.rows[24].phase[0] == 101 * 256 + 0, "PF subunit rounding/phase lost");
        pf0(*b).layer.clip_enabled = 1; pf0(*b).layer.clip_inverse = true; row24(*b).clips[0] = {50, 300};
        result = pair(*a, *b);
        require(!result.stats.playfield_rows && result.stats.row_control_rejections == 1 &&
                result.state.rows[24].phase[0] == 101 * 256 + 1, "own PF clip change interpolated");
        auto &text = row24(*a).text;
        text.enabled = true;
        row24(*a).text_x = 100; row24(*a).text_y = 100;
        *b = *a; row24(*b).text_x = 101; row24(*b).text_y = 101;
        result = pair(*a, *b);
        require(result.stats.text_rows == 1 && result.state.rows[24].text_x == 100 * 256 + 128 &&
                result.state.rows[24].text_y == 100 * 256 + 128, "fractional text midpoint lost");
        row24(*b).text.clip_enabled = 1;
        result = pair(*a, *b);
        require(!result.stats.text_rows && result.state.rows[24].text_x == 101 * 256, "own text clip change interpolated");
        row24(*b).text.clip_enabled = 0;
        row24(*a).text_x = 511; row24(*b).text_x = 0;
        result = pair(*a, *b);
        require(result.state.rows[24].text_x == 0 && result.state.rows[24].text_y == 100 * 256 + 128,
                "text wrap failed to snap independently");
        f3rt::GpuMotionHistory history;
        history.capture(*a, 10); history.capture(*b, 11);
        for (float alpha : {-0.1f, 1.1f, std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()})
            require(!history.apply(*b, alpha).stats.paired, "invalid alpha paired");
        history.capture(*b, 11);
        require(!history.apply(*b, 0.5f).stats.paired, "duplicate frame paired");
        history.capture(*b, 13);
        require(!history.apply(*b, 0.5f).stats.paired, "history gap paired");
        b->fallback = true; history.capture(*b, 14); b->fallback = false;
        history.capture(*b, 15);
        require(!history.apply(*b, 0.5f).stats.paired, "fallback recovery paired too early");
        history.capture(*b, 16);
        require(history.apply(*b, 0.5f).stats.paired, "fallback recovery never resumed pairing");
        history.reset();
        require(!history.apply(*b, 0.5f).stats.paired, "reset retained pairing");
        std::cout << "Motion identity, count shifts, ambiguity, static rates and independent-axis guards passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
