#include "renderer/gpu/encode.hpp"
#include "renderer/game/clip.hpp"
#include <algorithm>
#include <bit>
#include <cstring>
#include <stdexcept>

namespace f3rt {
namespace {
static_assert(std::endian::native == std::endian::little, "glyph RAM is packed little-endian");
static_assert(F3_SCENE_PF_CELLS + 4 * F3_SCENE_PF_LAYER_CELLS * F3_SCENE_PF_CELL_STRIDE == F3_SCENE_TEXT_CELLS);
static_assert(F3_SCENE_TEXT_CELLS + 4096 == F3_SCENE_GLYPHS);
static_assert(F3_SCENE_GLYPHS + 4096 == F3_SCENE_PALETTE);
static_assert(F3_SCENE_PALETTE + F3_SCENE_PALETTE_WORDS <= F3_SCENE_ROWS);
static_assert(F3_SCENE_ROWS + F3_SCENE_ROW_COUNT * F3_SCENE_ROW_STRIDE == F3_SCENE_SPRITES);
static_assert(F3_SCENE_SPRITES + F3_SCENE_SPRITE_COUNT * F3_SCENE_SPRITE_STRIDE == F3_SCENE_WORD_COUNT);
static_assert(F3_SCENE_INTERP == F3_SCENE_WORD_COUNT);
static_assert(4 * F3_INTERP_PF_STRIDE == F3_SCENE_INTERP_ROW_STRIDE);
static_assert(F3_SCENE_INTERP + F3_SCENE_ROW_COUNT * F3_SCENE_INTERP_ROW_STRIDE == F3_SCENE_INTERP_WORD_COUNT);
static_assert(F3_ROW_ORDER + layer_count <= F3_ROW_MOTION_TEXT_X && F3_ROW_MOTION_TEXT_Y < F3_ROW_LAYERS);
static_assert(F3_LAYER_CLIPS + 2 * 16 == F3_LAYER_STRIDE);
static_assert(F3_ROW_LAYERS + layer_count * F3_LAYER_STRIDE == F3_ROW_PF);
static_assert(F3_ROW_PF + 4 * F3_PF_STRIDE <= F3_SCENE_ROW_STRIDE);
static_assert(GameTiles::map_count == 4 || GameTiles::map_count == 6);
static_assert(CapturedFrame::max_sprites == F3_SCENE_SPRITE_COUNT && CapturedFrame::palette_size == F3_SCENE_PALETTE_WORDS);

uint32_t layer_flags(const SceneLayer &l) {
    return uint32_t(l.priority) | (uint32_t(l.blend_mode) << F3_LAYER_MODE_SHIFT) |
        (l.enabled ? F3_LAYER_ENABLED : 0u) | (uint32_t(l.blend_select) << F3_LAYER_SELECT_SHIFT) |
        (l.mosaic ? F3_LAYER_MOSAIC : 0u);
}
constexpr unsigned alternate_maps = GameTiles::map_count - 4;
// `presented` (expanded output) routes playfield geometry through presented_playfield(): alt-map rows
// with an exact full-resolution twin sample the main map at twice the step. Motion keeps moving such a
// row horizontally (the remap is linear in source_x); a row whose vertical phase moved samples map rows
// the twin was not solved for, so it keeps the canonical alternate map.
void encode_row(const SceneRow &r, const MotionState::Row *motion, bool presented, int16_t left, int16_t right,
                uint32_t *at) {
    std::array<ScenePlayfield, 4> pf = r.playfields;
    std::array<bool, 4> moved_y{};
    for (unsigned i = 0; i < 4; ++i) {
        if (!motion) continue;
        pf[i].source_x = motion->source_x[i];
        moved_y[i] = motion->phase[i] != int32_t(uint32_t(r.playfields[i].source_y) * 256u + r.playfields[i].y_fraction);
    }
    if (presented)
        for (unsigned i = 0; i < 4; ++i)
            if (!moved_y[i]) pf[i] = presented_playfield(pf[i]);
    at[F3_ROW_BACKGROUND] = r.background;
    at[F3_ROW_MOSAIC] = r.mosaic_period | (r.palette_15bit ? F3_ROW_PALETTE15 : 0u) | (r.blur ? F3_ROW_BLUR : 0u);
    at[F3_ROW_TEXT_X] = uint32_t(int32_t(r.text_x));
    at[F3_ROW_TEXT_Y] = uint32_t(int32_t(r.text_y));
    at[F3_ROW_BLEND] = r.blend[0] | (uint32_t(r.blend[1]) << 8) | (uint32_t(r.blend[2]) << 16) |
        (uint32_t(r.blend[3]) << 24);
    const auto order = scene_order(r);
    for (unsigned i = 0; i < layer_count; ++i) {
        at[F3_ROW_ORDER + i] = unsigned(order[i]);
        const auto &l = r.layer(LayerId(i));
        uint32_t *la = at + F3_ROW_LAYERS + i * F3_LAYER_STRIDE;
        la[F3_LAYER_FLAGS] = layer_flags(l);
        if (i < 4 && pf[i].alt_map) la[F3_LAYER_FLAGS] |= F3_LAYER_ALT_MAP;
        if (i < 4 && r.playfields[i].empty_row) la[F3_LAYER_FLAGS] &= ~F3_LAYER_ENABLED;
        const auto clips = clip_ranges(r, l, left, right);
        la[F3_LAYER_CLIP_COUNT] = clips.count;
        for (unsigned c = 0; c < clips.count; ++c) {
            la[F3_LAYER_CLIPS + c * 2] = uint32_t(int32_t(clips.ranges[c].left));
            la[F3_LAYER_CLIPS + c * 2 + 1] = uint32_t(int32_t(clips.ranges[c].right));
        }
    }
    for (unsigned i = 0; i < 4; ++i) {
        const auto &p = pf[i];
        uint32_t *pa = at + F3_ROW_PF + i * F3_PF_STRIDE;
        pa[F3_PF_SOURCE_X] = uint32_t(p.source_x);
        pa[F3_PF_SOURCE_Y] = uint32_t(p.source_y);
        pa[F3_PF_X_STEP] = uint32_t(p.x_step);
        pa[F3_PF_Y_STEP] = uint32_t(p.y_step);
        pa[F3_PF_Y_FRACTION] = p.y_fraction;
        pa[F3_PF_PALETTE_ADD] = p.palette_add;
        // The captured phase is kept bit-exact where motion did not move it.
        if (moved_y[i]) {
            pa[F3_PF_SOURCE_Y] = uint32_t(motion->phase[i]) >> 8;
            pa[F3_PF_Y_FRACTION] = uint32_t(motion->phase[i]) & 255u;
        }
    }
    at[F3_ROW_MOTION_TEXT_X] = motion ? uint32_t(motion->text_x) : 0u;
    at[F3_ROW_MOTION_TEXT_Y] = motion ? uint32_t(motion->text_y) : 0u;
}
void encode_coefficients(const InterpolationCoefficients &coefficients, uint32_t *out) {
    for (unsigned y = 0; y < F3_SCENE_ROW_COUNT; ++y)
        for (unsigned pf = 0; pf < 4; ++pf) {
            const auto &c = coefficients.rows[y][pf];
            uint32_t *at = out + y * F3_SCENE_INTERP_ROW_STRIDE + pf * F3_INTERP_PF_STRIDE;
            at[0] = c.flags;
            if (c.flags & F3_INTERP_FLAG_PALETTE) at[0] |= uint32_t(c.palette_stride) << F3_INTERP_PALETTE_STRIDE_SHIFT;
            const std::array<float, 3> *triples[]{&c.source, &c.zoom, &c.vertical, &c.palette};
            constexpr unsigned offsets[]{F3_INTERP_SOURCE, F3_INTERP_ZOOM, F3_INTERP_VERTICAL, F3_INTERP_PALETTE};
            for (unsigned t = 0; t < 4; ++t)
                for (unsigned k = 0; k < 3; ++k) at[offsets[t] + k] = std::bit_cast<uint32_t>((*triples[t])[k]);
        }
}
} // namespace

void encode(const CapturedFrame &frame, GameVideoOptions options, const MotionState *motion,
            const InterpolationCoefficients *coefficients, std::span<uint32_t> out) {
    if (frame.fallback) {
        if (out.size() < encoded_native_words) throw std::invalid_argument("GPU native upload too small");
        std::memcpy(out.data(), frame.native_pixels.data(), encoded_native_words * sizeof(uint32_t));
        return;
    }
    if (out.size() < (coefficients ? encoded_interpolation_words : encoded_scene_words) ||
        frame.sprite_count > F3_SCENE_SPRITE_COUNT)
        throw std::invalid_argument("GPU scene upload too small");
    uint32_t *w = out.data();
    for (unsigned l = 0; l < 4; ++l) {
        // Raw 4-byte video-RAM cell (attributes<<16 | code); the shader decodes it
        // exactly like GameTiles::RowSampler. Word 1 of each slot is unused.
        const auto cells = frame.tiles.cells(l);
        // Only PF2/PF3 have alternates (maps 4/5); word 1 carries them.
        const bool alternate = alternate_maps && l >= 2;
        const auto alt_cells = frame.tiles.cells(alternate ? l + 2 : l);
        uint32_t *dst = w + F3_SCENE_PF_CELLS + l * F3_SCENE_PF_LAYER_CELLS * F3_SCENE_PF_CELL_STRIDE;
        for (unsigned i = 0; i < F3_SCENE_PF_LAYER_CELLS; ++i) {
            dst[i * 2] = cells[i];
            dst[i * 2 + 1] = alternate ? alt_cells[i] : 0;
        }
    }
    // Raw big-endian text-map words; the shader decodes them like GameText::pixel.
    const auto map = frame.text.map();
    std::copy(map.begin(), map.end(), w + F3_SCENE_TEXT_CELLS);
    // Raw glyph RAM, byte-packed four bytes per word; the shader indexes it as bytes.
    static_assert(0x2000 == F3_SCENE_GLYPH_WORDS * sizeof(uint32_t));
    std::memcpy(w + F3_SCENE_GLYPHS, frame.text.glyphs().data(), 0x2000);
    std::memcpy(w + F3_SCENE_PALETTE, frame.colors.data(), F3_SCENE_PALETTE_WORDS * sizeof(uint32_t));
    const int16_t left = int16_t(46 - int(options.border)), right = int16_t(366 + options.border);
    for (unsigned y = 0; y < F3_SCENE_ROW_COUNT; ++y)
        encode_row(frame.rows[y], motion ? &motion->rows[y] : nullptr, options.expanded(), left, right,
                   w + F3_SCENE_ROWS + y * F3_SCENE_ROW_STRIDE);
    for (unsigned i = 0; i < frame.sprite_count; ++i) {
        const auto &s = frame.sprites[i];
        uint32_t *at = w + F3_SCENE_SPRITES + i * F3_SCENE_SPRITE_STRIDE;
        at[F3_SPRITE_X] = uint32_t(motion ? motion->sprite_x[i] : s.x);
        at[F3_SPRITE_Y] = uint32_t(motion ? motion->sprite_y[i] : s.y);
        at[F3_SPRITE_SCALE_X] = s.scale_x;
        at[F3_SPRITE_SCALE_Y] = s.scale_y;
        at[F3_SPRITE_TILE] = s.tile;
        at[F3_SPRITE_PALETTE] = s.palette;
        at[F3_SPRITE_FLIP] = (s.flip_x ? F3_SPRITE_FLIP_X : 0u) | (s.flip_y ? F3_SPRITE_FLIP_Y : 0u) |
                             (s.shadow ? F3_SPRITE_SHADOW : 0u);
    }
    if (coefficients) encode_coefficients(*coefficients, w + F3_SCENE_INTERP);
}
} // namespace f3rt
