#include "renderer/gpu/motion.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>
#include "renderer/decode.hpp"

namespace {
using f3rt::CapturedFrame;
void require(bool value, const char *message) {
    if (!value) throw std::runtime_error(message);
}
void sprite(CapturedFrame &s, unsigned slot, int x, unsigned tile = 10) {
    s.sprites[slot] = {x * 256, 20 * 256, 256, 256, tile, 0, false, false};
    s.sprite_count = std::max(s.sprite_count, slot + 1);
}
f3rt::SceneRow &row24(CapturedFrame &s) { return s.rows[f3rt::geometry::first_line]; }
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
void put16(uint8_t *p, uint16_t v) { p[0] = uint8_t(v >> 8); p[1] = uint8_t(v); }
struct Entry {
    uint16_t tile = 0, zoom = 0, x = 0, y = 0, cont = 0, color = 0, w5 = 0, w6 = 0;
};
void write_entry(uint8_t *p, const Entry &e) {
    put16(p + 0, e.tile); put16(p + 2, e.zoom); put16(p + 4, e.x);
    put16(p + 6, e.y); put16(p + 8, uint16_t((e.cont << 8) | e.color));
    put16(p + 10, e.w5); put16(p + 12, e.w6);
}
void put_entry(std::vector<uint8_t> &ram, unsigned bank, unsigned index, const Entry &e) {
    write_entry(&ram[bank * 0x8000 + index * 16], e);
}
std::vector<f3rt::DecodedSpriteEntry> decode(const std::vector<uint8_t> &ram,
                                             const f3rt::SpritePresentation *p = nullptr) {
    f3rt::SpriteRamState state;
    std::vector<f3rt::DecodedSpriteEntry> out(64);
    out.resize(f3rt::decode_sprite_list(ram.data(), int(f3rt::geometry::first_line), int(f3rt::geometry::height), out, state, false, p));
    return out;
}
bool same_geometry(const f3rt::DecodedSpriteEntry &a, const f3rt::DecodedSpriteEntry &b) {
    return a.x == b.x && a.y == b.y && a.scale_x == b.scale_x && a.scale_y == b.scale_y && a.tile == b.tile &&
           a.color == b.color && a.flip_x == b.flip_x && a.flip_y == b.flip_y && a.pri == b.pri;
}
void decode_checks() {
    using namespace f3rt;
    std::vector<uint8_t> ram(0x10000, 0);
    put_entry(ram, 0, 0, {.tile = 1, .x = 100, .y = 50});
    put_entry(ram, 0, 1, {.tile = 2, .x = 120, .y = 50});
    put_entry(ram, 0, 2, {.tile = 3, .x = 200, .y = 60});
    const auto baseline = decode(ram);
    require(baseline.size() == 3 && baseline[2].tile == 3 && baseline[0].x == 100 * 256, "baseline decode wrong");
    for (const auto &d : baseline) require(d.identity == 0, "null presentation produced identity");

    // Empty presentation is identical to nullptr.
    SpritePresentation empty;
    const auto with_empty = decode(ram, &empty);
    require(with_empty.size() == baseline.size(), "empty presentation changed count");
    for (size_t i = 0; i < baseline.size(); ++i)
        require(same_geometry(baseline[i], with_empty[i]) && !with_empty[i].identity, "empty presentation changed output");

    // Identity propagation for untouched entries.
    std::vector<uint64_t> ids(0x800, 0);
    ids[0] = 42; ids[2] = 44;
    SpritePresentation id_only; id_only.identity = ids;
    const auto ided = decode(ram, &id_only);
    require(ided.size() == 3 && ided[0].identity == 42 && ided[1].identity == 0 && ided[2].identity == 44 &&
            same_geometry(ided[0], baseline[0]) && same_geometry(ided[2], baseline[2]), "identity not propagated");

    // Replacement: a zoomed, chained 2-entry block (x/y continue by scale*16), with a jump word
    // inside the replacement that must be ignored.
    const Entry head{.tile = 9, .zoom = 0x0080, .x = 150, .y = 70, .cont = 0x08};
    const Entry tail{.tile = 10, .cont = 0xf0, .w6 = 0x8001};
    std::vector<uint8_t> replacement(32);
    write_entry(&replacement[0], head);
    write_entry(&replacement[16], tail);
    std::vector<uint8_t> real(ram.begin(), ram.begin() + 32);
    SpriteSplice splice;
    splice.first = 0; splice.last = 1; splice.real = real; splice.replacement = replacement; splice.identity = 0x1234;
    SpritePresentation spliced; spliced.splices = std::span<const SpriteSplice>(&splice, 1);
    const auto out = decode(ram, &spliced);
    require(out.size() == 3, "splice changed entry count wrongly (jump word not ignored?)");
    require(out[0].tile == 9 && out[1].tile == 10 && out[2].tile == 3, "splice replacement/continuation wrong");
    require(out[0].scale_x == 128 && out[1].scale_x == 128, "replacement zoom not applied");
    require(out[1].x == 150 * 256 + 128 * 16 && out[1].y == 70 * 256 + 256 * 16, "replacement chaining wrong");
    require(out[0].identity == sprite_identity_mix(0x1234, 0) && out[1].identity == sprite_identity_mix(0x1234, 1) &&
            out[0].identity != out[1].identity && out[2].identity == 0, "replacement identity wrong");
    require(same_geometry(out[2], baseline[2]), "entry after splice changed");

    // The same entries placed in real RAM decode to the same geometry (shared state machine).
    std::vector<uint8_t> direct(0x10000, 0);
    put_entry(direct, 0, 0, head);
    put_entry(direct, 0, 1, {.tile = 10, .cont = 0xf0});
    put_entry(direct, 0, 2, {.tile = 3, .x = 200, .y = 60});
    const auto reference = decode(direct);
    require(reference.size() == 3, "reference decode count");
    for (size_t i = 0; i < 3; ++i) require(same_geometry(reference[i], out[i]), "replacement differs from native decode");

    // Stale splice (real bytes no longer match) is ignored.
    std::vector<uint8_t> stale = ram;
    put_entry(stale, 0, 1, {.tile = 77, .x = 120, .y = 50});
    const auto stale_out = decode(stale, &spliced);
    require(stale_out.size() == 3 && stale_out[0].tile == 1 && stale_out[1].tile == 77 &&
            stale_out[0].identity == 0, "stale splice applied");

    // Splice for the other bank does not apply to the active bank.
    SpriteSplice other = splice; other.bank = true;
    SpritePresentation other_bank; other_bank.splices = std::span<const SpriteSplice>(&other, 1);
    require(decode(ram, &other_bank)[0].tile == 1, "other-bank splice applied");

    // Empty replacement removes the covered entries.
    SpriteSplice removal = splice; removal.replacement = {};
    SpritePresentation removed; removed.splices = std::span<const SpriteSplice>(&removal, 1);
    const auto removed_out = decode(ram, &removed);
    require(removed_out.size() == 1 && removed_out[0].tile == 3, "empty replacement did not remove entries");
}
void sprite_id(CapturedFrame &s, unsigned slot, int x, unsigned tile, uint64_t identity) {
    s.sprites[slot] = {x * 256, 20 * 256, 256, 256, tile, 0, false, false, identity};
    s.sprite_count = std::max(s.sprite_count, slot + 1);
}
void sprite_obj(CapturedFrame &s, unsigned slot, int x, unsigned tile, uint64_t identity, uint64_t object) {
    sprite_id(s, slot, x, tile, identity);
    s.sprites[slot].object = object;
}
}
int main() {
    try {
        decode_checks();
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
        // Identity-matched sprites: scale/flip changes reject, unchanged appearance interpolates.
        // A tile change interpolates only when it agrees with its object's rigid delta; with no
        // object it snaps (entry k may be another body part after a pose change).
        *a = CapturedFrame{}; a->fallback = false; sprite_id(*a, 0, 20, 10, 7);
        *b = *a; sprite_id(*b, 0, 24, 99, 7);
        stats = pair(*a, *b).stats;
        require(stats.sprites == 0 && stats.sprite_transform_rejections == 1, "objectless tile change interpolated");
        // Tile change agreeing with the object's same-tile entries interpolates.
        *a = CapturedFrame{}; a->fallback = false;
        sprite_obj(*a, 0, 20, 10, 1, 5); sprite_obj(*a, 1, 60, 11, 2, 5); sprite_obj(*a, 2, 100, 12, 3, 5);
        *b = CapturedFrame{}; b->fallback = false;
        sprite_obj(*b, 0, 24, 10, 1, 5); sprite_obj(*b, 1, 64, 11, 2, 5); sprite_obj(*b, 2, 104, 99, 3, 5);
        result = pair(*a, *b);
        require(result.stats.sprites == 3 && result.state.sprite_x[2] == 102 * 256 && !result.stats.rejected_sprites,
                "rigid-agreeing tile change did not interpolate");
        // Tile change disagreeing with the object (pose swap) snaps and counts as a transform rejection.
        sprite_obj(*b, 2, 110, 99, 3, 5);
        result = pair(*a, *b);
        require(result.stats.sprites == 2 && result.stats.sprite_transform_rejections == 1 &&
                result.state.sprite_x[2] == 110 * 256, "pose-swap tile change interpolated");
        // A tile change differing by more than 1 px snaps; within 1 px interpolates.
        sprite_obj(*b, 2, 105, 99, 3, 5);
        require(pair(*a, *b).stats.sprites == 3, "tile change within 1 px tolerance snapped");
        sprite_obj(*b, 2, 106, 99, 3, 5);
        require(pair(*a, *b).stats.sprites == 2, "tile change beyond 1 px tolerance interpolated");
        // A single-entry object with a tile change interpolates.
        *a = CapturedFrame{}; a->fallback = false; sprite_obj(*a, 0, 20, 10, 1, 5);
        *b = CapturedFrame{}; b->fallback = false; sprite_obj(*b, 0, 24, 99, 1, 5);
        result = pair(*a, *b);
        require(result.stats.sprites == 1 && result.state.sprite_x[0] == 22 * 256 && !result.stats.rejected_sprites,
                "single-entry tile change snapped");
        // Every entry tile-changed with no majority delta: no reference, all snap.
        sprite_obj(*a, 1, 60, 11, 2, 5); sprite_obj(*a, 2, 100, 12, 3, 5);
        sprite_obj(*b, 1, 68, 99, 2, 5); sprite_obj(*b, 2, 112, 98, 3, 5);
        stats = pair(*a, *b).stats;
        require(stats.sprites == 0 && stats.sprite_transform_rejections == 3, "majority-less tile changes interpolated");
        // Entries of another object do not contribute to the reference.
        sprite_obj(*a, 1, 60, 11, 2, 6); sprite_obj(*b, 1, 64, 11, 2, 6);
        sprite_obj(*b, 2, 104, 98, 3, 5);
        require(pair(*a, *b).stats.sprites == 3, "single-entry object after split snapped");
        *a = CapturedFrame{}; a->fallback = false; sprite_id(*a, 0, 20, 10, 7);
        *b = *a; sprite_id(*b, 0, 24, 10, 7);
        result = pair(*a, *b);
        require(result.stats.sprites == 1 && result.state.sprite_x[0] == 22 * 256 &&
            !result.stats.rejected_sprites, "identity-matched motion did not interpolate");
        sprite_id(*b, 0, 24, 10, 7);
        result = pair(*a, *b);
        require(result.stats.sprites == 1 && result.state.sprite_x[0] == 22 * 256 &&
            !result.stats.rejected_sprites, "identity-matched motion did not interpolate");
        sprite_id(*b, 0, 24, 10, 7); b->sprites[0].scale_x = 128;
        stats = pair(*a, *b).stats;
        require(stats.sprites == 0 && stats.sprite_transform_rejections == 1, "identity-matched zoom interpolated");
        // A vanished identity and a born identity with the same appearance within the window are the
        // same sprite re-keyed: the appearance fallback pairs them, and a single-entry object passes
        // the rigid check trivially, so it interpolates.
        sprite_obj(*a, 0, 20, 10, 7, 5);
        sprite_obj(*b, 0, 24, 10, 8, 5);
        result = pair(*a, *b);
        require(result.stats.sprites == 1 && result.state.sprite_x[0] == 22 * 256 &&
            !result.stats.rejected_sprites, "re-keyed identity did not pair by appearance");
        // A born identity with a different tile than the vanished one does not pair.
        sprite_obj(*b, 0, 24, 11, 8, 5);
        stats = pair(*a, *b).stats;
        require(stats.sprites == 0 && stats.sprite_identity_rejections == 1, "re-keyed identity with new tile paired");
        // A born identity beyond the 32 px window does not pair.
        sprite_obj(*b, 0, 60, 10, 8, 5);
        stats = pair(*a, *b).stats;
        require(stats.sprites == 0 && stats.sprite_jump_rejections == 1, "re-keyed identity beyond window paired");
        // Two equidistant vanished candidates for one birth snap (ambiguity).
        *a = CapturedFrame{}; a->fallback = false; sprite_obj(*a, 0, 20, 10, 7, 5); sprite_obj(*a, 1, 28, 10, 9, 5);
        *b = CapturedFrame{}; b->fallback = false; sprite_obj(*b, 0, 24, 10, 8, 5);
        stats = pair(*a, *b).stats;
        require(stats.sprites == 0 && stats.sprite_identity_rejections == 1, "equidistant re-key candidates paired");
        // A fallback match with object 0 has no reference and snaps.
        *a = CapturedFrame{}; a->fallback = false; sprite_id(*a, 0, 20, 10, 7);
        *b = CapturedFrame{}; b->fallback = false; sprite_id(*b, 0, 24, 10, 8);
        stats = pair(*a, *b).stats;
        require(stats.sprites == 0 && stats.sprite_transform_rejections == 1, "object-0 fallback interpolated");
        // Fallback matches of one object with disagreeing deltas: the minority snaps, the majority interpolates.
        *a = CapturedFrame{}; a->fallback = false;
        sprite_obj(*a, 0, 20, 10, 1, 5); sprite_obj(*a, 1, 60, 11, 2, 5); sprite_obj(*a, 2, 100, 12, 3, 5);
        *b = CapturedFrame{}; b->fallback = false;
        sprite_obj(*b, 0, 24, 10, 11, 5); sprite_obj(*b, 1, 64, 11, 12, 5); sprite_obj(*b, 2, 110, 12, 13, 5);
        result = pair(*a, *b);
        require(result.stats.sprites == 2 && result.stats.sprite_transform_rejections == 1 &&
                result.state.sprite_x[0] == 22 * 256 && result.state.sprite_x[1] == 62 * 256 &&
                result.state.sprite_x[2] == 110 * 256, "fallback minority did not snap with majority interpolating");
        // A 50/50 split has no reference: all snap.
        *a = CapturedFrame{}; a->fallback = false;
        sprite_obj(*a, 0, 20, 10, 1, 5); sprite_obj(*a, 1, 60, 11, 2, 5);
        *b = CapturedFrame{}; b->fallback = false;
        sprite_obj(*b, 0, 24, 10, 11, 5); sprite_obj(*b, 1, 70, 11, 12, 5);
        stats = pair(*a, *b).stats;
        require(stats.sprites == 0 && stats.sprite_transform_rejections == 2, "50/50 fallback split interpolated");
        *a = CapturedFrame{}; a->fallback = false; sprite_id(*a, 0, 20, 10, 7);
        // Identity-less vs identified (either direction) never pairs, even at identical appearance.
        *b = CapturedFrame{}; b->fallback = false;
        sprite_id(*b, 0, 24, 10, 0);
        stats = pair(*a, *b).stats;
        require(stats.sprites == 0 && stats.sprite_identity_rejections == 1, "identity vs none paired");
        stats = pair(*b, *a).stats;
        require(stats.sprites == 0 && stats.sprite_identity_rejections == 1, "none vs identity paired");
        // Identity-only change with identical geometry is not motion.
        *b = *a; b->sprites[0].identity = 9;
        stats = pair(*a, *b).stats;
        require(!stats.sprites && !stats.moving_sprites && !stats.rejected_sprites, "identity-only change counted");
        // Duplicate identities are ambiguous and snap.
        *a = CapturedFrame{}; a->fallback = false; sprite_id(*a, 0, 20, 10, 7); sprite_id(*a, 1, 60, 10, 7);
        *b = *a; sprite_id(*b, 0, 24, 10, 7); sprite_id(*b, 1, 64, 10, 7);
        stats = pair(*a, *b).stats;
        require(stats.sprites == 0, "duplicate identities interpolated");
        // Sprites without identity keep (tile, palette) + nearest matching.
        *a = CapturedFrame{}; a->fallback = false; sprite(*a, 0, 20);
        *b = *a; sprite(*b, 0, 24, 99);
        stats = pair(*a, *b).stats;
        require(stats.sprites == 0 && stats.sprite_identity_rejections == 1, "no-identity tile change interpolated");
        sprite(*b, 0, 24);
        require(pair(*a, *b).stats.sprites == 1, "no-identity motion lost");
        layer(*a); *b = *a;
        pf0(*b).source_x += 4 * 256;
        row24(*b).blend = {8, 8, 8, 8};
        row24(*b).playfields[1].layer.enabled = true; row24(*b).playfields[1].layer.priority = 9;
        result = pair(*a, *b);
        require(result.stats.playfield_rows == 1 && result.state.rows[f3rt::geometry::first_line].source_x[0] == 102 * 256,
                "unrelated controls vetoed PF scroll");
        pf0(*b).source_y = 400;
        result = pair(*a, *b);
        require(result.state.rows[f3rt::geometry::first_line].source_x[0] == 102 * 256 && result.state.rows[f3rt::geometry::first_line].phase[0] == 400 * 256 &&
                result.stats.row_jump_rejections == 1, "Y jump vetoed valid X motion or failed to snap");
        pf0(*b).source_y = 104; pf0(*b).y_step = 128;
        result = pair(*a, *b);
        require(result.state.rows[f3rt::geometry::first_line].source_x[0] == 102 * 256 && result.state.rows[f3rt::geometry::first_line].phase[0] == 104 * 256 &&
                result.stats.row_transform_rejections == 1, "Y zoom vetoed valid X motion or failed to snap");
        pf0(*b).y_step = 256; pf0(*a).source_x = 1022 * 256; pf0(*b).source_x = 2 * 256;
        result = pair(*a, *b);
        require(result.state.rows[f3rt::geometry::first_line].source_x[0] == 2 * 256 && result.state.rows[f3rt::geometry::first_line].phase[0] == 102 * 256,
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
        require(result.state.rows[f3rt::geometry::first_line].source_x[0] == 100 * 256 + 1 &&
                result.state.rows[f3rt::geometry::first_line].phase[0] == 101 * 256 + 0, "PF subunit rounding/phase lost");
        pf0(*b).layer.clip_enabled = 1; pf0(*b).layer.clip_inverse = true; row24(*b).clips[0] = {50, 300};
        result = pair(*a, *b);
        require(!result.stats.playfield_rows && result.stats.row_control_rejections == 1 &&
                result.state.rows[f3rt::geometry::first_line].phase[0] == 101 * 256 + 1, "own PF clip change interpolated");
        auto &text = row24(*a).text;
        text.enabled = true;
        row24(*a).text_x = 100; row24(*a).text_y = 100;
        *b = *a; row24(*b).text_x = 101; row24(*b).text_y = 101;
        result = pair(*a, *b);
        require(result.stats.text_rows == 1 && result.state.rows[f3rt::geometry::first_line].text_x == 100 * 256 + 128 &&
                result.state.rows[f3rt::geometry::first_line].text_y == 100 * 256 + 128, "fractional text midpoint lost");
        row24(*b).text.clip_enabled = 1;
        result = pair(*a, *b);
        require(!result.stats.text_rows && result.state.rows[f3rt::geometry::first_line].text_x == 101 * 256, "own text clip change interpolated");
        row24(*b).text.clip_enabled = 0;
        row24(*a).text_x = 511; row24(*b).text_x = 0;
        result = pair(*a, *b);
        require(result.state.rows[f3rt::geometry::first_line].text_x == 0 && result.state.rows[f3rt::geometry::first_line].text_y == 100 * 256 + 128,
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
