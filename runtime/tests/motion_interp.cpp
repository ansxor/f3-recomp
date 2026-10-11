#include "renderer/gpu/motion.hpp"
#include "renderer/decode.hpp"
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace {

using f3rt::CapturedFrame;

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

f3rt::MotionResult pair(const CapturedFrame &a, const CapturedFrame &b, float alpha = 0.5f) {
    f3rt::GpuMotionHistory history;
    history.capture(a, 10); history.capture(b, 11);
    return history.apply(b, alpha);
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

void sprite_id(CapturedFrame &s, unsigned slot, int x, unsigned tile, uint64_t identity) {
    s.sprites[slot] = {x * 256, 20 * 256, 256, 256, tile, 0, false, false, identity};
    s.sprite_count = std::max(s.sprite_count, slot + 1);
}

void sprite_obj(CapturedFrame &s, unsigned slot, int x, unsigned tile, uint64_t identity, uint64_t object) {
    sprite_id(s, slot, x, tile, identity);
    s.sprites[slot].object = object;
}

} // namespace

TEST(MotionDecode, BaselineAndEmptyPresentation) {
    std::vector<uint8_t> ram(0x10000, 0);
    put_entry(ram, 0, 0, {.tile = 1, .x = 100, .y = 50});
    put_entry(ram, 0, 1, {.tile = 2, .x = 120, .y = 50});
    put_entry(ram, 0, 2, {.tile = 3, .x = 200, .y = 60});

    const auto baseline = decode(ram);
    ASSERT_EQ(baseline.size(), 3u) << "baseline decode wrong count";
    EXPECT_EQ(baseline[2].tile, 3u) << "baseline decode wrong tile";
    EXPECT_EQ(baseline[0].x, 100 * 256) << "baseline decode wrong x";
    for (const auto &d : baseline) {
        EXPECT_EQ(d.identity, 0u) << "null presentation produced identity";
    }

    // Empty presentation is identical to nullptr.
    f3rt::SpritePresentation empty;
    const auto with_empty = decode(ram, &empty);
    ASSERT_EQ(with_empty.size(), baseline.size()) << "empty presentation changed count";
    for (size_t i = 0; i < baseline.size(); ++i) {
        EXPECT_TRUE(same_geometry(baseline[i], with_empty[i])) << "empty presentation changed geometry";
        EXPECT_EQ(with_empty[i].identity, 0u) << "empty presentation produced identity";
    }
}

TEST(MotionDecode, IdentityPropagation) {
    std::vector<uint8_t> ram(0x10000, 0);
    put_entry(ram, 0, 0, {.tile = 1, .x = 100, .y = 50});
    put_entry(ram, 0, 1, {.tile = 2, .x = 120, .y = 50});
    put_entry(ram, 0, 2, {.tile = 3, .x = 200, .y = 60});
    const auto baseline = decode(ram);

    std::vector<uint64_t> ids(0x800, 0);
    ids[0] = 42; ids[2] = 44;
    f3rt::SpritePresentation id_only; id_only.identity = ids;
    const auto ided = decode(ram, &id_only);
    ASSERT_EQ(ided.size(), 3u) << "identity propagation changed count";
    EXPECT_EQ(ided[0].identity, 42u) << "identity not propagated to entry 0";
    EXPECT_EQ(ided[1].identity, 0u) << "unexpected identity on entry 1";
    EXPECT_EQ(ided[2].identity, 44u) << "identity not propagated to entry 2";
    EXPECT_TRUE(same_geometry(ided[0], baseline[0])) << "entry 0 geometry changed";
    EXPECT_TRUE(same_geometry(ided[2], baseline[2])) << "entry 2 geometry changed";
}

TEST(MotionDecode, SplicedReplacementAndChaining) {
    std::vector<uint8_t> ram(0x10000, 0);
    put_entry(ram, 0, 0, {.tile = 1, .x = 100, .y = 50});
    put_entry(ram, 0, 1, {.tile = 2, .x = 120, .y = 50});
    put_entry(ram, 0, 2, {.tile = 3, .x = 200, .y = 60});
    const auto baseline = decode(ram);

    const Entry head{.tile = 9, .zoom = 0x0080, .x = 150, .y = 70, .cont = 0x08};
    const Entry tail{.tile = 10, .cont = 0xf0, .w6 = 0x8001};
    std::vector<uint8_t> replacement(32);
    write_entry(&replacement[0], head);
    write_entry(&replacement[16], tail);
    std::vector<uint8_t> real(ram.begin(), ram.begin() + 32);
    f3rt::SpriteSplice splice;
    splice.first = 0; splice.last = 1; splice.real = real; splice.replacement = replacement; splice.identity = 0x1234;
    f3rt::SpritePresentation spliced; spliced.splices = std::span<const f3rt::SpriteSplice>(&splice, 1);
    const auto out = decode(ram, &spliced);

    ASSERT_EQ(out.size(), 3u) << "splice changed entry count wrongly (jump word not ignored?)";
    EXPECT_EQ(out[0].tile, 9u) << "splice head tile wrong";
    EXPECT_EQ(out[1].tile, 10u) << "splice tail tile wrong";
    EXPECT_EQ(out[2].tile, 3u) << "post-splice tile wrong";
    EXPECT_EQ(out[0].scale_x, 128) << "replacement zoom not applied to entry 0";
    EXPECT_EQ(out[1].scale_x, 128) << "replacement zoom not applied to entry 1";
    EXPECT_EQ(out[1].x, 150 * 256 + 128 * 16) << "replacement chaining x wrong";
    EXPECT_EQ(out[1].y, 70 * 256 + 256 * 16) << "replacement chaining y wrong";
    EXPECT_EQ(out[0].identity, f3rt::sprite_identity_mix(0x1234, 0)) << "replacement identity 0 wrong";
    EXPECT_EQ(out[1].identity, f3rt::sprite_identity_mix(0x1234, 1)) << "replacement identity 1 wrong";
    EXPECT_NE(out[0].identity, out[1].identity) << "replacement identities should be distinct";
    EXPECT_EQ(out[2].identity, 0u) << "post-splice identity should be zero";
    EXPECT_TRUE(same_geometry(out[2], baseline[2])) << "entry after splice changed";

    // Native direct RAM decode matches spliced decode geometry.
    std::vector<uint8_t> direct(0x10000, 0);
    put_entry(direct, 0, 0, head);
    put_entry(direct, 0, 1, {.tile = 10, .cont = 0xf0});
    put_entry(direct, 0, 2, {.tile = 3, .x = 200, .y = 60});
    const auto reference = decode(direct);
    ASSERT_EQ(reference.size(), 3u) << "reference decode count wrong";
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_TRUE(same_geometry(reference[i], out[i])) << "replacement differs from native decode";
    }

    // Stale splice is ignored when real RAM mismatches.
    std::vector<uint8_t> stale = ram;
    put_entry(stale, 0, 1, {.tile = 77, .x = 120, .y = 50});
    const auto stale_out = decode(stale, &spliced);
    ASSERT_EQ(stale_out.size(), 3u) << "stale splice changed count";
    EXPECT_EQ(stale_out[0].tile, 1u) << "stale splice replaced head tile";
    EXPECT_EQ(stale_out[1].tile, 77u) << "stale splice modified tail tile";
    EXPECT_EQ(stale_out[0].identity, 0u) << "stale splice applied identity";

    // Splice for other bank does not apply to active bank.
    f3rt::SpriteSplice other = splice; other.bank = true;
    f3rt::SpritePresentation other_bank; other_bank.splices = std::span<const f3rt::SpriteSplice>(&other, 1);
    EXPECT_EQ(decode(ram, &other_bank)[0].tile, 1u) << "other-bank splice applied";

    // Empty replacement removes covered entries.
    f3rt::SpriteSplice removal = splice; removal.replacement = {};
    f3rt::SpritePresentation removed; removed.splices = std::span<const f3rt::SpriteSplice>(&removal, 1);
    const auto removed_out = decode(ram, &removed);
    ASSERT_EQ(removed_out.size(), 1u) << "empty replacement did not remove entries";
    EXPECT_EQ(removed_out[0].tile, 3u) << "empty replacement retained wrong entry";
}

TEST(MotionInterp, SpriteCountShiftAndRemoval) {
    auto a = std::make_unique<CapturedFrame>(); a->fallback = false;
    sprite(*a, 0, 20); sprite(*a, 1, 80, 11);
    auto b = std::make_unique<CapturedFrame>(*a);

    // An unrelated insertion shifts both compact slots, but neither object identity.
    sprite(*b, 0, 150, 12); sprite(*b, 1, 24); sprite(*b, 2, 84, 11);
    auto result = pair(*a, *b);
    auto stats = result.stats;
    EXPECT_EQ(stats.sprites, 2u) << "count insertion lost stable motion";
    EXPECT_EQ(stats.moving_sprites, 2u) << "count insertion lost moving count";
    EXPECT_EQ(result.state.sprite_x[1], 22 * 256) << "shifted sprite midpoint wrong";

    stats = pair(*b, *a).stats;
    EXPECT_EQ(stats.sprites, 2u) << "count removal lost stable motion";
}

TEST(MotionInterp, AmbiguousDuplicatesSnap) {
    auto a = std::make_unique<CapturedFrame>(); a->fallback = false;
    sprite(*a, 0, 20); sprite(*a, 1, 24);
    auto b = std::make_unique<CapturedFrame>(*a);
    sprite(*b, 0, 22); sprite(*b, 1, 26);
    auto stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "ambiguous duplicates interpolated";
    EXPECT_EQ(stats.sprite_identity_rejections, 2u) << "ambiguous duplicates should reject";
}

TEST(MotionInterp, DenseUnambiguousDuplicateMovement) {
    auto a = std::make_unique<CapturedFrame>(); a->fallback = false;
    sprite(*a, 0, 20); sprite(*a, 1, 36);
    auto b = std::make_unique<CapturedFrame>(*a);
    sprite(*b, 0, 24); sprite(*b, 1, 40);
    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 2u) << "dense duplicate group lost uniquely nearest motion";
    EXPECT_EQ(result.state.sprite_x[0], 22 * 256) << "dense duplicate slot 0 midpoint wrong";
    EXPECT_EQ(result.state.sprite_x[1], 38 * 256) << "dense duplicate slot 1 midpoint wrong";
}

TEST(MotionInterp, TransformAndJumpRejections) {
    auto a = std::make_unique<CapturedFrame>(); a->fallback = false;
    sprite(*a, 0, 20);
    auto b = std::make_unique<CapturedFrame>(*a);
    sprite(*b, 0, 24);

    for (unsigned field : {2u, 6u}) {
        const auto original = b->sprites[0];
        if (field == 2) b->sprites[0].scale_x = 128; else b->sprites[0].flip_x = true;
        auto stats = pair(*a, *b).stats;
        EXPECT_EQ(stats.sprites, 0u) << "zoom/flip interpolated";
        EXPECT_EQ(stats.sprite_transform_rejections, 1u) << "zoom/flip rejection not recorded";
        b->sprites[0] = original;
    }

    sprite(*b, 0, 24, 99);
    auto stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "replacement tile interpolated";
    EXPECT_EQ(stats.sprite_identity_rejections, 1u) << "replacement tile rejection not recorded";

    sprite(*b, 0, 100);
    stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "large sprite jump interpolated";
    EXPECT_EQ(stats.sprite_jump_rejections, 1u) << "large jump rejection not recorded";
}

TEST(MotionInterp, StaticPairNoMovementOrRejections) {
    auto a = std::make_unique<CapturedFrame>(); a->fallback = false;
    sprite(*a, 0, 20);
    auto stats = pair(*a, *a).stats;
    EXPECT_EQ(stats.sprites, 0u) << "static sprite counted as motion";
    EXPECT_EQ(stats.moving_sprites, 0u) << "static sprite counted as moving";
    EXPECT_EQ(stats.rejected_sprites, 0u) << "static sprite counted as rejected";
}

TEST(MotionInterp, RigidAgreementAndPoseSwap) {
    auto a = std::make_unique<CapturedFrame>(); a->fallback = false;
    auto b = std::make_unique<CapturedFrame>(); b->fallback = false;

    // Objectless tile change snaps.
    sprite_id(*a, 0, 20, 10, 7);
    *b = *a; sprite_id(*b, 0, 24, 99, 7);
    auto stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "objectless tile change interpolated";
    EXPECT_EQ(stats.sprite_transform_rejections, 1u) << "objectless tile change should reject transform";

    // Tile change agreeing with object rigid delta interpolates.
    *a = CapturedFrame{}; a->fallback = false;
    sprite_obj(*a, 0, 20, 10, 1, 5); sprite_obj(*a, 1, 60, 11, 2, 5); sprite_obj(*a, 2, 100, 12, 3, 5);
    *b = CapturedFrame{}; b->fallback = false;
    sprite_obj(*b, 0, 24, 10, 1, 5); sprite_obj(*b, 1, 64, 11, 2, 5); sprite_obj(*b, 2, 104, 99, 3, 5);
    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 3u) << "rigid-agreeing tile change did not interpolate";
    EXPECT_EQ(result.state.sprite_x[2], 102 * 256) << "rigid-agreeing midpoint wrong";
    EXPECT_EQ(result.stats.rejected_sprites, 0u) << "rigid-agreeing tile change produced rejections";

    // Pose-swap tile change (disagreeing delta) snaps.
    sprite_obj(*b, 2, 110, 99, 3, 5);
    result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 2u) << "pose-swap tile change interpolated";
    EXPECT_EQ(result.stats.sprite_transform_rejections, 1u) << "pose-swap should count as transform rejection";
    EXPECT_EQ(result.state.sprite_x[2], 110 * 256) << "snapped sprite position should equal current frame";

    // Tile change within 1 px tolerance interpolates; beyond 1 px snaps.
    sprite_obj(*b, 2, 105, 99, 3, 5);
    EXPECT_EQ(pair(*a, *b).stats.sprites, 3u) << "tile change within 1 px tolerance snapped";
    sprite_obj(*b, 2, 106, 99, 3, 5);
    EXPECT_EQ(pair(*a, *b).stats.sprites, 2u) << "tile change beyond 1 px tolerance interpolated";

    // Single-entry object with tile change interpolates.
    *a = CapturedFrame{}; a->fallback = false; sprite_obj(*a, 0, 20, 10, 1, 5);
    *b = CapturedFrame{}; b->fallback = false; sprite_obj(*b, 0, 24, 99, 1, 5);
    result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 1u) << "single-entry tile change snapped";
    EXPECT_EQ(result.state.sprite_x[0], 22 * 256) << "single-entry midpoint wrong";
    EXPECT_EQ(result.stats.rejected_sprites, 0u) << "single-entry produced rejections";
}

TEST(MotionInterp, ReindexedGridAndAppearanceTwin) {
    auto a = std::make_unique<CapturedFrame>(); a->fallback = false;
    auto b = std::make_unique<CapturedFrame>(); b->fallback = false;

    // Re-indexed zoomed grid stays static.
    sprite_obj(*a, 0, 0, 10, 1, 5); sprite_obj(*a, 1, 16, 11, 2, 5);
    a->sprites[0].y = 0; a->sprites[1].y = 16 * 256;
    sprite_obj(*b, 0, 16, 11, 1, 5); sprite_obj(*b, 1, 0, 10, 2, 5);
    b->sprites[0].y = 16 * 256; b->sprites[1].y = 0;
    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 0u) << "re-indexed grid moved";
    EXPECT_EQ(result.stats.moving_sprites, 0u) << "re-indexed grid reported moving sprites";
    EXPECT_EQ(result.stats.rejected_sprites, 0u) << "re-indexed grid reported rejected sprites";
    EXPECT_EQ(result.state.sprite_x[0], 16 * 256) << "re-indexed grid position 0 wrong";
    EXPECT_EQ(result.state.sprite_x[1], 0) << "re-indexed grid position 1 wrong";

    // Demoted pair with static twin pairs with twin.
    *a = CapturedFrame{}; a->fallback = false;
    sprite_obj(*a, 0, 20, 10, 1, 5); sprite_obj(*a, 1, 40, 11, 2, 5);
    *b = CapturedFrame{}; b->fallback = false;
    sprite_obj(*b, 0, 40, 11, 1, 5); sprite_obj(*b, 1, 44, 12, 2, 5);
    result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 0u) << "demoted pair beat its static twin";
    EXPECT_EQ(result.state.sprite_x[0], 40 * 256) << "demoted pair position wrong";

    // Majority-less tile change: all snap.
    *a = CapturedFrame{}; a->fallback = false; sprite_obj(*a, 0, 20, 10, 1, 5);
    *b = CapturedFrame{}; b->fallback = false; sprite_obj(*b, 0, 24, 99, 1, 5);
    sprite_obj(*a, 1, 60, 11, 2, 5); sprite_obj(*a, 2, 100, 12, 3, 5);
    sprite_obj(*b, 1, 68, 99, 2, 5); sprite_obj(*b, 2, 112, 98, 3, 5);
    auto stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "majority-less tile changes interpolated";
    EXPECT_EQ(stats.sprite_transform_rejections, 3u) << "expected 3 transform rejections";

    // Entries of another object do not contribute to reference.
    sprite_obj(*a, 1, 60, 11, 2, 6); sprite_obj(*b, 1, 64, 11, 2, 6);
    sprite_obj(*b, 2, 104, 98, 3, 5);
    EXPECT_EQ(pair(*a, *b).stats.sprites, 3u) << "single-entry object after split snapped";
}

TEST(MotionInterp, IdentityMatchedMotionAndZoomRejection) {
    auto a = std::make_unique<CapturedFrame>(); a->fallback = false;
    auto b = std::make_unique<CapturedFrame>(); b->fallback = false;

    sprite_id(*a, 0, 20, 10, 7);
    *b = *a; sprite_id(*b, 0, 24, 10, 7);
    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 1u) << "identity-matched motion did not interpolate";
    EXPECT_EQ(result.state.sprite_x[0], 22 * 256) << "identity-matched midpoint wrong";
    EXPECT_EQ(result.stats.rejected_sprites, 0u) << "identity-matched motion produced rejections";

    // Scale change rejects.
    b->sprites[0].scale_x = 128;
    auto stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "identity-matched zoom interpolated";
    EXPECT_EQ(stats.sprite_transform_rejections, 1u) << "zoom transform rejection not recorded";
}

TEST(MotionInterp, RekeyedIdentityAndAppearanceFallback) {
    auto a = std::make_unique<CapturedFrame>(); a->fallback = false;
    auto b = std::make_unique<CapturedFrame>(); b->fallback = false;

    // Vanished identity and born identity with same appearance within window.
    sprite_obj(*a, 0, 20, 10, 7, 5);
    sprite_obj(*b, 0, 24, 10, 8, 5);
    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 1u) << "re-keyed identity did not pair by appearance";
    EXPECT_EQ(result.state.sprite_x[0], 22 * 256) << "re-keyed identity midpoint wrong";
    EXPECT_EQ(result.stats.rejected_sprites, 0u) << "re-keyed appearance match had rejections";

    // Born identity with different tile does not pair.
    sprite_obj(*b, 0, 24, 11, 8, 5);
    auto stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "re-keyed identity with new tile paired";
    EXPECT_EQ(stats.sprite_identity_rejections, 1u) << "tile change should reject identity";

    // Born identity beyond 32 px window does not pair.
    sprite_obj(*b, 0, 60, 10, 8, 5);
    stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "re-keyed identity beyond window paired";
    EXPECT_EQ(stats.sprite_jump_rejections, 1u) << "beyond window should reject jump";

    // Equidistant vanished candidates snap.
    *a = CapturedFrame{}; a->fallback = false; sprite_obj(*a, 0, 20, 10, 7, 5); sprite_obj(*a, 1, 28, 10, 9, 5);
    *b = CapturedFrame{}; b->fallback = false; sprite_obj(*b, 0, 24, 10, 8, 5);
    stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "equidistant re-key candidates paired";
    EXPECT_EQ(stats.sprite_identity_rejections, 1u) << "equidistant candidates should reject";

    // Fallback match with object 0 has no reference and snaps.
    *a = CapturedFrame{}; a->fallback = false; sprite_id(*a, 0, 20, 10, 7);
    *b = CapturedFrame{}; b->fallback = false; sprite_id(*b, 0, 24, 10, 8);
    stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "object-0 fallback interpolated";
    EXPECT_EQ(stats.sprite_transform_rejections, 1u) << "object-0 should reject transform";

    // Fallback matches with disagreeing deltas: minority snaps, majority interpolates.
    *a = CapturedFrame{}; a->fallback = false;
    sprite_obj(*a, 0, 20, 10, 1, 5); sprite_obj(*a, 1, 60, 11, 2, 5); sprite_obj(*a, 2, 100, 12, 3, 5);
    *b = CapturedFrame{}; b->fallback = false;
    sprite_obj(*b, 0, 24, 10, 11, 5); sprite_obj(*b, 1, 64, 11, 12, 5); sprite_obj(*b, 2, 110, 12, 13, 5);
    result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 2u) << "fallback minority did not snap";
    EXPECT_EQ(result.stats.sprite_transform_rejections, 1u) << "minority transform rejection missing";
    EXPECT_EQ(result.state.sprite_x[0], 22 * 256) << "majority 0 midpoint wrong";
    EXPECT_EQ(result.state.sprite_x[1], 62 * 256) << "majority 1 midpoint wrong";
    EXPECT_EQ(result.state.sprite_x[2], 110 * 256) << "minority snapped position wrong";

    // 50/50 split has no reference: all snap.
    *a = CapturedFrame{}; a->fallback = false;
    sprite_obj(*a, 0, 20, 10, 1, 5); sprite_obj(*a, 1, 60, 11, 2, 5);
    *b = CapturedFrame{}; b->fallback = false;
    sprite_obj(*b, 0, 24, 10, 11, 5); sprite_obj(*b, 1, 70, 11, 12, 5);
    stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "50/50 fallback split interpolated";
    EXPECT_EQ(stats.sprite_transform_rejections, 2u) << "50/50 should reject both transforms";
}

TEST(MotionInterp, MixedIdentityVsNoIdentity) {
    auto a = std::make_unique<CapturedFrame>(); a->fallback = false;
    auto b = std::make_unique<CapturedFrame>(); b->fallback = false;

    sprite_id(*a, 0, 20, 10, 7);
    sprite_id(*b, 0, 24, 10, 0);
    auto stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "identity vs none paired";
    EXPECT_EQ(stats.sprite_identity_rejections, 1u) << "identity vs none should reject";
    stats = pair(*b, *a).stats;
    EXPECT_EQ(stats.sprites, 0u) << "none vs identity paired";
    EXPECT_EQ(stats.sprite_identity_rejections, 1u) << "none vs identity should reject";

    // Identity-only change with identical geometry is not motion.
    *b = *a; b->sprites[0].identity = 9;
    stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "identity-only change counted as motion";
    EXPECT_EQ(stats.moving_sprites, 0u) << "identity-only change counted as moving";
    EXPECT_EQ(stats.rejected_sprites, 0u) << "identity-only change counted as rejected";

    // Duplicate identities are ambiguous and snap.
    *a = CapturedFrame{}; a->fallback = false; sprite_id(*a, 0, 20, 10, 7); sprite_id(*a, 1, 60, 10, 7);
    *b = *a; sprite_id(*b, 0, 24, 10, 7); sprite_id(*b, 1, 64, 10, 7);
    stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "duplicate identities interpolated";

    // Sprites without identity keep (tile, palette) + nearest matching.
    *a = CapturedFrame{}; a->fallback = false; sprite(*a, 0, 20);
    *b = *a; sprite(*b, 0, 24, 99);
    stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "no-identity tile change interpolated";
    EXPECT_EQ(stats.sprite_identity_rejections, 1u) << "no-identity tile change should reject";
    sprite(*b, 0, 24);
    EXPECT_EQ(pair(*a, *b).stats.sprites, 1u) << "no-identity motion lost";
}

TEST(MotionInterp, PlayfieldScrollAndIndependentGuards) {
    auto a = std::make_unique<CapturedFrame>(); a->fallback = false;
    layer(*a);
    auto b = std::make_unique<CapturedFrame>(*a);
    pf0(*b).source_x += 4 * 256;
    row24(*b).blend = {8, 8, 8, 8};
    row24(*b).playfields[1].layer.enabled = true; row24(*b).playfields[1].layer.priority = 9;

    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.playfield_rows, 1u) << "unrelated controls vetoed PF scroll";
    EXPECT_EQ(result.state.rows[f3rt::geometry::first_line].source_x[0], 102 * 256) << "PF scroll midpoint wrong";

    // PF Y jump snaps Y independently, keeps valid X motion.
    pf0(*b).source_y = 400;
    result = pair(*a, *b);
    EXPECT_EQ(result.state.rows[f3rt::geometry::first_line].source_x[0], 102 * 256) << "Y jump vetoed valid X motion";
    EXPECT_EQ(result.state.rows[f3rt::geometry::first_line].phase[0], 400 * 256) << "Y jump failed to snap to current frame";
    EXPECT_EQ(result.stats.row_jump_rejections, 1u) << "Y jump rejection not recorded";

    // PF Y zoom snaps Y independently, keeps valid X motion.
    pf0(*b).source_y = 104; pf0(*b).y_step = 128;
    result = pair(*a, *b);
    EXPECT_EQ(result.state.rows[f3rt::geometry::first_line].source_x[0], 102 * 256) << "Y zoom vetoed valid X motion";
    EXPECT_EQ(result.state.rows[f3rt::geometry::first_line].phase[0], 104 * 256) << "Y zoom failed to snap";
    EXPECT_EQ(result.stats.row_transform_rejections, 1u) << "Y zoom transform rejection not recorded";

    // PF X wrap snaps independently.
    pf0(*b).y_step = 256; pf0(*a).source_x = 1022 * 256; pf0(*b).source_x = 2 * 256;
    result = pair(*a, *b);
    EXPECT_EQ(result.state.rows[f3rt::geometry::first_line].source_x[0], 2 * 256) << "X wrap failed to snap";
    EXPECT_EQ(result.state.rows[f3rt::geometry::first_line].phase[0], 102 * 256) << "Y motion corrupted by X wrap";

    // Fixed-point jump boundary is inclusive, without integer rounding.
    sprite(*a, 0, 20); *b = *a; sprite(*b, 0, 52);
    result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 1u) << "exact 32px boundary rejected";
    EXPECT_EQ(result.state.sprite_x[0], 36 * 256) << "exact 32px boundary midpoint wrong";
    ++b->sprites[0].x;
    auto stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "32px plus subunit accepted";
    EXPECT_EQ(stats.sprite_jump_rejections, 1u) << "32px plus subunit should reject jump";

    // Palette replacement snaps.
    sprite(*b, 0, 24); ++b->sprites[0].palette;
    result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 0u) << "palette replacement interpolated";
    EXPECT_EQ(result.state.sprite_x[0], b->sprites[0].x) << "palette replacement failed to snap";

    // Pen mask change rejects.
    b->sprites[0].palette = a->sprites[0].palette; b->pen_mask = 63;
    stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "pen mask change interpolated";
    EXPECT_EQ(stats.sprite_transform_rejections, 1u) << "pen mask change should reject transform";
    b->pen_mask = a->pen_mask;

    // PF Y phase is fractional; palette adjustment remains current discrete state.
    pf0(*a).source_x = 100 * 256; pf0(*a).source_y = 100; pf0(*a).y_fraction = 255;
    *b = *a; pf0(*b).source_x += 1; pf0(*b).source_y = 101; pf0(*b).y_fraction = 1;
    pf0(*b).palette_add = 0x1234;
    result = pair(*a, *b);
    EXPECT_EQ(result.state.rows[f3rt::geometry::first_line].source_x[0], 100 * 256 + 1) << "PF X subunit lost";
    EXPECT_EQ(result.state.rows[f3rt::geometry::first_line].phase[0], 101 * 256 + 0) << "PF fractional phase lost";

    // Clip change snaps.
    pf0(*b).layer.clip_enabled = 1; pf0(*b).layer.clip_inverse = true; row24(*b).clips[0] = {50, 300};
    result = pair(*a, *b);
    EXPECT_EQ(result.stats.playfield_rows, 0u) << "own PF clip change interpolated";
    EXPECT_EQ(result.stats.row_control_rejections, 1u) << "PF clip change should reject row control";
    EXPECT_EQ(result.state.rows[f3rt::geometry::first_line].phase[0], 101 * 256 + 1) << "clipped PF phase wrong";
}

TEST(MotionInterp, TextScrollAndIndependentWrap) {
    auto a = std::make_unique<CapturedFrame>(); a->fallback = false;
    auto &text = row24(*a).text;
    text.enabled = true;
    row24(*a).text_x = 100; row24(*a).text_y = 100;
    auto b = std::make_unique<CapturedFrame>(*a);
    row24(*b).text_x = 101; row24(*b).text_y = 101;

    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.text_rows, 1u) << "text scroll row missing";
    EXPECT_EQ(result.state.rows[f3rt::geometry::first_line].text_x, 100 * 256 + 128) << "fractional text midpoint X lost";
    EXPECT_EQ(result.state.rows[f3rt::geometry::first_line].text_y, 100 * 256 + 128) << "fractional text midpoint Y lost";

    // Own text clip change snaps.
    row24(*b).text.clip_enabled = 1;
    result = pair(*a, *b);
    EXPECT_EQ(result.stats.text_rows, 0u) << "own text clip change interpolated";
    EXPECT_EQ(result.state.rows[f3rt::geometry::first_line].text_x, 101 * 256) << "snapped text X wrong";

    // Text wrap snaps independently.
    row24(*b).text.clip_enabled = 0;
    row24(*a).text_x = 511; row24(*b).text_x = 0;
    result = pair(*a, *b);
    EXPECT_EQ(result.state.rows[f3rt::geometry::first_line].text_x, 0) << "text wrap failed to snap independently";
    EXPECT_EQ(result.state.rows[f3rt::geometry::first_line].text_y, 100 * 256 + 128) << "text Y motion corrupted by X wrap";
}

TEST(MotionInterp, HistoryLifecycleAndDiscontinuity) {
    auto a = std::make_unique<CapturedFrame>(); a->fallback = false;
    sprite(*a, 0, 20);
    auto b = std::make_unique<CapturedFrame>(*a);
    sprite(*b, 0, 24);

    f3rt::GpuMotionHistory history;
    history.capture(*a, 10); history.capture(*b, 11);

    for (float alpha : {-0.1f, 1.1f, std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
        EXPECT_FALSE(history.apply(*b, alpha).stats.paired) << "invalid alpha paired: " << alpha;
    }

    history.capture(*b, 11);
    EXPECT_FALSE(history.apply(*b, 0.5f).stats.paired) << "duplicate frame paired";

    history.capture(*b, 13);
    EXPECT_FALSE(history.apply(*b, 0.5f).stats.paired) << "history gap paired";

    b->fallback = true; history.capture(*b, 14); b->fallback = false;
    history.capture(*b, 15);
    EXPECT_FALSE(history.apply(*b, 0.5f).stats.paired) << "fallback recovery paired too early";

    history.capture(*b, 16);
    EXPECT_TRUE(history.apply(*b, 0.5f).stats.paired) << "fallback recovery never resumed pairing";

    history.reset();
    EXPECT_FALSE(history.apply(*b, 0.5f).stats.paired) << "reset retained pairing";
}

// RapidCheck property: At alpha=0.0 and alpha=1.0, motion interpolation matches endpoints exactly.
RC_GTEST_PROP(MotionInterp, EndpointIdentity, ()) {
    const int x0 = *rc::gen::inRange(-500, 500);
    const int dx = *rc::gen::inRange(-32, 33);
    RC_PRE(dx != 0);

    CapturedFrame a; a.fallback = false;
    sprite(a, 0, x0);
    CapturedFrame b = a;
    sprite(b, 0, x0 + dx);

    f3rt::GpuMotionHistory history;
    history.capture(a, 10);
    history.capture(b, 11);

    const auto res0 = history.apply(b, 0.0f);
    RC_ASSERT(res0.stats.paired);
    RC_ASSERT(res0.stats.sprites == 1);
    RC_ASSERT(res0.state.sprite_x[0] == x0 * 256);

    const auto res1 = history.apply(b, 1.0f);
    RC_ASSERT(res1.stats.paired);
    RC_ASSERT(res1.stats.sprites == 1);
    RC_ASSERT(res1.state.sprite_x[0] == (x0 + dx) * 256);
}

// RapidCheck property: For any alpha in [0.0, 1.0], interpolated position stays within endpoint bounds.
RC_GTEST_PROP(MotionInterp, MonotonicInterpolationBounds, ()) {
    const int x0 = *rc::gen::inRange(-300, 300);
    const int dx = *rc::gen::inRange(-32, 33);
    const int alpha_step = *rc::gen::inRange(0, 101);
    const float alpha = float(alpha_step) / 100.0f;

    CapturedFrame a; a.fallback = false;
    sprite(a, 0, x0);
    CapturedFrame b = a;
    sprite(b, 0, x0 + dx);

    f3rt::GpuMotionHistory history;
    history.capture(a, 10);
    history.capture(b, 11);

    const auto res = history.apply(b, alpha);
    RC_ASSERT(res.stats.paired);
    if (dx == 0) {
        RC_ASSERT(res.state.sprite_x[0] == x0 * 256);
    } else {
        const int32_t min_x = std::min(x0, x0 + dx) * 256;
        const int32_t max_x = std::max(x0, x0 + dx) * 256;
        RC_ASSERT(res.state.sprite_x[0] >= min_x);
        RC_ASSERT(res.state.sprite_x[0] <= max_x);
    }
}

// RapidCheck property: Sprite displacement strictly exceeding 32px is rejected as a jump.
RC_GTEST_PROP(MotionInterp, JumpThresholdRejection, ()) {
    const int x0 = *rc::gen::inRange(-300, 300);
    const int sign = *rc::gen::element(1, -1);
    const int jump_px = *rc::gen::inRange(33, 100);
    const int dx = sign * jump_px;

    CapturedFrame a; a.fallback = false;
    sprite(a, 0, x0);
    CapturedFrame b = a;
    sprite(b, 0, x0 + dx);

    const auto res = pair(a, b);
    RC_ASSERT(res.stats.paired);
    RC_ASSERT(res.stats.sprites == 0);
    RC_ASSERT(res.stats.sprite_jump_rejections == 1);
}
