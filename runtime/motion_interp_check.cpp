#include "gpu_motion.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace {
using f3rt::GpuScene;
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
void sprite(GpuScene &s, unsigned slot, int x, unsigned tile = 10) {
    const unsigned at = GpuScene::sprites + slot * GpuScene::sprite_stride;
    const uint32_t words[]{uint32_t(x * 256), 20 * 256, 256, 256, tile, 0, 0};
    std::copy_n(words, 7, s.words.begin() + at);
    s.sprite_count = std::max(s.sprite_count, slot + 1);
}
void layer(GpuScene &s) {
    const unsigned row = GpuScene::rows + 24 * GpuScene::row_stride;
    const unsigned at = row + GpuScene::row_layers;
    s.words[at] = 64; s.words[at + 1] = 1;
    s.words[at + 2] = 0; s.words[at + 3] = 320;
    const unsigned pf = row + GpuScene::row_pf;
    s.words[pf] = 100 * 256; s.words[pf + 1] = 100;
    s.words[pf + 2] = 256; s.words[pf + 3] = 256;
}
f3rt::MotionInterpolationStats pair(const GpuScene &a, const GpuScene &b, std::vector<uint32_t> &out) {
    f3rt::GpuMotionHistory history;
    history.capture(a, 10); history.capture(b, 11);
    out.assign(b.words.begin(), b.words.end());
    return history.apply(b, 0.5f, out);
}
}
int main() {
    try {
        auto a = std::make_unique<GpuScene>(); a->fallback = false;
        sprite(*a, 0, 20); sprite(*a, 1, 80, 11);
        auto b = std::make_unique<GpuScene>(*a);
        std::vector<uint32_t> out;
        // An unrelated insertion shifts both compact slots, but neither object identity.
        sprite(*b, 0, 150, 12); sprite(*b, 1, 24); sprite(*b, 2, 84, 11);
        auto stats = pair(*a, *b, out);
        require(stats.sprites == 2 && stats.moving_sprites == 2, "count insertion lost stable motion");
        require(out[GpuScene::sprites + GpuScene::sprite_stride] == 22 * 256, "shifted sprite midpoint wrong");
        stats = pair(*b, *a, out);
        require(stats.sprites == 2, "count removal lost stable motion");
        // Equal-distance identity ties must snap, rather than choose a neighbor.
        *b = *a; sprite(*a, 1, 24); sprite(*b, 0, 22); sprite(*b, 1, 26);
        stats = pair(*a, *b, out);
        require(stats.sprites == 0 && stats.sprite_identity_rejections == 2, "ambiguous duplicates interpolated");
        // A dense, identically textured group still has unambiguous small motion.
        *b = *a; sprite(*a, 1, 36); sprite(*b, 0, 24); sprite(*b, 1, 40);
        stats = pair(*a, *b, out);
        require(stats.sprites == 2 && out[GpuScene::sprites] == 22 * 256 &&
            out[GpuScene::sprites + GpuScene::sprite_stride] == 38 * 256,
            "dense duplicate group lost uniquely nearest motion");
        a->sprite_count = 1; *b = *a; sprite(*b, 0, 24);
        const unsigned sp = GpuScene::sprites;
        for (unsigned field : {2u, 6u}) {
            const uint32_t original = b->words[sp + field];
            b->words[sp + field] = field == 2 ? 128 : 1;
            stats = pair(*a, *b, out);
            require(stats.sprites == 0 && stats.sprite_transform_rejections == 1, "zoom/flip interpolated");
            b->words[sp + field] = original;
        }
        sprite(*b, 0, 24, 99);
        stats = pair(*a, *b, out);
        require(stats.sprites == 0 && stats.sprite_identity_rejections == 1, "replacement tile interpolated");
        sprite(*b, 0, 100);
        stats = pair(*a, *b, out);
        require(stats.sprites == 0 && stats.sprite_jump_rejections == 1, "large sprite jump interpolated");
        // An unchanged pair must not inflate movement or rejection statistics.
        stats = pair(*a, *a, out);
        require(!stats.sprites && !stats.moving_sprites && !stats.rejected_sprites, "static sprite counted as motion");
        layer(*a); *b = *a;
        const unsigned row = GpuScene::rows + 24 * GpuScene::row_stride;
        const unsigned pf = row + GpuScene::row_pf;
        b->words[pf] += 4 * 256;
        b->words[row + 4] = 0x08080808; b->words[row + 5] = 8;
        b->words[row + GpuScene::row_layers + GpuScene::layer_stride] = 73;
        stats = pair(*a, *b, out);
        require(stats.playfield_rows == 1 && out[pf] == 102 * 256, "unrelated controls vetoed PF scroll");
        b->words[pf + 1] = 400;
        stats = pair(*a, *b, out);
        require(out[pf] == 102 * 256 && out[pf + 1] == 400 && stats.row_jump_rejections == 1,
                "Y jump vetoed valid X motion or failed to snap");
        b->words[pf + 1] = 104; b->words[pf + 3] = 128;
        stats = pair(*a, *b, out);
        require(out[pf] == 102 * 256 && out[pf + 1] == 104 && stats.row_transform_rejections == 1,
                "Y zoom vetoed valid X motion or failed to snap");
        b->words[pf + 3] = 256; a->words[pf] = 1022 * 256; b->words[pf] = 2 * 256;
        stats = pair(*a, *b, out);
        require(out[pf] == 2 * 256 && out[pf + 1] == 102, "X wrap failed to snap independently");
        // The fixed-point jump boundary is inclusive, without integer rounding.
        sprite(*a, 0, 20); *b = *a; sprite(*b, 0, 52);
        stats = pair(*a, *b, out);
        require(stats.sprites == 1 && out[sp] == 36 * 256, "exact 32px boundary rejected");
        ++b->words[sp];
        stats = pair(*a, *b, out);
        require(!stats.sprites && stats.sprite_jump_rejections == 1, "32px plus subunit accepted");
        sprite(*b, 0, 24); ++b->words[sp + 5];
        stats = pair(*a, *b, out);
        require(!stats.sprites && out[sp + 5] == b->words[sp + 5], "palette replacement interpolated");
        b->words[sp + 5] = a->words[sp + 5]; b->pen_mask = 63;
        stats = pair(*a, *b, out);
        require(!stats.sprites && stats.sprite_transform_rejections == 1, "pen mask change interpolated");
        b->pen_mask = a->pen_mask;
        // PF Y phase is fractional; palette adjustment remains current discrete state.
        a->words[pf] = 100 * 256; a->words[pf + 1] = 100; a->words[pf + 4] = 255;
        *b = *a; b->words[pf] += 1; b->words[pf + 1] = 101; b->words[pf + 4] = 1;
        b->words[pf + 5] = 0x1234;
        stats = pair(*a, *b, out);
        require(out[pf] == 100 * 256 + 1 && out[pf + 1] == 101 && out[pf + 4] == 0 &&
                out[pf + 5] == 0x1234, "PF subunit rounding/phase or current palette lost");
        const unsigned own_layer = row + GpuScene::row_layers;
        ++b->words[own_layer + 2];
        stats = pair(*a, *b, out);
        require(!stats.playfield_rows && stats.row_control_rejections == 1 &&
                out[pf + 4] == 1, "own PF clip change interpolated");
        const unsigned text = row + GpuScene::row_layers + 8 * GpuScene::layer_stride;
        a->words[text] = 64; a->words[text + 1] = 1;
        a->words[text + 2] = 0; a->words[text + 3] = 320;
        a->words[row + 2] = 100; a->words[row + 3] = 100;
        *b = *a; b->words[row + 2] = 101; b->words[row + 3] = 101;
        stats = pair(*a, *b, out);
        require(stats.text_rows == 1 && out[row + 14] == 100 * 256 + 128 &&
                out[row + 15] == 100 * 256 + 128, "fractional text midpoint lost");
        b->reference_rows[24].text.clip_enabled = 1;
        stats = pair(*a, *b, out);
        require(!stats.text_rows && out[row + 14] == 101 * 256, "own text clip change interpolated");
        b->reference_rows[24].text.clip_enabled = 0;
        a->words[row + 2] = 511; b->words[row + 2] = 0;
        stats = pair(*a, *b, out);
        require(out[row + 14] == 0 && out[row + 15] == 100 * 256 + 128,
                "text wrap failed to snap independently");
        f3rt::GpuMotionHistory history;
        history.capture(*a, 10); history.capture(*b, 11);
        out.assign(b->words.begin(), b->words.end());
        for (float alpha : {-0.1f, 1.1f, std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()}) {
            const auto before = out;
            require(!history.apply(*b, alpha, out).paired && out == before, "invalid alpha changed upload");
        }
        history.capture(*b, 11);
        require(!history.apply(*b, 0.5f, out).paired, "duplicate frame paired");
        history.capture(*b, 13);
        require(!history.apply(*b, 0.5f, out).paired, "history gap paired");
        b->fallback = true; history.capture(*b, 14); b->fallback = false;
        history.capture(*b, 15);
        require(!history.apply(*b, 0.5f, out).paired, "fallback recovery paired too early");
        history.capture(*b, 16);
        require(history.apply(*b, 0.5f, out).paired, "fallback recovery never resumed pairing");
        history.reset();
        require(!history.apply(*b, 0.5f, out).paired, "reset retained pairing");
        std::cout << "Motion identity, count shifts, ambiguity, static rates and independent-axis guards passed\n";
        return 0;
    } catch (const std::exception &e) {
        std::cerr << e.what() << '\n'; return 1;
    }
}
