#include "f3rt/machine.hpp"
#include "f3rt/abi/audio.h"
#include "f3rt/game_video.hpp"
#include "f3rt/rom.hpp"
#include "f3rt/sprite_group.hpp"
#include "sprites/sprite_system.hpp"
#include "sprites/enhancements.hpp"
#include "renderer/game/flicker_hold.hpp"
#include "renderer/game/full_detail.hpp"
#include "renderer/decode.hpp"
#include "renderer/game/video.hpp"
#include "renderer/game/scene_snapshot.hpp"
#include "sprite_check.hpp"
#include "../../tools/commands/sprite_walk.hpp"
#include "runner.hpp"
#include "support.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using f3test::fixture;

namespace {
constexpr uint32_t SYNTH_DATA_OFFSET = 0x100;
constexpr uint32_t SYNTH_DATA = f3rt::RAM_BASE + SYNTH_DATA_OFFSET;

constexpr uint32_t OBJ_A = f3rt::RAM_BASE + 0x200;
constexpr uint32_t OBJ_B = f3rt::RAM_BASE + 0x280;
constexpr uint32_t GRP_PC_XY = 0x300;
constexpr uint32_t GRP_PC_Y = 0x304;
constexpr uint32_t GRP_PC_CLASS2 = 0x308;
constexpr uint32_t GRP_PC_A4 = 0x30c;
constexpr uint32_t GRP_PC_RESET = 0x400;
constexpr uint32_t GRP_PC_RESET2 = 0x404;
constexpr uint32_t GRP_PC_TILE = 0x500;
constexpr uint32_t SLOT = 4;

constexpr uint8_t BASE_A3 = 8 + 3, BASE_A4 = 8 + 4;
constexpr f3rt::SpriteGroupWrite SYNTH_WRITES[] = {
    {GRP_PC_XY, BASE_A3, uint8_t(f3rt::SpriteLane::XY), 1, 8, 0, 2},
    {GRP_PC_Y, BASE_A3, uint8_t(f3rt::SpriteLane::Y), 1, 8, 0xffff, 2},
    {GRP_PC_CLASS2, BASE_A3, uint8_t(f3rt::SpriteLane::XY), 2, 8, 0x10, 0x12},
    {GRP_PC_A4, BASE_A4, uint8_t(f3rt::SpriteLane::XY), 1, 8, 0, 2},
};
constexpr f3rt::SpriteGroupReset SYNTH_RESETS[] = {{GRP_PC_RESET, BASE_A3, 0, 0},
                                                   {GRP_PC_RESET, BASE_A3, 0, 0x10},
                                                   {GRP_PC_RESET2, BASE_A3, 0, 0}};
constexpr f3rt::SpriteGroupTable SYNTH_GROUP_TABLE{
    std::span<const f3rt::SpriteGroupWrite>(SYNTH_WRITES),
    std::span<const f3rt::SpriteGroupReset>(SYNTH_RESETS)};

constexpr f3rt::SpriteGroupWrite FLICKER_GROUP_WRITES[] = {
    {0x100, BASE_A3, uint8_t(f3rt::SpriteLane::XY), 2, 8, 0, 2},
    {0x104, BASE_A3, uint8_t(f3rt::SpriteLane::XY), 1, 8, 4, 6},
};
constexpr f3rt::SpriteGroupTable FLICKER_GROUPS{FLICKER_GROUP_WRITES, {}};

std::unique_ptr<f3rt::Machine> synth_machine() {
    auto m = std::make_unique<f3rt::Machine>(fixture());
    m->write16(SYNTH_DATA, 0x1234);
    m->cpu.sr = 0x2700;
    m->cpu.a[3] = SYNTH_DATA;
    return m;
}

std::unique_ptr<f3rt::Machine> group_machine() {
    auto m = std::make_unique<f3rt::Machine>(fixture());
    m->sprites = std::make_unique<f3rt::SpriteSystem>(*m, SYNTH_GROUP_TABLE);
    m->cpu.sr = 0x2700;
    m->cpu.a[3] = OBJ_A;
    m->cpu.a[4] = OBJ_B;
    return m;
}

void poke16(f3rt::Machine &m, uint32_t address, uint16_t value, uint32_t pc) {
    m.cpu.pc = pc;
    m.write16(address, value);
}

void poke32(f3rt::Machine &m, uint32_t address, uint32_t value, uint32_t pc) {
    m.cpu.pc = pc;
    m.write32(address, value);
}

void raw16(uint8_t *p, uint16_t value) {
    p[0] = uint8_t(value >> 8);
    p[1] = uint8_t(value);
}

void raw_entry(uint8_t *ram, uint16_t slot, uint16_t tile, uint16_t x, uint16_t y,
               uint16_t cont = 0) {
    uint8_t *p = ram + size_t(slot) * 16;
    std::memset(p, 0, 16);
    raw16(p + 0, tile);
    raw16(p + 2, 0);
    raw16(p + 4, x);
    raw16(p + 6, y);
    raw16(p + 8, cont);
}

uint32_t sprite_address(uint16_t slot, uint32_t offset) {
    return f3rt::GRAPHICS_BASE + uint32_t(slot) * 16 + offset;
}

void write_head(f3rt::Machine &m, uint16_t slot, uint16_t x, uint16_t y, uint16_t cont = 0) {
    poke16(m, sprite_address(slot, 0), 1, GRP_PC_TILE);
    poke32(m, sprite_address(slot, 4), uint32_t(x) << 16 | y, GRP_PC_XY);
    if (cont) poke16(m, sprite_address(slot, 8), cont, GRP_PC_TILE);
}

void write_member(f3rt::Machine &m, uint16_t slot, uint16_t cont = 0x5400) {
    poke16(m, sprite_address(slot, 0), 2, GRP_PC_TILE);
    poke16(m, sprite_address(slot, 8), cont, GRP_PC_TILE);
}

std::vector<f3rt::DecodedSpriteEntry> decode(const f3rt::Machine &m,
                                              const f3rt::SpritePresentation &presentation,
                                              f3rt::SpriteRamState &state) {
    std::array<f3rt::DecodedSpriteEntry, 64> out;
    const size_t n =
        f3rt::decode_sprite_list(m.graphics.data(), int(f3rt::geometry::FIRST_LINE),
                                 int(f3rt::geometry::HEIGHT), out, state, false, &presentation);
    return {out.begin(), out.begin() + std::ptrdiff_t(n)};
}
} // namespace

TEST(FlickerHold, UsesGameFramesAndWrapsTheCounter) {
    auto m = synth_machine();
    f3rt::SpriteGroupTracker tracker(*m, FLICKER_GROUPS);
    using Hold = f3rt::game_config::FlickerHold;
    constexpr Hold config = {"test", 2, 1, 2, Hold::Follow::Self};
    f3rt::FlickerHoldStage hold(config);
    f3rt::SpriteStage &stage = hold;
    const f3rt::SceneSprite shadow{
        .x = 20 * 256, .group = f3rt::pack_group_key(SYNTH_DATA, 2, 0, 0), .group_x = 10 * 256};
    const f3rt::SceneSprite body{.group = f3rt::pack_group_key(SYNTH_DATA, 1, 0, 0)};
    std::vector sprites{shadow, body};

    stage.present(sprites, f3rt::SpriteFrame{&tracker, 0xffff, false});
    EXPECT_FLOAT_EQ(sprites[0].blend_weight, 0.5f);
    EXPECT_FLOAT_EQ(sprites[1].blend_weight, 1.0f);
    m->write16(SYNTH_DATA, 12);
    for (int i = 0; i < 2; ++i) {
        sprites = {body};
        stage.present(sprites, f3rt::SpriteFrame{&tracker, 0, false});
        ASSERT_EQ(sprites.size(), 2u);
        EXPECT_EQ(sprites[0].group, shadow.group);
        EXPECT_EQ(sprites[0].x, 22 * 256);
        EXPECT_FLOAT_EQ(sprites[0].blend_weight, 0.5f);
    }
    sprites = {body};
    stage.present(sprites, f3rt::SpriteFrame{&tracker, 1, false});
    EXPECT_EQ(sprites.size(), 1u);
    std::ostringstream report;
    stage.report(report);
    EXPECT_NE(report.str().find("held_frames=2 drops=1 anchor_missing=0"), std::string::npos);
}

TEST(FlickerHold, FollowsAnchorAndDropsOnGenerationMismatch) {
    auto m = synth_machine();
    f3rt::SpriteGroupTracker tracker(*m, FLICKER_GROUPS);
    using Hold = f3rt::game_config::FlickerHold;
    constexpr Hold config = {"test", 2, 1, 2, Hold::Follow::Anchor};
    f3rt::FlickerHoldStage hold(config);
    f3rt::SpriteStage &stage = hold;
    const f3rt::SceneSprite shadow{
        .x = 20 * 256, .group = f3rt::pack_group_key(SYNTH_DATA, 2, 0, 0)};
    f3rt::SceneSprite body{
        .group = f3rt::pack_group_key(SYNTH_DATA, 1, 0, 0), .group_x = 70 * 256};
    std::vector sprites{shadow, body};

    stage.present(sprites, f3rt::SpriteFrame{&tracker, 4, false});
    body.group_x += 5 * 256;
    sprites = {body};
    stage.present(sprites, f3rt::SpriteFrame{&tracker, 5, false});
    ASSERT_EQ(sprites.size(), 2u);
    EXPECT_EQ(sprites[0].x, 25 * 256);
    body.group = f3rt::pack_group_key(SYNTH_DATA, 1, 1, 0);
    sprites = {body};
    stage.present(sprites, f3rt::SpriteFrame{&tracker, 5, false});
    EXPECT_EQ(sprites.size(), 1u);
    std::ostringstream report;
    stage.report(report);
    EXPECT_NE(report.str().find("held_frames=1 drops=1 anchor_missing=1"), std::string::npos);
}

TEST(EnhancementSet, EnabledByDefaultAndLastOverrideWins) {
    f3rt::EnhancementSet enhancements;
    for (const auto &entry : enhancements.entries()) {
        EXPECT_TRUE(enhancements.enabled(entry.name, f3rt::VideoBackend::Gpu));
        const std::string name(entry.name);
        enhancements.override_setting(name + "=off");
        EXPECT_FALSE(enhancements.enabled(entry.name, f3rt::VideoBackend::Gpu));
        enhancements.override_setting(name + "=on");
        EXPECT_TRUE(enhancements.enabled(entry.name, f3rt::VideoBackend::Gpu));
    }
    EXPECT_THROW(enhancements.override_setting("invalid"), std::invalid_argument);
    EXPECT_THROW(enhancements.override_setting("=on"), std::invalid_argument);
}

TEST(SpriteSystem, TableLookupResolvesKeyAndOrigin) {
    auto m = group_machine();
    poke16(*m, OBJ_A, 0x0100, GRP_PC_TILE);
    poke16(*m, OBJ_A + 2, 0x00f0, GRP_PC_TILE);
    write_head(*m, SLOT, 0x0020, 0x0030);

    const auto presentation = m->sprites->presentation();
    ASSERT_EQ(presentation.groups.size(), 0x800u) << "Groups are exposed per slot";
    const auto &slot = presentation.groups[SLOT];
    EXPECT_NE(slot.key, 0u) << "A tabled X/Y write resolves a group";
    EXPECT_EQ(slot.x, 0x0100 << 8) << "Origin X is the root in 24.8";
    EXPECT_EQ(slot.y, 0x00f0 << 8) << "Origin Y is the root in 24.8";
    const auto &untouched = presentation.groups[SLOT + 1];
    EXPECT_EQ(untouched.key, 0u);
    EXPECT_EQ(untouched.pc, 0u);
    EXPECT_EQ(untouched.x, 0);
    EXPECT_EQ(untouched.y, 0);
}

TEST(SpriteSystem, ResetBumpsGenerationAndChangesKey) {
    auto m = group_machine();
    poke16(*m, OBJ_A, 0x0100, GRP_PC_TILE);
    poke16(*m, OBJ_A + 2, 0x00f0, GRP_PC_TILE);
    write_head(*m, SLOT, 0x0020, 0x0030);
    const uint64_t first = m->sprites->presentation().groups[SLOT].key;
    ASSERT_NE(first, 0u);

    poke16(*m, OBJ_A, 0x0101, GRP_PC_TILE);
    write_head(*m, SLOT, 0x0020, 0x0030);
    EXPECT_EQ(m->sprites->presentation().groups[SLOT].key, first)
        << "Rewriting the same record keeps the group key";

    poke16(*m, OBJ_A, 0x0800, GRP_PC_RESET);
    write_head(*m, SLOT, 0x0020, 0x0030);
    const auto &slot = m->sprites->presentation().groups[SLOT];
    EXPECT_NE(slot.key, first) << "A reset write changes the key";
    EXPECT_EQ(slot.x, 0x0800 << 8) << "The origin follows the reset root";
}

TEST(SpriteSystem, DifferentClassesAtTheSameAddressDiffer) {
    auto m = group_machine();
    poke16(*m, OBJ_A, 0x0100, GRP_PC_TILE);
    poke16(*m, OBJ_A + 2, 0x00f0, GRP_PC_TILE);
    write_head(*m, SLOT, 0x0020, 0x0030);
    const uint64_t class1 = m->sprites->presentation().groups[SLOT].key;

    poke16(*m, OBJ_A + 0x10, 0x0200, GRP_PC_TILE);
    poke16(*m, OBJ_A + 0x12, 0x0210, GRP_PC_TILE);
    poke32(*m, sprite_address(SLOT, 4), 0x0020u << 16 | 0x0030, GRP_PC_CLASS2);
    const uint64_t class2 = m->sprites->presentation().groups[SLOT].key;
    EXPECT_NE(class2, class1) << "Two root-field classes never share a key";
}

TEST(SpriteSystem, DistinctBaseAddressesNeverShareAKey) {
    auto m = group_machine();
    for (const uint32_t base : {OBJ_A, OBJ_B}) {
        poke16(*m, base, 0x0100, GRP_PC_TILE);
        poke16(*m, base + 2, 0x00f0, GRP_PC_TILE);
    }
    write_head(*m, SLOT, 0x0020, 0x0030);
    const uint64_t a = m->sprites->presentation().groups[SLOT].key;
    poke32(*m, sprite_address(SLOT, 4), 0x0020u << 16 | 0x0030, GRP_PC_A4);
    const uint64_t b = m->sprites->presentation().groups[SLOT].key;
    ASSERT_NE(a, 0u);
    ASSERT_NE(b, 0u);
    EXPECT_NE(a, b) << "Equal root values under different records must not share a key";
}

TEST(SpriteSystem, UnresolvedWriteLeavesGroupZero) {
    auto m = group_machine();
    poke16(*m, OBJ_A, 0x0100, GRP_PC_TILE);
    write_head(*m, SLOT, 0x0020, 0x0030);
    ASSERT_NE(m->sprites->presentation().groups[SLOT].key, 0u);
    poke16(*m, sprite_address(SLOT, 4), 0x0040, GRP_PC_TILE);
    EXPECT_EQ(m->sprites->presentation().groups[SLOT].key, 0u)
        << "A writer outside the table is unresolved";
}

TEST(SpriteSystem, BlockMembersInheritTheHeadGroup) {
    auto m = group_machine();
    poke16(*m, OBJ_A, 0x0040, GRP_PC_TILE);
    poke16(*m, OBJ_A + 2, 0x0050, GRP_PC_TILE);
    write_head(*m, SLOT, 0x0040, 0x0050, 0x0800);
    write_member(*m, uint16_t(SLOT + 1), 0x5400);

    const auto presentation = m->sprites->presentation();
    ASSERT_EQ(presentation.groups[SLOT + 1].key, 0u) << "Members carry no group of their own";
    f3rt::SpriteRamState state;
    const auto decoded = decode(*m, presentation, state);
    ASSERT_EQ(decoded.size(), 2u) << "Head and member are drawn";
    EXPECT_NE(decoded[0].group, 0u);
    EXPECT_EQ(decoded[1].group, decoded[0].group) << "A block member inherits the head's group";
    EXPECT_EQ(decoded[1].group_x, decoded[0].group_x);
    EXPECT_EQ(decoded[1].group_y, decoded[0].group_y);
}

TEST(SpriteSystem, PeriodicResetsDoNotRebirthTheGroup) {
    auto m = group_machine();
    poke16(*m, OBJ_A + 2, 0x00f0, GRP_PC_TILE);
    m->frame = 1;
    poke16(*m, OBJ_A, 0x0100, GRP_PC_RESET);
    write_head(*m, SLOT, 0x0020, 0x0030);
    const uint64_t key = m->sprites->presentation().groups[SLOT].key;
    ASSERT_NE(key, 0u);

    m->frame = 2;
    poke16(*m, OBJ_A, 0x0101, GRP_PC_RESET);
    write_head(*m, SLOT, 0x0020, 0x0030);
    EXPECT_EQ(m->sprites->presentation().groups[SLOT].key, key)
        << "A reset repeated on consecutive frames keeps the key";

    m->frame = 3;
    poke16(*m, OBJ_A, 0x0102, GRP_PC_TILE);
    write_head(*m, SLOT, 0x0020, 0x0030);
    EXPECT_EQ(m->sprites->presentation().groups[SLOT].key, key);

    m->frame = 4;
    poke16(*m, OBJ_A, 0x0103, GRP_PC_RESET);
    write_head(*m, SLOT, 0x0020, 0x0030);
    EXPECT_NE(m->sprites->presentation().groups[SLOT].key, key)
        << "A reset after a gap starts a new generation";

    m->frame = 5;
    poke16(*m, OBJ_A, 0x0104, GRP_PC_TILE);
    m->frame = 6;
    poke16(*m, OBJ_A, 0x0105, GRP_PC_RESET);
    write_head(*m, SLOT, 0x0020, 0x0030);
    const auto new_key = m->sprites->presentation().groups[SLOT].key;
    poke16(*m, OBJ_A, 0x0106, GRP_PC_RESET2);
    write_head(*m, SLOT, 0x0020, 0x0030);
    EXPECT_EQ(m->sprites->presentation().groups[SLOT].key, new_key)
        << "Two reset writers in one frame bump once";
}

TEST(SpriteSystem, DecoderDoesNotLeakBlockGroupAcrossAnUnresolvedHead) {
    std::array<uint8_t, 0x10000> ram{};
    raw_entry(ram.data(), 0, 5, 100, 50, 0x0800);
    raw_entry(ram.data(), 1, 6, 0, 0, 0x5400);
    raw_entry(ram.data(), 2, 7, 200, 60, 0x0800);
    raw_entry(ram.data(), 3, 8, 0, 0, 0x5400);

    std::array<f3rt::SpriteGroupSlot, 0x800> slots{};
    slots[0] = {0x7777, 100 << 8, 50 << 8};
    f3rt::SpritePresentation presentation;
    presentation.groups = slots;

    std::array<f3rt::DecodedSpriteEntry, 8> out;
    f3rt::SpriteRamState state;
    const size_t n = f3rt::decode_sprite_list(ram.data(), int(f3rt::geometry::FIRST_LINE),
                                              int(f3rt::geometry::HEIGHT), out, state, false,
                                              &presentation);
    ASSERT_EQ(n, 4u);
    EXPECT_EQ(out[0].group, 0x7777u);
    EXPECT_EQ(out[1].group, 0x7777u);
    EXPECT_EQ(out[2].group, 0u);
    EXPECT_EQ(out[3].group, 0u) << "An unresolved block must not inherit the previous block";
}

TEST(SpriteSystem, RomMotionGroupPairing) {
    const char *rom_env = std::getenv("F3_ROM_DIR");
#ifdef F3RT_DEFAULT_ROM_DIR
    if (!rom_env) rom_env = F3RT_DEFAULT_ROM_DIR;
#endif
    if (!rom_env || !*rom_env) GTEST_SKIP() << "F3_ROM_DIR not set";
    const std::filesystem::path rom_dir(rom_env);
    if (!std::filesystem::exists(rom_dir)) GTEST_SKIP() << "ROM directory does not exist";
    auto m = f3rt::runner::create_strict_machine(rom_dir, F3RT_DEFAULT_SET, true);
    if (!m->game_video) GTEST_SKIP() << "Game has no scene video";
    m->sprites = std::make_unique<f3rt::SpriteSystem>(*m, f3rt::SpriteSystem::game_table());

    uint64_t frames = 1500;
    if (const char *env = std::getenv("MOTION_GROUP_FRAMES")) frames = std::stoull(env);
    auto history = f3rt::rust_video::new_motion_history({1, 0});
    std::unordered_map<uint64_t, std::pair<int32_t, int32_t>> previous, current;
    std::unordered_set<uint64_t> previous_ambiguous, current_ambiguous;
    uint64_t paired_frames = 0, grouped = 0, paired_group_sprites = 0, moving_grouped = 0,
             verified = 0, cross_paired = 0, max_error = 0, key_changed = 0, key_change_moved = 0,
             rejected_group = 0, runs = 0;
    for (uint64_t f = 0; f < frames; ++f) {
        if (!m->run_frame() || m->cpu.halted) break;
        std::array<int16_t, 8192> audio;
        while (f3rt_audio_render(audio.data(), audio.size() / 2) != 0) {
        }
        const f3rt::SceneSnapshot &scene = m->game_video->scene_snapshot();
        ++runs;
        current.clear();
        current_ambiguous.clear();
        for (unsigned i = 0; i < scene.sprite_count; ++i) {
            const auto &sprite = scene.sprites[i];
            if (!sprite.group) continue;
            const auto [it, fresh] = current.try_emplace(sprite.group,
                                                         std::pair{sprite.group_x, sprite.group_y});
            if (!fresh && (it->second.first != sprite.group_x ||
                           it->second.second != sprite.group_y))
                current_ambiguous.insert(sprite.group);
        }
        history->capture(scene.inner(), f);
        const auto motion = history->apply(scene.inner(), 0.5f);
        if (motion.stats.paired) {
            ++paired_frames;
            rejected_group += motion.stats.sprite_group_rejections;
            for (unsigned i = 0; i < scene.sprite_count; ++i) {
                const auto &sprite = scene.sprites[i];
                if (!sprite.group) continue;
                ++grouped;
                const auto prev = previous.find(sprite.group);
                const bool unique = prev != previous.end() &&
                                    !previous_ambiguous.contains(sprite.group) &&
                                    !current_ambiguous.contains(sprite.group);
                const bool moved = motion.sprite_x[i] != sprite.x ||
                                   motion.sprite_y[i] != sprite.y;
                if (!unique) {
                    ++key_changed;
                    if (moved) ++key_change_moved;
                    continue;
                }
                ++paired_group_sprites;
                if (!moved) continue;
                ++moving_grouped;
                const int32_t dx = sprite.group_x - prev->second.first;
                const int32_t dy = sprite.group_y - prev->second.second;
                const int32_t expected_x =
                    int32_t(std::llround(double(sprite.x) - double(dx) * 0.5));
                const int32_t expected_y =
                    int32_t(std::llround(double(sprite.y) - double(dy) * 0.5));
                const int32_t error =
                    std::max(std::abs(expected_x - motion.sprite_x[i]),
                             std::abs(expected_y - motion.sprite_y[i]));
                max_error = std::max<uint64_t>(max_error, uint64_t(error));
                if (error <= 1) ++verified;
                else ++cross_paired;
            }
        }
        previous = std::move(current);
        previous_ambiguous = std::move(current_ambiguous);
    }
    EXPECT_GT(paired_frames, 0u) << "No consecutive paired frame was captured";
    EXPECT_GT(paired_group_sprites, 0u) << "No sprite paired by group key";
    EXPECT_EQ(cross_paired, 0u) << "A paired sprite moved by a delta other than its group delta";
    EXPECT_EQ(key_change_moved, 0u) << "A new/ambiguous group key was interpolated instead of snapping";
}

#if defined(F3RT_GENERATED)

TEST(SpriteSystem, RomGroupCoverage) {
    const char *rom_env = std::getenv("F3_ROM_DIR");
#ifdef F3RT_DEFAULT_ROM_DIR
    if (!rom_env) rom_env = F3RT_DEFAULT_ROM_DIR;
#endif
    if (!rom_env || !*rom_env) {
        GTEST_SKIP() << "F3_ROM_DIR not set; skipping ROM-backed sprite group check";
    }
    const std::filesystem::path rom_dir(rom_env);
    if (!std::filesystem::exists(rom_dir)) {
        GTEST_SKIP() << "ROM directory does not exist: " << rom_dir;
    }
    if (f3rt::SpriteSystem::game_table().writes.empty()) {
        GTEST_SKIP() << "Configured game declares no sprite groups";
    }

    f3rt::runner::SpriteCheckArgs args;
    args.rom_dir = rom_dir;
    args.set = F3RT_DEFAULT_SET;
    args.frames = 6000;
    if (const char *frames_env = std::getenv("SPRITE_CHECK_FRAMES")) {
        args.frames = std::stoull(frames_env);
    }

    const auto res = f3rt::runner::run_sprite_check(args);
    EXPECT_TRUE(res.passed) << res.error;
    EXPECT_GT(res.frames, 0u);
    EXPECT_GT(res.group_entries, 0u) << "The generated table must group drawn sprites";
    EXPECT_GT(res.max_groups_frame, 0u);
    if (std::string_view(F3RT_DEFAULT_SET) == "commandw") {
        EXPECT_GT(res.presented_entries, res.group_entries)
            << "Command War full-detail stage must expand coarse LoD sprites";
    }
}

#endif

TEST(EnhancementSet, ListPrintsKindAndName) {
    const f3rt::EnhancementSet enhancements;
    std::ostringstream actual, expected;
    for (const auto &entry : enhancements.entries())
        expected << entry.kind << ' ' << entry.name << '\n';
    enhancements.list(actual);
    EXPECT_EQ(actual.str(), expected.str());
}

namespace {
std::string render_report(const f3rt::EnhancementSet &enhancements, f3rt::VideoBackend backend) {
    std::ostringstream report;
    enhancements.render(backend).report(report);
    return report.str();
}
} // namespace

TEST(EnhancementSet, SelectionsFollowEnabledFlags) {
    f3rt::EnhancementSet enhancements;
    for (const auto &entry : enhancements.entries()) {
        std::ostringstream expected_render;
        if (std::string_view(F3RT_DEFAULT_SET) == "commandw" && entry.name != "full-detail") {
            expected_render << "full detail: frames=0 expanded_objects=0 expanded_sprites=0\n";
        }
        for (const auto &hold : f3rt::game_config::flicker_holds) {
            if (hold.name == entry.name) continue;
            expected_render << "flicker hold " << hold.name
                            << ": held_frames=0 drops=0 anchor_missing=0\n";
        }
        enhancements.set(entry.name, false);
        EXPECT_EQ(render_report(enhancements, f3rt::VideoBackend::Gpu), expected_render.str());
        enhancements.set(entry.name, true);
    }
}

TEST(EnhancementSet, BackendsMatchConfiguredStages) {
    const f3rt::EnhancementSet enhancements;
    const bool has_full_detail = std::string_view(F3RT_DEFAULT_SET) == "commandw";
    EXPECT_EQ(enhancements.render(f3rt::VideoBackend::Cpu).empty(), !has_full_detail);
    EXPECT_EQ(enhancements.render(f3rt::VideoBackend::Gpu).empty(),
              !has_full_detail && std::ranges::empty(f3rt::game_config::flicker_holds));
}

TEST(EnhancementSet, NamesAreUnique) {
    const f3rt::EnhancementSet enhancements;
    std::unordered_set<std::string_view> names;
    for (const auto &entry : enhancements.entries())
        EXPECT_TRUE(names.insert(entry.name).second) << entry.name;
}

TEST(RenderEnhancements, RunsStagesInOrderAndResets) {
    class Stage final : public f3rt::SpriteStage {
    public:
        explicit Stage(unsigned digit) : digit(digit) {}
        void present(std::vector<f3rt::SceneSprite> &sprites, const f3rt::SpriteFrame &) override {
            sprites.front().tile = sprites.front().tile * 10 + digit;
            ++frames;
        }
        void reset() override { frames = 0; }
        void report(std::ostream &out) const override { out << digit << '=' << frames << ' '; }

    private:
        unsigned digit;
        unsigned frames = 0;
    };

    std::vector<std::unique_ptr<f3rt::SpriteStage>> stages;
    stages.push_back(std::make_unique<Stage>(2));
    stages.push_back(std::make_unique<Stage>(3));
    f3rt::RenderEnhancements enhancements(std::move(stages));
    EXPECT_FALSE(enhancements.empty());
    const std::array latched{f3rt::SceneSprite{.tile = 1}};
    const f3rt::SpriteFrame frame{nullptr, 4, false};

    enhancements.present(latched, frame);
    ASSERT_EQ(enhancements.sprites().size(), 1u);
    EXPECT_EQ(enhancements.sprites().front().tile, 123u);
    EXPECT_EQ(latched.front().tile, 1u);
    std::ostringstream ran;
    enhancements.report(ran);
    EXPECT_EQ(ran.str(), "2=1 3=1 ");

    enhancements.present(latched, std::nullopt);
    EXPECT_TRUE(std::ranges::equal(enhancements.sprites(), latched));
    std::ostringstream reset;
    enhancements.report(reset);
    EXPECT_EQ(reset.str(), "2=0 3=0 ");

    enhancements.present(latched, frame);
    enhancements.reset();
    EXPECT_TRUE(enhancements.sprites().empty());
    std::ostringstream cleared;
    enhancements.report(cleared);
    EXPECT_EQ(cleared.str(), "2=0 3=0 ");
}

TEST(FlickerHold, FlipClearsHeldGroupsAndKeepsStats) {
    auto m = synth_machine();
    f3rt::SpriteGroupTracker tracker(*m, FLICKER_GROUPS);
    using Hold = f3rt::game_config::FlickerHold;
    f3rt::FlickerHoldStage hold({"test", 2, 1, 3, Hold::Follow::Anchor});
    f3rt::SpriteStage &stage = hold;
    const f3rt::SceneSprite shadow{.tile = 1, .group = f3rt::pack_group_key(SYNTH_DATA, 2, 0, 0)};
    const f3rt::SceneSprite body{.tile = 2, .group = f3rt::pack_group_key(SYNTH_DATA, 1, 0, 0)};
    std::vector sprites{shadow, body};
    stage.present(sprites, {&tracker, 4, false});
    sprites = {body};
    stage.present(sprites, {&tracker, 5, false});
    ASSERT_EQ(sprites.size(), 2u);
    sprites = {body};
    stage.present(sprites, {&tracker, 6, true});
    ASSERT_EQ(sprites.size(), 1u);
    EXPECT_EQ(sprites.front().tile, body.tile);
    std::ostringstream report;
    stage.report(report);
    EXPECT_EQ(report.str(), "flicker hold test: held_frames=1 drops=0 anchor_missing=0\n");
}

TEST(SpriteFrame, ReadsGameFrameCounter) {
    constexpr auto counter = f3rt::game_config::game_frame_counter;
    if (counter == 0) GTEST_SKIP() << "No configured game frame counter";
    auto m = synth_machine();
    const f3rt::SpriteGroupTracker tracker(*m, FLICKER_GROUPS);
    m->write16(counter, 0xfedc);
    const auto frame = f3rt::SpriteFrame::read(tracker, true);
    ASSERT_TRUE(frame);
    EXPECT_EQ(frame->groups, &tracker);
    EXPECT_EQ(frame->game_frame, 0xfedc);
    EXPECT_TRUE(frame->flipped);
}

namespace {

void sprite_words(std::vector<uint8_t> &ram, unsigned bank, unsigned slot, uint16_t w0, uint16_t w2,
                  uint16_t w3, uint16_t w4 = 0, uint16_t w5 = 0, uint16_t w6 = 0) {
    uint8_t *p = ram.data() + bank * f3rt::SPRITE_BANK_STRIDE + slot * 16;
    std::memset(p, 0, 16);
    raw16(p + 0, w0);
    raw16(p + 4, w2);
    raw16(p + 6, w3);
    raw16(p + 8, w4);
    raw16(p + 10, w5);
    raw16(p + 12, w6);
}

constexpr int VIS_Y = int(f3rt::geometry::FIRST_LINE), VIS_H = int(f3rt::geometry::HEIGHT);

std::vector<f3rt::DecodedSpriteEntry> renderer_list(const std::vector<uint8_t> &ram,
                                                    f3rt::SpriteRamState &state) {
    std::vector<f3rt::DecodedSpriteEntry> out(1024);
    out.resize(f3rt::decode_sprite_list(ram.data(), VIS_Y, VIS_H, out, state, true, nullptr));
    return out;
}

} // namespace

TEST(McpSpriteWalk, MatchesRendererOnBankJumpAndFlip) {
    std::vector<uint8_t> ram(f3rt::SPRITE_RAM_SIZE, 0);
    sprite_words(ram, 0, 0, 5, 100, 50);
    sprite_words(ram, 0, 1, 0, 0, 0x8000, 0, 0x0001);
    sprite_words(ram, 1, 2, 7, 200, 60, 0, 0, 0x8000 | 5);
    sprite_words(ram, 1, 5, 0, 0, 0x8000, 0, 0x2001);
    sprite_words(ram, 1, 6, 9, 100, 50);
    sprite_words(ram, 1, 7, 10, 200, 50, 0x0100);
    sprite_words(ram, 1, 8, 0, 0, 0, 0, 0, 0x8000 | 8);

    const f3rt::SpriteRamState start;
    const auto walk = f3rt::tool::walk_sprite_list(ram, start, 1024, VIS_Y, VIS_H);

    std::vector<unsigned> slots, banks;
    for (const auto &e : walk.entries) {
        slots.push_back(e.slot);
        banks.push_back(e.bank);
    }
    EXPECT_EQ(slots, (std::vector<unsigned>{0, 1, 2, 5, 6, 7, 8}));
    EXPECT_EQ(banks, (std::vector<unsigned>{0, 0, 1, 1, 1, 1, 1}));
    EXPECT_EQ(walk.visited, 7u);
    EXPECT_EQ(walk.commands, 2u);
    EXPECT_EQ(walk.jumps, 1u);
    EXPECT_EQ(walk.end_slot, 8);
    EXPECT_TRUE(walk.end_state.bank);
    EXPECT_TRUE(walk.end_state.flipscreen);

    const auto &e0 = walk.entries[0], &e2 = walk.entries[2], &e6 = walk.entries[4],
               &e7 = walk.entries[5];
    EXPECT_TRUE(e0.drawn);
    EXPECT_EQ(e0.x, 100 << 8);
    EXPECT_EQ(e2.jump, 5);
    EXPECT_TRUE(e2.drawn);
    EXPECT_EQ(e2.x, 200 << 8);
    EXPECT_FALSE(e6.drawn);
    EXPECT_TRUE(e6.culled);
    EXPECT_EQ(e6.raw_x, 100 << 8);
    EXPECT_EQ(e6.x, (512 - 16 - 100) << 8);
    EXPECT_TRUE(e7.drawn);
    EXPECT_FALSE(e7.flip_x);
    EXPECT_EQ(walk.drawn, 3u);
    EXPECT_EQ(walk.culled, 3u);

    f3rt::SpriteRamState state;
    const auto ref = renderer_list(ram, state);
    ASSERT_EQ(ref.size(), walk.drawn);
    size_t k = 0;
    for (const auto &e : walk.entries) {
        if (!e.drawn) continue;
        const auto &r = ref[k++];
        EXPECT_EQ(e.x, r.x) << "slot " << e.slot;
        EXPECT_EQ(e.y, r.y) << "slot " << e.slot;
        EXPECT_EQ(e.scale_x, r.scale_x);
        EXPECT_EQ(e.scale_y, r.scale_y);
        EXPECT_EQ(e.tile, r.tile);
        EXPECT_EQ(e.color, r.color);
        EXPECT_EQ(e.flip_x, r.flip_x);
        EXPECT_EQ(e.flip_y, r.flip_y);
    }
    EXPECT_EQ(state.bank, walk.end_state.bank);
    EXPECT_EQ(state.flipscreen, walk.end_state.flipscreen);
}

TEST(McpSpriteWalk, StartStateSelectsBankAndMirror) {
    std::vector<uint8_t> ram(f3rt::SPRITE_RAM_SIZE, 0);
    sprite_words(ram, 0, 0, 5, 100, 50);
    sprite_words(ram, 1, 0, 6, 200, 50);
    f3rt::SpriteRamState start;
    start.bank = true;
    start.flipscreen = true;
    const auto walk = f3rt::tool::walk_sprite_list(ram, start, 1, VIS_Y, VIS_H);
    ASSERT_EQ(walk.entries.size(), 1u);
    EXPECT_EQ(walk.entries[0].tile, 6u);
    EXPECT_EQ(walk.entries[0].bank, 1u);
    EXPECT_EQ(walk.entries[0].x, (512 - 16 - 200) << 8);
    f3rt::SpriteRamState reference = start;
    const auto ref = renderer_list(ram, reference);
    ASSERT_FALSE(ref.empty());
    EXPECT_EQ(ref.front().tile, 6u);
}

TEST(McpSpriteWalk, TraversalBudgetIsRendererBudget) {
    std::vector<uint8_t> ram(f3rt::SPRITE_RAM_SIZE, 0);
    sprite_words(ram, 0, 0, 5, 100, 50);
    sprite_words(ram, 0, 1, 0, 0, 0, 0, 0, 0x8000 | 0);
    const auto walk = f3rt::tool::walk_sprite_list(ram, {}, 100000, VIS_Y, VIS_H);
    EXPECT_EQ(walk.visited, f3rt::tool::SPRITE_WALK_BUDGET);
    EXPECT_EQ(walk.entries.size(), size_t(f3rt::tool::SPRITE_WALK_BUDGET));
    f3rt::SpriteRamState state;
    EXPECT_EQ(renderer_list(ram, state).size(), walk.drawn);
    EXPECT_EQ(f3rt::tool::walk_sprite_list(ram, {}, 4, VIS_Y, VIS_H).visited, 4u);
}
