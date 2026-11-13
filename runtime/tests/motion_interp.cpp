// rapidcheck uses std::current_exception without including <exception>; this must come first
// so the project's C++23 libc++ exposes a complete exception_ptr.
#include <exception>
#include "renderer/gpu/motion.hpp"
#include "renderer/decode.hpp"
#include "f3rt/memory_map.hpp"
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <vector>

namespace {

using f3rt::CapturedFrame;

// Fresh render slot: default transform, y = 20 px, canonical palette. Written field by field so
// render-only fields added to SceneSprite never disturb these helpers.
f3rt::SceneSprite &fresh(CapturedFrame &s, unsigned slot) {
    auto &sp = s.sprites[slot];
    sp = f3rt::SceneSprite{};
    sp.y = 20 * 256;
    s.sprite_count = std::max(s.sprite_count, slot + 1);
    return sp;
}

// Keyless sprite (no group): motion may only pair it by appearance.
void sprite(CapturedFrame &s, unsigned slot, int x, unsigned tile = 10) {
    auto &sp = fresh(s, slot);
    sp.x = x * 256;
    sp.tile = tile;
}

// Member of a rigid group: `group` key, group origin (gx, gy) in pixels, member at x.
f3rt::SceneSprite &grouped(CapturedFrame &s, unsigned slot, int x, unsigned tile, uint64_t group,
                           int gx, int gy) {
    auto &sp = fresh(s, slot);
    sp.x = x * 256;
    sp.tile = tile;
    sp.group = group;
    sp.group_x = gx * 256;
    sp.group_y = gy * 256;
    return sp;
}

f3rt::SceneRow &row24(CapturedFrame &s) { return s.rows[f3rt::geometry::FIRST_LINE]; }
f3rt::ScenePlayfield &pf0(CapturedFrame &s) { return row24(s).playfields[0]; }

void layer(CapturedFrame &s) {
    auto &p = pf0(s);
    p.layer.enabled = true;
    p.source_x = 100 * 256;
    p.source_y = 100;
    p.x_step = 256;
    p.y_step = 256;
}

f3rt::MotionResult pair(const CapturedFrame &a, const CapturedFrame &b, float alpha = 0.5f) {
    f3rt::GpuMotionHistory history;
    history.capture(a, 10);
    history.capture(b, 11);
    return history.apply(b, alpha);
}

void put16(uint8_t *p, uint16_t v) {
    p[0] = uint8_t(v >> 8);
    p[1] = uint8_t(v);
}

struct Entry {
    uint16_t tile = 0, zoom = 0, x = 0, y = 0, cont = 0, color = 0, w5 = 0, w6 = 0;
};

void write_entry(uint8_t *p, const Entry &e) {
    put16(p + 0, e.tile);
    put16(p + 2, e.zoom);
    put16(p + 4, e.x);
    put16(p + 6, e.y);
    put16(p + 8, uint16_t((e.cont << 8) | e.color));
    put16(p + 10, e.w5);
    put16(p + 12, e.w6);
}

void put_entry(std::vector<uint8_t> &ram, unsigned bank, unsigned index, const Entry &e) {
    write_entry(&ram[bank * f3rt::SPRITE_BANK_STRIDE + size_t(index) * 16], e);
}

std::vector<f3rt::DecodedSpriteEntry> decode(const std::vector<uint8_t> &ram,
                                             const f3rt::SpritePresentation *p = nullptr) {
    f3rt::SpriteRamState state;
    std::vector<f3rt::DecodedSpriteEntry> out(64);
    out.resize(f3rt::decode_sprite_list(ram.data(), int(f3rt::geometry::FIRST_LINE),
                                        int(f3rt::geometry::HEIGHT), out, state, false, p));
    return out;
}

bool same_geometry(const f3rt::DecodedSpriteEntry &a, const f3rt::DecodedSpriteEntry &b) {
    return a.x == b.x && a.y == b.y && a.scale_x == b.scale_x && a.scale_y == b.scale_y &&
           a.tile == b.tile && a.color == b.color && a.flip_x == b.flip_x && a.flip_y == b.flip_y &&
           a.pri == b.pri;
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
        EXPECT_EQ(d.group, 0u) << "null presentation produced a group";
        EXPECT_EQ(d.group_x, 0) << "null presentation produced a group origin";
    }

    // Empty presentation is identical to nullptr.
    f3rt::SpritePresentation empty;
    const auto with_empty = decode(ram, &empty);
    ASSERT_EQ(with_empty.size(), baseline.size()) << "empty presentation changed count";
    for (size_t i = 0; i < baseline.size(); ++i) {
        EXPECT_TRUE(same_geometry(baseline[i], with_empty[i]))
            << "empty presentation changed geometry";
        EXPECT_EQ(with_empty[i].group, 0u) << "empty presentation produced a group";
    }
}

TEST(MotionDecode, GroupPropagation) {
    std::vector<uint8_t> ram(0x10000, 0);
    // Entry 0 opens a hardware block, entry 1 is its member (inherits the group), entry 2 carries
    // its own group.
    put_entry(ram, 0, 0, {.tile = 1, .x = 100, .y = 50, .cont = 0x08});
    put_entry(ram, 0, 1, {.tile = 2, .x = 120, .y = 50, .cont = 0x08});
    put_entry(ram, 0, 2, {.tile = 3, .x = 200, .y = 60});
    const auto baseline = decode(ram);

    std::vector<f3rt::SpriteGroupSlot> slots(0x800);
    slots[0] = {42, 100 * 256, 50 * 256};
    slots[2] = {44, 200 * 256, 60 * 256};
    f3rt::SpritePresentation presented;
    presented.groups = slots;
    const auto grouped = decode(ram, &presented);
    ASSERT_EQ(grouped.size(), 3u) << "group propagation changed count";
    EXPECT_EQ(grouped[0].group, 42u) << "group not propagated to entry 0";
    EXPECT_EQ(grouped[0].group_x, 100 * 256) << "group origin x wrong on entry 0";
    EXPECT_EQ(grouped[0].group_y, 50 * 256) << "group origin y wrong on entry 0";
    EXPECT_EQ(grouped[1].group, 42u) << "block member did not inherit the head's group";
    EXPECT_EQ(grouped[1].group_x, 100 * 256) << "block member origin x wrong";
    EXPECT_EQ(grouped[2].group, 44u) << "group not propagated to entry 2";
    EXPECT_EQ(grouped[2].group_x, 200 * 256) << "group origin x wrong on entry 2";
    EXPECT_TRUE(same_geometry(grouped[0], baseline[0])) << "entry 0 geometry changed";
    EXPECT_TRUE(same_geometry(grouped[2], baseline[2])) << "entry 2 geometry changed";
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
    splice.first = 0;
    splice.last = 1;
    splice.real = real;
    splice.replacement = replacement;
    splice.group = 0x1234;
    splice.group_x = 150 * 256;
    splice.group_y = 70 * 256;
    f3rt::SpritePresentation spliced;
    spliced.splices = std::span<const f3rt::SpriteSplice>(&splice, 1);
    const auto out = decode(ram, &spliced);

    ASSERT_EQ(out.size(), 3u) << "splice changed entry count wrongly (jump word not ignored?)";
    EXPECT_EQ(out[0].tile, 9u) << "splice head tile wrong";
    EXPECT_EQ(out[1].tile, 10u) << "splice tail tile wrong";
    EXPECT_EQ(out[2].tile, 3u) << "post-splice tile wrong";
    EXPECT_EQ(out[0].scale_x, 128) << "replacement zoom not applied to entry 0";
    EXPECT_EQ(out[1].scale_x, 128) << "replacement zoom not applied to entry 1";
    EXPECT_EQ(out[1].x, 150 * 256 + 128 * 16) << "replacement chaining x wrong";
    EXPECT_EQ(out[1].y, 70 * 256 + 256 * 16) << "replacement chaining y wrong";
    EXPECT_EQ(out[0].group, 0x1234u) << "replacement group 0 wrong";
    EXPECT_EQ(out[1].group, 0x1234u) << "replacement group 1 wrong";
    EXPECT_EQ(out[0].group_x, 150 * 256) << "replacement group origin 0 wrong";
    EXPECT_EQ(out[1].group_x, 150 * 256) << "replacement group origin 1 wrong";
    EXPECT_EQ(out[2].group, 0u) << "post-splice group should be zero";
    EXPECT_TRUE(same_geometry(out[2], baseline[2])) << "entry after splice changed";

    // Native direct RAM decode matches spliced decode geometry.
    std::vector<uint8_t> direct(0x10000, 0);
    put_entry(direct, 0, 0, head);
    put_entry(direct, 0, 1, {.tile = 10, .cont = 0xf0});
    put_entry(direct, 0, 2, {.tile = 3, .x = 200, .y = 60});
    const auto reference = decode(direct);
    ASSERT_EQ(reference.size(), 3u) << "reference decode count wrong";
    for (size_t i = 0; i < 3; ++i) {
        EXPECT_TRUE(same_geometry(reference[i], out[i]))
            << "replacement differs from native decode";
    }

    // Stale splice is ignored when real RAM mismatches.
    std::vector<uint8_t> stale = ram;
    put_entry(stale, 0, 1, {.tile = 77, .x = 120, .y = 50});
    const auto stale_out = decode(stale, &spliced);
    ASSERT_EQ(stale_out.size(), 3u) << "stale splice changed count";
    EXPECT_EQ(stale_out[0].tile, 1u) << "stale splice replaced head tile";
    EXPECT_EQ(stale_out[1].tile, 77u) << "stale splice modified tail tile";
    EXPECT_EQ(stale_out[0].group, 0u) << "stale splice applied a group";

    // Splice for other bank does not apply to active bank.
    f3rt::SpriteSplice other = splice;
    other.bank = true;
    f3rt::SpritePresentation other_bank;
    other_bank.splices = std::span<const f3rt::SpriteSplice>(&other, 1);
    EXPECT_EQ(decode(ram, &other_bank)[0].tile, 1u) << "other-bank splice applied";

    // Empty replacement removes covered entries.
    f3rt::SpriteSplice removal = splice;
    removal.replacement = {};
    f3rt::SpritePresentation removed;
    removed.splices = std::span<const f3rt::SpriteSplice>(&removal, 1);
    const auto removed_out = decode(ram, &removed);
    ASSERT_EQ(removed_out.size(), 1u) << "empty replacement did not remove entries";
    EXPECT_EQ(removed_out[0].tile, 3u) << "empty replacement retained wrong entry";
}

// Every member of one rigid group moves by the group-origin delta, whatever its own offset.
TEST(MotionInterp, GroupMembersInterpolateTogether) {
    auto a = std::make_unique<CapturedFrame>();
    a->fallback = false;
    grouped(*a, 0, 20, 10, 1, 20, 20);
    grouped(*a, 1, 36, 11, 1, 20, 20);
    auto b = std::make_unique<CapturedFrame>(*a);
    grouped(*b, 0, 30, 10, 1, 30, 20);
    grouped(*b, 1, 46, 11, 1, 30, 20);

    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 2u) << "group members did not both interpolate";
    EXPECT_EQ(result.stats.moving_sprites, 2u) << "group motion not counted per member";
    EXPECT_EQ(result.stats.rejected_sprites, 0u) << "group motion rejected";
    EXPECT_EQ(result.stats.sprite_group_rejections, 0u) << "group pairing rejected";
    EXPECT_EQ(result.state.sprite_x[0], 25 * 256) << "member 0 midpoint wrong";
    EXPECT_EQ(result.state.sprite_x[1], 41 * 256) << "member 1 midpoint wrong";

    result = pair(*a, *b, 0.0f);
    EXPECT_EQ(result.state.sprite_x[0], 20 * 256) << "member 0 alpha 0 wrong";
    EXPECT_EQ(result.state.sprite_x[1], 36 * 256) << "member 1 alpha 0 wrong";
    result = pair(*a, *b, 1.0f);
    EXPECT_EQ(result.state.sprite_x[0], 30 * 256) << "member 0 alpha 1 wrong";
}

// Body and shadow are separate groups with independent origins. They deliberately share tile and
// palette: only the group key may pair them.
TEST(MotionInterp, BodyAndShadowGroupsInterpolateIndependently) {
    auto a = std::make_unique<CapturedFrame>();
    a->fallback = false;
    grouped(*a, 0, 20, 10, 1, 20, 20); // body
    grouped(*a, 1, 18, 10, 2, 18, 20); // shadow
    auto b = std::make_unique<CapturedFrame>(*a);
    grouped(*b, 0, 30, 10, 1, 30, 20);
    grouped(*b, 1, 14, 10, 2, 14, 20);

    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 2u) << "body and shadow did not interpolate";
    EXPECT_EQ(result.state.sprite_x[0], 25 * 256) << "body delta wrong";
    EXPECT_EQ(result.state.sprite_x[1], 16 * 256) << "shadow delta wrong (cross-paired?)";

    // Swapped slot order must not change the result: pairing is by key, not by slot.
    auto swapped = std::make_unique<CapturedFrame>(*b);
    std::swap(swapped->sprites[0], swapped->sprites[1]);
    result = pair(*a, *swapped);
    EXPECT_EQ(result.state.sprite_x[0], 16 * 256) << "shadow lost its own delta after swap";
    EXPECT_EQ(result.state.sprite_x[1], 25 * 256) << "body lost its own delta after swap";
}

// A birth or reset changes the group key: that group cannot pair and snaps.
TEST(MotionInterp, GroupKeyChangeSnaps) {
    auto a = std::make_unique<CapturedFrame>();
    a->fallback = false;
    grouped(*a, 0, 20, 10, 7, 20, 20);
    auto b = std::make_unique<CapturedFrame>(*a);
    grouped(*b, 0, 24, 10, 8, 24, 20);

    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 0u) << "changed group key interpolated";
    EXPECT_EQ(result.stats.sprite_group_rejections, 1u) << "changed group key not rejected";
    EXPECT_EQ(result.stats.rejected_sprites, 1u) << "changed group key not counted";
    EXPECT_EQ(result.state.sprite_x[0], 24 * 256) << "changed group key did not snap";
}

// One key with two origins in a frame is ambiguous and never pairs.
TEST(MotionInterp, AmbiguousGroupOriginSnaps) {
    auto a = std::make_unique<CapturedFrame>();
    a->fallback = false;
    grouped(*a, 0, 20, 10, 7, 20, 20);
    grouped(*a, 1, 60, 11, 7, 60, 20); // same key, different origin
    auto b = std::make_unique<CapturedFrame>();
    b->fallback = false;
    grouped(*b, 0, 24, 10, 7, 24, 20);

    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 0u) << "ambiguous previous group paired";
    EXPECT_EQ(result.stats.sprite_group_rejections, 1u) << "ambiguous previous group not rejected";
    EXPECT_EQ(result.state.sprite_x[0], 24 * 256) << "ambiguous previous group did not snap";

    // Ambiguity on the current side is just as unusable.
    auto c = std::make_unique<CapturedFrame>();
    c->fallback = false;
    grouped(*c, 0, 24, 10, 7, 24, 20);
    grouped(*c, 1, 64, 11, 7, 64, 20);
    *a = CapturedFrame{};
    a->fallback = false;
    grouped(*a, 0, 20, 10, 7, 20, 20);

    result = pair(*a, *c);
    EXPECT_EQ(result.stats.sprites, 0u) << "ambiguous current group paired";
    EXPECT_EQ(result.stats.sprite_group_rejections, 2u) << "ambiguous current members not rejected";
    EXPECT_EQ(result.state.sprite_x[0], 24 * 256) << "ambiguous current member 0 did not snap";
    EXPECT_EQ(result.state.sprite_x[1], 64 * 256) << "ambiguous current member 1 did not snap";
}

// LoD/flicker/skipped tiles change the member set without changing the group: the current members
// still interpolate with the group delta.
TEST(MotionInterp, MemberSetChangeInterpolates) {
    auto a = std::make_unique<CapturedFrame>();
    a->fallback = false;
    grouped(*a, 0, 20, 10, 3, 20, 20);
    grouped(*a, 1, 36, 11, 3, 20, 20);
    auto b = std::make_unique<CapturedFrame>(*a);
    b->sprite_count = 1; // one tile dropped
    grouped(*b, 0, 40, 11, 3, 24, 20);

    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 1u) << "shrunken member set did not interpolate";
    EXPECT_EQ(result.stats.rejected_sprites, 0u) << "shrunken member set rejected";
    EXPECT_EQ(result.state.sprite_x[0], 38 * 256) << "shrunken member set midpoint wrong";

    // Growing 1 -> 2 members: the new member has no previous counterpart and still interpolates.
    auto c = std::make_unique<CapturedFrame>();
    c->fallback = false;
    grouped(*c, 0, 36, 11, 3, 20, 20);
    auto d = std::make_unique<CapturedFrame>();
    d->fallback = false;
    grouped(*d, 0, 24, 10, 3, 24, 20);
    grouped(*d, 1, 40, 11, 3, 24, 20);
    result = pair(*c, *d);
    EXPECT_EQ(result.stats.sprites, 2u) << "grown member set did not interpolate";
    EXPECT_EQ(result.state.sprite_x[0], 22 * 256) << "new member midpoint wrong";
    EXPECT_EQ(result.state.sprite_x[1], 38 * 256) << "surviving member midpoint wrong";
}

// Zoom/flip is a whole-object change: the member offsets are no longer rigid, so the group snaps.
TEST(MotionInterp, GroupTransformChangeSnaps) {
    auto a = std::make_unique<CapturedFrame>();
    a->fallback = false;
    grouped(*a, 0, 20, 10, 9, 20, 20);
    auto b = std::make_unique<CapturedFrame>(*a);
    grouped(*b, 0, 24, 10, 9, 24, 20);
    b->sprites[0].scale_x = 128;

    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 0u) << "zoom change interpolated";
    EXPECT_EQ(result.stats.sprite_transform_rejections, 1u) << "zoom change not rejected";
    EXPECT_EQ(result.state.sprite_x[0], 24 * 256) << "zoom change did not snap";

    b->sprites[0].scale_x = 256;
    b->sprites[0].flip_y = true;
    result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 0u) << "flip change interpolated";
    EXPECT_EQ(result.stats.sprite_transform_rejections, 1u) << "flip change not rejected";

    // Previous members disagreeing on the transform signature make the group unusable.
    auto c = std::make_unique<CapturedFrame>();
    c->fallback = false;
    grouped(*c, 0, 20, 10, 9, 20, 20);
    auto &c1 = grouped(*c, 1, 36, 11, 9, 20, 20);
    c1.scale_y = 128;
    auto d = std::make_unique<CapturedFrame>();
    d->fallback = false;
    grouped(*d, 0, 24, 10, 9, 24, 20);

    result = pair(*c, *d);
    EXPECT_EQ(result.stats.sprites, 0u) << "mixed previous transforms interpolated";
    EXPECT_EQ(result.stats.sprite_transform_rejections, 1u)
        << "mixed previous transforms not rejected";
}

// Keyless sprites keep the appearance fallback: (tile, palette) with mutually unique nearest
// positions within 32 px.
TEST(MotionInterp, KeylessAppearanceFallback) {
    auto a = std::make_unique<CapturedFrame>();
    a->fallback = false;
    sprite(*a, 0, 20);
    auto b = std::make_unique<CapturedFrame>(*a);
    sprite(*b, 0, 24);

    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 1u) << "keyless motion lost";
    EXPECT_EQ(result.state.sprite_x[0], 22 * 256) << "keyless midpoint wrong";

    // Tile animation snaps; there is no identity to pair.
    sprite(*b, 0, 24, 99);
    auto stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "keyless tile change interpolated";
    EXPECT_EQ(stats.sprite_group_rejections, 1u) << "keyless tile change not rejected";

    // Equidistant duplicates are ambiguous and snap.
    *a = CapturedFrame{};
    a->fallback = false;
    sprite(*a, 0, 20);
    sprite(*a, 1, 24);
    *b = *a;
    sprite(*b, 0, 22);
    sprite(*b, 1, 26);
    stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "ambiguous keyless duplicates interpolated";
    EXPECT_EQ(stats.sprite_group_rejections, 2u) << "ambiguous keyless duplicates not rejected";

    // Dense but unambiguous duplicates still pair with their uniquely nearest partner.
    *a = CapturedFrame{};
    a->fallback = false;
    sprite(*a, 0, 20);
    sprite(*a, 1, 36);
    *b = *a;
    sprite(*b, 0, 24);
    sprite(*b, 1, 40);
    result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 2u) << "dense keyless motion lost";
    EXPECT_EQ(result.state.sprite_x[0], 22 * 256) << "dense keyless slot 0 midpoint wrong";
    EXPECT_EQ(result.state.sprite_x[1], 38 * 256) << "dense keyless slot 1 midpoint wrong";
}

// Without a group pair the sprite is drawn canonically at every alpha.
TEST(MotionInterp, NoGroupPairIsCanonicalAtEveryAlpha) {
    auto a = std::make_unique<CapturedFrame>();
    a->fallback = false;
    grouped(*a, 0, 20, 10, 1, 20, 20);
    auto b = std::make_unique<CapturedFrame>(*a);
    grouped(*b, 0, 24, 10, 2, 24, 20); // new key = birth

    for (float alpha : {0.0f, 0.25f, 0.5f, 1.0f}) {
        auto result = pair(*a, *b, alpha);
        EXPECT_TRUE(result.stats.paired) << "frames did not pair at alpha " << alpha;
        EXPECT_EQ(result.stats.sprites, 0u) << "unpaired group interpolated at alpha " << alpha;
        EXPECT_EQ(result.state.sprite_x[0], 24 * 256) << "unpaired group moved at alpha " << alpha;
    }
}

// The group-origin delta obeys the same 32 px per-axis limit as every other motion channel.
TEST(MotionInterp, GroupJumpLimit) {
    auto a = std::make_unique<CapturedFrame>();
    a->fallback = false;
    grouped(*a, 0, 20, 10, 5, 20, 20);
    auto b = std::make_unique<CapturedFrame>(*a);
    grouped(*b, 0, 52, 10, 5, 52, 20);

    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 1u) << "exact 32 px group delta rejected";
    EXPECT_EQ(result.state.sprite_x[0], 36 * 256) << "exact 32 px group midpoint wrong";

    grouped(*b, 0, 52, 10, 5, 52, 20);
    ++b->sprites[0].group_x; // one subunit beyond the limit
    auto stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "group delta beyond 32 px accepted";
    EXPECT_EQ(stats.sprite_jump_rejections, 1u) << "group jump rejection not recorded";
}

// A static grouped frame is neither motion nor a reject.
TEST(MotionInterp, StaticGroupedFrameHasNoMotionOrRejections) {
    auto a = std::make_unique<CapturedFrame>();
    a->fallback = false;
    grouped(*a, 0, 20, 10, 1, 20, 20);
    grouped(*a, 1, 36, 11, 1, 20, 20);

    auto stats = pair(*a, *a).stats;
    EXPECT_EQ(stats.sprites, 0u) << "static group counted as motion";
    EXPECT_EQ(stats.moving_sprites, 0u) << "static group counted as moving";
    EXPECT_EQ(stats.rejected_sprites, 0u) << "static group counted as rejected";
    EXPECT_EQ(stats.sprite_group_rejections, 0u) << "static group counted as rejected group";
}

TEST(MotionInterp, PlayfieldScrollAndIndependentGuards) {
    auto a = std::make_unique<CapturedFrame>();
    a->fallback = false;
    layer(*a);
    auto b = std::make_unique<CapturedFrame>(*a);
    pf0(*b).source_x += 4 * 256;
    row24(*b).blend = {8, 8, 8, 8};
    row24(*b).playfields[1].layer.enabled = true;
    row24(*b).playfields[1].layer.priority = 9;

    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.playfield_rows, 1u) << "unrelated controls vetoed PF scroll";
    EXPECT_EQ(result.state.rows[f3rt::geometry::FIRST_LINE].source_x[0], 102 * 256)
        << "PF scroll midpoint wrong";

    // PF Y jump snaps Y independently, keeps valid X motion.
    pf0(*b).source_y = 400;
    result = pair(*a, *b);
    EXPECT_EQ(result.state.rows[f3rt::geometry::FIRST_LINE].source_x[0], 102 * 256)
        << "Y jump vetoed valid X motion";
    EXPECT_EQ(result.state.rows[f3rt::geometry::FIRST_LINE].phase[0], 400 * 256)
        << "Y jump failed to snap to current frame";
    EXPECT_EQ(result.stats.row_jump_rejections, 1u) << "Y jump rejection not recorded";

    // PF Y zoom snaps Y independently, keeps valid X motion.
    pf0(*b).source_y = 104;
    pf0(*b).y_step = 128;
    result = pair(*a, *b);
    EXPECT_EQ(result.state.rows[f3rt::geometry::FIRST_LINE].source_x[0], 102 * 256)
        << "Y zoom vetoed valid X motion";
    EXPECT_EQ(result.state.rows[f3rt::geometry::FIRST_LINE].phase[0], 104 * 256)
        << "Y zoom failed to snap";
    EXPECT_EQ(result.stats.row_transform_rejections, 1u)
        << "Y zoom transform rejection not recorded";

    // PF X wrap snaps independently.
    pf0(*b).y_step = 256;
    pf0(*a).source_x = 1022 * 256;
    pf0(*b).source_x = 2 * 256;
    result = pair(*a, *b);
    EXPECT_EQ(result.state.rows[f3rt::geometry::FIRST_LINE].source_x[0], 2 * 256)
        << "X wrap failed to snap";
    EXPECT_EQ(result.state.rows[f3rt::geometry::FIRST_LINE].phase[0], 102 * 256)
        << "Y motion corrupted by X wrap";

    // Fixed-point jump boundary is inclusive, without integer rounding.
    sprite(*a, 0, 20);
    *b = *a;
    sprite(*b, 0, 52);
    result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 1u) << "exact 32px boundary rejected";
    EXPECT_EQ(result.state.sprite_x[0], 36 * 256) << "exact 32px boundary midpoint wrong";
    ++b->sprites[0].x;
    auto stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "32px plus subunit accepted";
    EXPECT_EQ(stats.sprite_jump_rejections, 1u) << "32px plus subunit should reject jump";

    // Palette replacement snaps.
    sprite(*b, 0, 24);
    ++b->sprites[0].palette;
    result = pair(*a, *b);
    EXPECT_EQ(result.stats.sprites, 0u) << "palette replacement interpolated";
    EXPECT_EQ(result.state.sprite_x[0], b->sprites[0].x) << "palette replacement failed to snap";

    // Pen mask change rejects.
    b->sprites[0].palette = a->sprites[0].palette;
    b->pen_mask = 63;
    stats = pair(*a, *b).stats;
    EXPECT_EQ(stats.sprites, 0u) << "pen mask change interpolated";
    EXPECT_EQ(stats.sprite_transform_rejections, 1u) << "pen mask change should reject transform";
    b->pen_mask = a->pen_mask;

    // PF Y phase is fractional; palette adjustment remains current discrete state.
    pf0(*a).source_x = 100 * 256;
    pf0(*a).source_y = 100;
    pf0(*a).y_fraction = 255;
    *b = *a;
    pf0(*b).source_x += 1;
    pf0(*b).source_y = 101;
    pf0(*b).y_fraction = 1;
    pf0(*b).palette_add = 0x1234;
    result = pair(*a, *b);
    EXPECT_EQ(result.state.rows[f3rt::geometry::FIRST_LINE].source_x[0], 100 * 256 + 1)
        << "PF X subunit lost";
    EXPECT_EQ(result.state.rows[f3rt::geometry::FIRST_LINE].phase[0], 101 * 256 + 0)
        << "PF fractional phase lost";

    // Clip change snaps.
    pf0(*b).layer.clip_enabled = 1;
    pf0(*b).layer.clip_inverse = true;
    row24(*b).clips[0] = {50, 300};
    result = pair(*a, *b);
    EXPECT_EQ(result.stats.playfield_rows, 0u) << "own PF clip change interpolated";
    EXPECT_EQ(result.stats.row_control_rejections, 1u)
        << "PF clip change should reject row control";
    EXPECT_EQ(result.state.rows[f3rt::geometry::FIRST_LINE].phase[0], 101 * 256 + 1)
        << "clipped PF phase wrong";
}

TEST(MotionInterp, TextScrollAndIndependentWrap) {
    auto a = std::make_unique<CapturedFrame>();
    a->fallback = false;
    auto &text = row24(*a).text;
    text.enabled = true;
    row24(*a).text_x = 100;
    row24(*a).text_y = 100;
    auto b = std::make_unique<CapturedFrame>(*a);
    row24(*b).text_x = 101;
    row24(*b).text_y = 101;

    auto result = pair(*a, *b);
    EXPECT_EQ(result.stats.text_rows, 1u) << "text scroll row missing";
    EXPECT_EQ(result.state.rows[f3rt::geometry::FIRST_LINE].text_x, 100 * 256 + 128)
        << "fractional text midpoint X lost";
    EXPECT_EQ(result.state.rows[f3rt::geometry::FIRST_LINE].text_y, 100 * 256 + 128)
        << "fractional text midpoint Y lost";

    // Own text clip change snaps.
    row24(*b).text.clip_enabled = 1;
    result = pair(*a, *b);
    EXPECT_EQ(result.stats.text_rows, 0u) << "own text clip change interpolated";
    EXPECT_EQ(result.state.rows[f3rt::geometry::FIRST_LINE].text_x, 101 * 256)
        << "snapped text X wrong";

    // Text wrap snaps independently.
    row24(*b).text.clip_enabled = 0;
    row24(*a).text_x = 511;
    row24(*b).text_x = 0;
    result = pair(*a, *b);
    EXPECT_EQ(result.state.rows[f3rt::geometry::FIRST_LINE].text_x, 0)
        << "text wrap failed to snap independently";
    EXPECT_EQ(result.state.rows[f3rt::geometry::FIRST_LINE].text_y, 100 * 256 + 128)
        << "text Y motion corrupted by X wrap";
}

TEST(MotionInterp, HistoryLifecycleAndDiscontinuity) {
    auto a = std::make_unique<CapturedFrame>();
    a->fallback = false;
    sprite(*a, 0, 20);
    auto b = std::make_unique<CapturedFrame>(*a);
    sprite(*b, 0, 24);

    f3rt::GpuMotionHistory history;
    history.capture(*a, 10);
    history.capture(*b, 11);

    for (float alpha : {-0.1f, 1.1f, std::numeric_limits<float>::infinity(),
                        std::numeric_limits<float>::quiet_NaN()}) {
        EXPECT_FALSE(history.apply(*b, alpha).stats.paired) << "invalid alpha paired: " << alpha;
    }

    history.capture(*b, 11);
    EXPECT_FALSE(history.apply(*b, 0.5f).stats.paired) << "duplicate frame paired";

    history.capture(*b, 13);
    EXPECT_FALSE(history.apply(*b, 0.5f).stats.paired) << "history gap paired";

    b->fallback = true;
    history.capture(*b, 14);
    b->fallback = false;
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

    CapturedFrame a;
    a.fallback = false;
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

// RapidCheck property: For any alpha in [0.0, 1.0], the interpolated position stays within the
// endpoint bounds.
RC_GTEST_PROP(MotionInterp, MonotonicInterpolationBounds, ()) {
    const int x0 = *rc::gen::inRange(-300, 300);
    const int dx = *rc::gen::inRange(-32, 33);
    const int alpha_step = *rc::gen::inRange(0, 101);
    const float alpha = float(alpha_step) / 100.0f;

    CapturedFrame a;
    a.fallback = false;
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

    CapturedFrame a;
    a.fallback = false;
    sprite(a, 0, x0);
    CapturedFrame b = a;
    sprite(b, 0, x0 + dx);

    const auto res = pair(a, b);
    RC_ASSERT(res.stats.paired);
    RC_ASSERT(res.stats.sprites == 0);
    RC_ASSERT(res.stats.sprite_jump_rejections == 1);
}
