#include "f3rt/machine.hpp"
#include "f3rt/abi/audio.h"
#include "f3rt/game_video.hpp"
#include "f3rt/rom.hpp"
#include "f3rt/sprite_group.hpp"
#include "state_io.hpp"
#include "sprites/sprite_system.hpp"
#include "sprites/behaviour.hpp"
#include "debug/discovery.hpp"
#include "renderer/decode.hpp"
#include "renderer/gpu/motion.hpp"
#include "sprite_check.hpp"
#include "runner.hpp"
#include "support.hpp"
#include <gtest/gtest.h>
#include <exception>
#include <rapidcheck/gtest.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#if defined(F3RT_GENERATED)
#include "program.h"
#endif

using f3test::fixture;

namespace {
// Synthetic emit-unit code layout: block entry PCs and resume PCs (fake code, not game addresses).
constexpr uint32_t UNIT0_BLOCK0 = 0x100;
constexpr uint32_t UNIT0_BLOCK1 = 0x104;
constexpr uint32_t UNIT0_BLOCK2 = 0x108;
constexpr uint32_t UNIT0_RESUME = 0x10a;
constexpr uint32_t UNIT1_BLOCK0 = 0x200;
constexpr uint32_t UNIT1_BLOCK1 = 0x204;
constexpr uint32_t UNIT1_BLOCK2 = 0x208;
constexpr uint32_t UNIT1_RESUME = 0x20a;
// Sprite RAM entry offsets from f3rt::GRAPHICS_BASE, and the playfield/control probes.
constexpr uint32_t SPRITE_ENTRY_3 = 0x30;
constexpr uint32_t SPRITE_ENTRY_4 = 0x40;
constexpr uint32_t SPRITE_ENTRY_HIGH = 0x200;
constexpr uint32_t PLAYFIELD_RAM_PROBE = f3rt::GRAPHICS_BASE + f3rt::OFFS_PLAYFIELDS + 1;
// Work-RAM scratch used by the synthetic units: array offset and bus address.
constexpr uint32_t SYNTH_DATA_OFFSET = 0x100;
constexpr uint32_t SYNTH_DATA = f3rt::RAM_BASE + SYNTH_DATA_OFFSET;
// Emit-unit replay fixtures: unit 0 bumps a RAM counter and draws entries 3-4 of bank 0;
// unit 1 touches I/O and must abort. Blocks follow the generated hook order (hook first).
constexpr uint32_t SYNTH_STARTS0[] = {UNIT0_BLOCK0}, SYNTH_ENDS0[] = {UNIT0_BLOCK2};
constexpr uint32_t SYNTH_STARTS1[] = {UNIT1_BLOCK0}, SYNTH_ENDS1[] = {UNIT1_BLOCK2};
constexpr f3rt::EmitUnit SYNTH_UNIT0{0, "synth_bump", SYNTH_STARTS0, SYNTH_ENDS0,
                                      f3rt::UnitRegister::A3, 0x20};
constexpr f3rt::EmitUnit SYNTH_UNIT1{1, "synth_io", SYNTH_STARTS1, SYNTH_ENDS1,
                                      f3rt::UnitRegister::A3, 0};
constexpr std::array<const f3rt::EmitUnit *, 2> SYNTH_UNITS{&SYNTH_UNIT0, &SYNTH_UNIT1};

F3RT_SPRITE_BEHAVIOUR(SYNTH_BUMP_BEHAVIOUR, SYNTH_UNIT0, "Adds 5 to the unit's first word",
                      [](const f3rt::Unit<SYNTH_UNIT0> &u, f3rt::Patch<SYNTH_UNIT0> &p) {
                          p.u16<0>(uint16_t(u.u16<0>() + 5));
                          return true;
                      });
// The I/O unit declares size 0, so this only requests a replay (which then aborts on the read).
F3RT_SPRITE_BEHAVIOUR(SYNTH_IO_BEHAVIOUR, SYNTH_UNIT1, "Requests a replay of the I/O unit",
                      [](const f3rt::Unit<SYNTH_UNIT1> &, f3rt::Patch<SYNTH_UNIT1> &) {
                          return true;
                      });

void synth_start(f3_cpu *cpu) {
    f3_unit_enter(cpu, 0);
    const uint16_t v = f3_read16(cpu, cpu->a[3]);
    f3_write16(cpu, cpu->a[3] + 2, uint16_t(v + 1));
    f3_write16(cpu, f3rt::GRAPHICS_BASE + SPRITE_ENTRY_3, 0xabcd);
    f3_write16(cpu, f3rt::GRAPHICS_BASE + SPRITE_ENTRY_3 + 2, v);
    cpu->pc = UNIT0_BLOCK1;
    cpu->cycles += 8;
}

void synth_mid(f3_cpu *cpu) {
    f3_write16(cpu, f3rt::GRAPHICS_BASE + SPRITE_ENTRY_4, 0x1111);
    cpu->pc = UNIT0_BLOCK2;
    cpu->cycles += 4;
}

void synth_end(f3_cpu *cpu) {
    if (f3_unit_exit(cpu, 0)) return;
    cpu->pc = UNIT0_RESUME;
    cpu->cycles += 4;
}

void synth_io(f3_cpu *cpu) {
    f3_unit_enter(cpu, 1);
    cpu->d[2] = f3_read32(cpu, f3rt::IO_BASE);
    cpu->pc = UNIT1_BLOCK1;
    cpu->cycles += 4;
}

void synth_io_mid(f3_cpu *cpu) {
    cpu->pc = UNIT1_BLOCK2;
    cpu->cycles += 4;
}

void synth_io_end(f3_cpu *cpu) {
    if (f3_unit_exit(cpu, 1)) return;
    cpu->pc = UNIT1_RESUME;
    cpu->cycles += 4;
}

const f3_block SYNTH_BLOCKS[] = {{UNIT0_BLOCK0, synth_start},  {UNIT0_BLOCK1, synth_mid},
                                 {UNIT0_BLOCK2, synth_end},    {UNIT1_BLOCK0, synth_io},
                                 {UNIT1_BLOCK1, synth_io_mid}, {UNIT1_BLOCK2, synth_io_end}};

std::unique_ptr<f3rt::Machine> synth_machine(bool replay,
                                             const f3rt::SpriteBehaviour *behaviour = nullptr) {
    auto m = std::make_unique<f3rt::Machine>(fixture());
    EXPECT_TRUE(f3_register_blocks(&m->cpu, SYNTH_BLOCKS, std::size(SYNTH_BLOCKS)))
        << "Synthetic unit blocks register";
    if (replay) {
        f3rt::SpriteSystem::Options options;
        if (behaviour) options.behaviours.push_back(behaviour);
        m->sprites =
            std::make_unique<f3rt::SpriteSystem>(*m, f3rt::SpriteGroupTable{},
                                                 f3rt::SpriteUnitTable{SYNTH_UNITS},
                                                 std::move(options));
    }
    m->write16(SYNTH_DATA, 0x1234);
    m->cpu.sr = 0x2700;
    m->cpu.a[3] = SYNTH_DATA;
    return m;
}

void synth_span(f3rt::Machine &m) {
    m.cpu.pc = UNIT0_BLOCK0;
    for (int i = 0; i < 3; ++i) {
        EXPECT_TRUE(f3_dispatch(&m.cpu)) << "Synthetic unit span dispatches";
    }
}

struct SynthSnapshot {
    std::array<uint8_t, 0x20000> ram;
    std::array<uint8_t, 0x40000> graphics;
    std::array<uint8_t, 0x8000> palette;
    std::array<uint32_t, 8> d, a;
    uint32_t pc;
    uint16_t sr;
    uint64_t cycles, native_blocks;
    uint32_t crc;

    explicit SynthSnapshot(f3rt::Machine &m)
        : ram(m.ram), graphics(m.graphics), palette(m.palette), pc(m.cpu.pc),
          sr((f3_cc_flush(&m.cpu), m.cpu.sr)), cycles(m.cpu.cycles), native_blocks(m.native_blocks),
          crc(m.state_crc()) {
        std::copy(std::begin(m.cpu.d), std::end(m.cpu.d), d.begin());
        std::copy(std::begin(m.cpu.a), std::end(m.cpu.a), a.begin());
    }

    bool operator==(const SynthSnapshot &o) const {
        return ram == o.ram && graphics == o.graphics && palette == o.palette && d == o.d &&
               a == o.a && pc == o.pc && sr == o.sr && cycles == o.cycles &&
               native_blocks == o.native_blocks && crc == o.crc;
    }
};

// ---- Synthetic sprite-group tables ------------------------------------------------------
// Object records live in work RAM; the group roots are the record's X and Y fields.
constexpr uint32_t OBJ_A = f3rt::RAM_BASE + 0x200; // a3
constexpr uint32_t OBJ_B = f3rt::RAM_BASE + 0x280; // a4
constexpr uint32_t GRP_PC_XY = 0x300;     // a3, class 1 (roots +0/+2), long X:Y store
constexpr uint32_t GRP_PC_Y = 0x304;      // a3, class 1, Y-only store
constexpr uint32_t GRP_PC_CLASS2 = 0x308; // a3, class 2 (roots +0x10/+0x12)
constexpr uint32_t GRP_PC_A4 = 0x30c;     // a4, class 1
constexpr uint32_t GRP_PC_RESET = 0x400;  // reset write to class 1 X root
constexpr uint32_t GRP_PC_RESET2 = 0x404; // second reset writer, same root
constexpr uint32_t GRP_PC_TILE = 0x500;   // plain sprite-RAM store outside the table
constexpr uint32_t SLOT = 4;

// Base register encoding matches UnitRegister: 0..7 = d0..d7, 8..14 = a0..a6.
constexpr uint8_t BASE_A3 = 8 + 3, BASE_A4 = 8 + 4;
constexpr f3rt::SpriteGroupWrite SYNTH_WRITES[] = {
    {GRP_PC_XY, BASE_A3, uint8_t(f3rt::SpriteLane::XY), 1, 8, 0, 2},
    {GRP_PC_Y, BASE_A3, uint8_t(f3rt::SpriteLane::Y), 1, 8, 0xffff, 2},
    {GRP_PC_CLASS2, BASE_A3, uint8_t(f3rt::SpriteLane::XY), 2, 8, 0x10, 0x12},
    {GRP_PC_A4, BASE_A4, uint8_t(f3rt::SpriteLane::XY), 1, 8, 0, 2},
};
// Fields: {pc, base, pad, offset}.
constexpr f3rt::SpriteGroupReset SYNTH_RESETS[] = {{GRP_PC_RESET, BASE_A3, 0, 0},
                                                   {GRP_PC_RESET, BASE_A3, 0, 0x10},
                                                   {GRP_PC_RESET2, BASE_A3, 0, 0}};
constexpr f3rt::SpriteGroupTable SYNTH_GROUP_TABLE{
    std::span<const f3rt::SpriteGroupWrite>(SYNTH_WRITES),
    std::span<const f3rt::SpriteGroupReset>(SYNTH_RESETS)};

std::unique_ptr<f3rt::Machine> group_machine() {
    auto m = std::make_unique<f3rt::Machine>(fixture());
    m->sprites = std::make_unique<f3rt::SpriteSystem>(
        *m, SYNTH_GROUP_TABLE, f3rt::SpriteUnitTable{}, f3rt::SpriteSystem::Options{});
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

// Raw sprite-RAM entry (16 bytes) at `slot` of bank 0.
void raw_entry(uint8_t *ram, uint16_t slot, uint16_t tile, uint16_t x, uint16_t y,
               uint16_t cont = 0) {
    uint8_t *p = ram + size_t(slot) * 16;
    std::memset(p, 0, 16);
    raw16(p + 0, tile);
    raw16(p + 2, 0); // zoom
    raw16(p + 4, x);
    raw16(p + 6, y);
    raw16(p + 8, cont);
}

uint32_t sprite_address(uint16_t slot, uint32_t offset) {
    return f3rt::GRAPHICS_BASE + uint32_t(slot) * 16 + offset;
}

// A drawable head entry: tile, zoom, X/Y (written by the group rule) and no block bit.
void write_head(f3rt::Machine &m, uint16_t slot, uint16_t x, uint16_t y) {
    poke16(m, sprite_address(slot, 0), 1, GRP_PC_TILE); // tile
    poke32(m, sprite_address(slot, 4), uint32_t(x) << 16 | y, GRP_PC_XY);
}

// A hardware block member: tile and a spritecont with multi + both control bits set, so the
// decoder keeps the head's block position. Written outside the table -> no group of its own.
void write_member(f3rt::Machine &m, uint16_t slot) {
    poke16(m, sprite_address(slot, 0), 2, GRP_PC_TILE);
    poke16(m, sprite_address(slot, 8), 0x5800, GRP_PC_TILE); // spritecont 0x58, color 0
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

// ---- Behaviour replay (emit units are anchors only) ------------------------------------

TEST(SpriteSystem, BehaviourNameHyphenation) {
    EXPECT_EQ(std::string(SYNTH_BUMP_BEHAVIOUR.name), "synth-bump-behaviour")
        << "Behaviour names replace underscores with hyphens";
}

TEST(SpriteSystem, ListedEntriesAreTheFirstHalfOfEachBank) {
    using f3rt::sprite_slot_for;
    using f3rt::sprite_slot_listed;
    EXPECT_TRUE(sprite_slot_listed(0x0000));
    EXPECT_TRUE(sprite_slot_listed(0x3ff0)) << "last listed entry of bank 0";
    EXPECT_FALSE(sprite_slot_listed(0x4000)) << "first unlisted entry of bank 0";
    EXPECT_FALSE(sprite_slot_listed(0x7fff));
    EXPECT_TRUE(sprite_slot_listed(0x8000)) << "bank 1 starts its own listed half";
    EXPECT_FALSE(sprite_slot_listed(0xc000)) << "first unlisted entry of bank 1";
    EXPECT_TRUE(sprite_slot_listed(0x10001)) << "address bits above 15 do not affect the listing";
    EXPECT_EQ(sprite_slot_for(0x8010), 0x401) << "bank 1 keys continue after bank 0";
}

TEST(SpriteSystem, CompletedReplayLeavesMachineUntouched) {
    auto m = synth_machine(true, &SYNTH_BUMP_BEHAVIOUR);
    m->cpu.pc = UNIT0_BLOCK0;
    const SynthSnapshot at_unit(*m);
    f3_unit_enter(&m->cpu, 0);
    EXPECT_TRUE(SynthSnapshot(*m) == at_unit)
        << "Sandbox replay leaves RAM, graphics, cycles, native_blocks and CPU untouched";
    const auto report = m->sprites->report();
    EXPECT_TRUE(report.replays == 1 && report.completed == 1 && report.aborted == 0)
        << "Unit replay runs to its end PC";
}

TEST(SpriteSystem, DeviceAccessAbortsReplay) {
    auto m = synth_machine(true, &SYNTH_IO_BEHAVIOUR);
    m->cpu.pc = UNIT1_BLOCK0;
    const SynthSnapshot at_unit(*m);
    f3_unit_enter(&m->cpu, 1);
    EXPECT_TRUE(SynthSnapshot(*m) == at_unit) << "Aborted replay leaves machine state untouched";
    const auto report = m->sprites->report();
    EXPECT_TRUE(report.replays == 1 && report.completed == 0 && report.aborted == 1 &&
                report.units[1].aborts[size_t(f3rt::Sandbox::Abort::Device)] == 1)
        << "I/O read aborts the replay";
}

// Sprite groups are render-only: installing them, with or without a splicing behaviour, must
// never change emulated state.
TEST(SpriteSystem, BehaviourSplicingNeverChangesEmulatedState) {
    const auto without_snapshot = [] {
        auto without_groups = synth_machine(false);
        synth_span(*without_groups);
        return std::make_unique<SynthSnapshot>(*without_groups);
    }();
    auto with_groups = synth_machine(true, &SYNTH_BUMP_BEHAVIOUR);
    synth_span(*with_groups);
    ASSERT_EQ(with_groups->sprites->presentation().splices.size(), 1)
        << "Splice must actually happen";
    EXPECT_TRUE(SynthSnapshot(*with_groups) == *without_snapshot)
        << "ram, graphics, cpu, cycles, native_blocks and state_crc match without groups";
}

// `--discovery-log` is observe-only: the same stores with and without a DiscoveryLog leave
// cycles, native_blocks, state_crc and all device memory identical.
TEST(DiscoveryLog, IsObserveOnly) {
    const auto path = std::filesystem::temp_directory_path() / "f3rt-test-discovery.log";
    for (const bool log_all : {false, true}) {
        const auto stores = [](f3rt::Machine &m) {
            synth_span(m);
            m.write16(f3rt::GRAPHICS_BASE + SPRITE_ENTRY_3, 0x0001); // sprite RAM
            m.write8(PLAYFIELD_RAM_PROBE, 0x5a);                     // playfield RAM
            m.write16(f3rt::CONTROL_BASE, 0x1234);                   // control register
            m.write32(f3rt::GRAPHICS_BASE + SPRITE_ENTRY_HIGH, 0xdeadbeef);
        };
        std::unique_ptr<SynthSnapshot> plain_snapshot;
        std::array<uint8_t, 0x20> plain_control{};
        {
            auto plain = synth_machine(true);
            stores(*plain);
            plain_snapshot = std::make_unique<SynthSnapshot>(*plain);
            plain_control = plain->control;
        }
        auto logged = synth_machine(true);
        logged->discovery = std::make_unique<f3rt::DiscoveryLog>(path.string(), *logged, "test");
        logged->discovery->set_log_all(log_all);
        stores(*logged);
        EXPECT_TRUE(*plain_snapshot == SynthSnapshot(*logged))
            << "ram, graphics, palette, cpu, cycles, native_blocks and state_crc, log_all="
            << log_all;
        EXPECT_EQ(plain_control, logged->control);
        logged->discovery->finish(*logged);
        EXPECT_GT(std::filesystem::file_size(path), 0u) << "The log must have observed something";
    }
    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST(SpriteSystem, BehaviourSplicesReplacementEntries) {
    auto m = synth_machine(true, &SYNTH_BUMP_BEHAVIOUR);
    synth_span(*m);
    EXPECT_TRUE(m->ram[SYNTH_DATA_OFFSET + 2] == 0x12 && m->ram[SYNTH_DATA_OFFSET + 3] == 0x35 &&
                m->graphics[SPRITE_ENTRY_3 + 2] == 0x12 && m->graphics[SPRITE_ENTRY_3 + 3] == 0x34)
        << "Patched replay never reaches real RAM or sprite RAM";
    const auto presented = m->sprites->presentation();
    EXPECT_EQ(presented.splices.size(), 1) << "Behaviour produces one splice";
    ASSERT_FALSE(presented.splices.empty());
    const auto &splice = presented.splices[0];
    EXPECT_TRUE(
        !splice.bank && splice.first == 3 && splice.last == 4 && splice.real.size() == 32 &&
        splice.replacement.size() == 32 &&
        std::equal(splice.real.begin(), splice.real.end(), m->graphics.begin() + SPRITE_ENTRY_3) &&
        splice.replacement[0] == 0xab && splice.replacement[1] == 0xcd &&
        splice.replacement[2] == 0x12 && splice.replacement[3] == 0x39 &&
        splice.replacement[16] == 0x11)
        << "Splice holds the real range and the patched replay's entries";
    EXPECT_EQ(m->sprites->report().spliced, 1) << "Splice is counted";
}

// RapidCheck property: The real emit-unit replay and splice pipeline transforms arbitrary RAM
// counter values into spliced replacement entries (value + 5) while leaving real RAM unmodified.
RC_GTEST_PROP(SpriteSystem, ReplayPipelineSplicesPatchedCounter, ()) {
    const auto counter_val = *rc::gen::arbitrary<uint16_t>();
    auto m = std::make_unique<f3rt::Machine>(fixture());
    f3_register_blocks(&m->cpu, SYNTH_BLOCKS, std::size(SYNTH_BLOCKS));
    f3rt::SpriteSystem::Options options;
    options.behaviours.push_back(&SYNTH_BUMP_BEHAVIOUR);
    m->sprites = std::make_unique<f3rt::SpriteSystem>(
        *m, f3rt::SpriteGroupTable{}, f3rt::SpriteUnitTable{SYNTH_UNITS}, std::move(options));

    m->write16(SYNTH_DATA, counter_val);
    m->cpu.sr = 0x2700;
    m->cpu.a[3] = SYNTH_DATA;
    synth_span(*m);

    const uint16_t real_ram_val =
        uint16_t((m->ram[SYNTH_DATA_OFFSET + 2] << 8) | m->ram[SYNTH_DATA_OFFSET + 3]);
    RC_ASSERT(real_ram_val == uint16_t(counter_val + 1));
    const uint16_t real_gfx_val =
        uint16_t((m->graphics[SPRITE_ENTRY_3 + 2] << 8) | m->graphics[SPRITE_ENTRY_3 + 3]);
    RC_ASSERT(real_gfx_val == counter_val);

    const auto presented = m->sprites->presentation();
    RC_ASSERT(presented.splices.size() == 1);
    const auto &splice = presented.splices[0];
    const uint16_t spliced_val = uint16_t((splice.replacement[2] << 8) | splice.replacement[3]);
    RC_ASSERT(spliced_val == uint16_t(counter_val + 5));
}

// ---- Group keys and origins ------------------------------------------------------------

TEST(SpriteSystem, TableLookupResolvesKeyAndOrigin) {
    auto m = group_machine();
    poke16(*m, OBJ_A, 0x0100, GRP_PC_TILE);     // root X
    poke16(*m, OBJ_A + 2, 0x00f0, GRP_PC_TILE); // root Y
    write_head(*m, SLOT, 0x0020, 0x0030);

    const auto presentation = m->sprites->presentation();
    ASSERT_EQ(presentation.groups.size(), 0x800u) << "Groups are exposed per slot";
    const auto &slot = presentation.groups[SLOT];
    EXPECT_NE(slot.key, 0u) << "A tabled X/Y write resolves a group";
    EXPECT_EQ(slot.x, 0x0100 << 8) << "Origin X is the root in 24.8";
    EXPECT_EQ(slot.y, 0x00f0 << 8) << "Origin Y is the root in 24.8";
    EXPECT_EQ(f3rt::SpriteGroupSlot{}, presentation.groups[SLOT + 1]) << "Untouched slots stay 0";
}

TEST(SpriteSystem, ResetBumpsGenerationAndChangesKey) {
    auto m = group_machine();
    poke16(*m, OBJ_A, 0x0100, GRP_PC_TILE);
    poke16(*m, OBJ_A + 2, 0x00f0, GRP_PC_TILE);
    write_head(*m, SLOT, 0x0020, 0x0030);
    const uint64_t first = m->sprites->presentation().groups[SLOT].key;
    ASSERT_NE(first, 0u);

    // A continuation write (same object) keeps the key.
    poke16(*m, OBJ_A, 0x0101, GRP_PC_TILE);
    write_head(*m, SLOT, 0x0020, 0x0030);
    EXPECT_EQ(m->sprites->presentation().groups[SLOT].key, first)
        << "Rewriting the same record keeps the group key";

    // A reset write (spawn/teleport: not derived from the previous value) starts a new group.
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
    // A write from a PC the table does not know snaps the slot.
    poke16(*m, sprite_address(SLOT, 4), 0x0040, GRP_PC_TILE);
    EXPECT_EQ(m->sprites->presentation().groups[SLOT].key, 0u)
        << "A writer outside the table is unresolved";
}

TEST(SpriteSystem, BlockMembersInheritTheHeadGroup) {
    auto m = group_machine();
    poke16(*m, OBJ_A, 0x0040, GRP_PC_TILE);
    poke16(*m, OBJ_A + 2, 0x0050, GRP_PC_TILE);
    write_head(*m, SLOT, 0x0040, 0x0050);
    write_member(*m, uint16_t(SLOT + 1));

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

// ---- Reset continuity -------------------------------------------------------------------

TEST(SpriteSystem, PeriodicResetsDoNotRebirthTheGroup) {
    auto m = group_machine();
    poke16(*m, OBJ_A + 2, 0x00f0, GRP_PC_TILE);
    m->frame = 1;
    poke16(*m, OBJ_A, 0x0100, GRP_PC_RESET);
    write_head(*m, SLOT, 0x0020, 0x0030);
    const uint64_t key = m->sprites->presentation().groups[SLOT].key;
    ASSERT_NE(key, 0u);
    ASSERT_EQ(m->sprites->report().generation_bumps, 1u);

    // The same reset runs again next frame: a recomputation, not a birth.
    m->frame = 2;
    poke16(*m, OBJ_A, 0x0101, GRP_PC_RESET);
    write_head(*m, SLOT, 0x0020, 0x0030);
    EXPECT_EQ(m->sprites->presentation().groups[SLOT].key, key)
        << "A reset repeated on consecutive frames keeps the key";
    EXPECT_EQ(m->sprites->report().generation_bumps, 1u);
    EXPECT_EQ(m->sprites->report().periodic_resets, 1u);

    // A root write without a reset does not count as a reset event.
    m->frame = 3;
    poke16(*m, OBJ_A, 0x0102, GRP_PC_TILE);
    write_head(*m, SLOT, 0x0020, 0x0030);
    EXPECT_EQ(m->sprites->presentation().groups[SLOT].key, key);

    // Reset after a frame without one: a birth.
    m->frame = 4;
    poke16(*m, OBJ_A, 0x0103, GRP_PC_RESET);
    write_head(*m, SLOT, 0x0020, 0x0030);
    EXPECT_NE(m->sprites->presentation().groups[SLOT].key, key)
        << "A reset after a gap starts a new generation";
    EXPECT_EQ(m->sprites->report().generation_bumps, 2u);

    // Skip a frame, then two reset writers for the same root in one frame bump at most once.
    m->frame = 5;
    poke16(*m, OBJ_A, 0x0104, GRP_PC_TILE);
    m->frame = 6;
    poke16(*m, OBJ_A, 0x0105, GRP_PC_RESET);
    poke16(*m, OBJ_A, 0x0106, GRP_PC_RESET2);
    EXPECT_EQ(m->sprites->report().generation_bumps, 3u)
        << "Two reset writers in one frame bump once";
}

// ---- Decoder: block inheritance --------------------------------------------------------

TEST(SpriteSystem, DecoderDoesNotLeakBlockGroupAcrossAnUnresolvedHead) {
    std::array<uint8_t, 0x10000> ram{};
    raw_entry(ram.data(), 0, 5, 100, 50);        // resolved head
    raw_entry(ram.data(), 1, 6, 0, 0, 0x5800);   // its member
    raw_entry(ram.data(), 2, 7, 200, 60);        // unresolved head (group 0)
    raw_entry(ram.data(), 3, 8, 0, 0, 0x5800);   // member of the unresolved block

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
    m->sprites = std::make_unique<f3rt::SpriteSystem>(
        *m, f3rt::SpriteSystem::game_table(), f3rt::SpriteSystem::game_units(),
        f3rt::SpriteSystem::Options{});

    uint64_t frames = 1500;
    if (const char *env = std::getenv("MOTION_GROUP_FRAMES")) frames = std::stoull(env);
    f3rt::GpuMotionHistory history;
    std::unordered_map<uint64_t, std::pair<int32_t, int32_t>> previous, current;
    std::unordered_set<uint64_t> previous_ambiguous, current_ambiguous;
    uint64_t paired_frames = 0, grouped = 0, paired_group_sprites = 0, moving_grouped = 0,
             verified = 0, cross_paired = 0, max_error = 0, key_changed = 0, key_change_moved = 0,
             rejected_group = 0, runs = 0;
    // Why a grouped sprite's key is absent from the previous frame (no key design change yet).
    uint64_t prev_group_zero = 0, base_changed = 0, class_changed = 0, gen_changed = 0,
             absent = 0, ambiguous = 0, multi_origin_keys = 0, ambiguous_examples = 0;
    uint64_t different_pc_pairs = 0, pc_split_unique = 0, still_pc_ambiguous = 0,
             multi_part_split = 0;
    std::unordered_map<uint64_t, uint64_t> ambiguous_pc_class; // (pc<<8|class) -> sprites
    std::vector<f3rt::SceneSprite> prev_sprites;
    std::unordered_set<uint64_t> prev_keys, prev_base_class;
    std::unordered_set<uint32_t> prev_bases;
    uint64_t prev_count = 0;
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
        history.capture(scene, f);
        const f3rt::MotionResult motion = history.apply(scene, 0.5f);
        multi_origin_keys += current_ambiguous.size();
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
                if (!unique) {
                    const uint32_t base = uint32_t((sprite.group >> 40) & 0xffffff);
                    const unsigned cls = unsigned((sprite.group >> 32) & 0xff);
                    if (previous_ambiguous.contains(sprite.group) ||
                        current_ambiguous.contains(sprite.group)) {
                        ++ambiguous;
                        std::unordered_map<uint32_t, std::vector<std::pair<int32_t, int32_t>>>
                            by_pc;
                        for (unsigned k = 0; k < scene.sprite_count; ++k)
                            if (scene.sprites[k].group == sprite.group)
                                by_pc[scene.sprites[k].group_pc].push_back(
                                    {scene.sprites[k].group_x, scene.sprites[k].group_y});
                        if (by_pc.size() > 1) ++different_pc_pairs;
                        bool unique_pc = true;
                        bool multi = false, single = false;
                        for (const auto &[pc, list] : by_pc) {
                            for (size_t a = 0; a < list.size(); ++a)
                                for (size_t b = a + 1; b < list.size(); ++b)
                                    if (list[a] != list[b]) unique_pc = false;
                            multi |= pc == 0x0a0beu;
                            single |= pc == 0x09e9au;
                        }
                        if (unique_pc) ++pc_split_unique;
                        else ++still_pc_ambiguous;
                        if (multi && single) ++multi_part_split;
                        ++ambiguous_pc_class[uint64_t(sprite.group_pc) << 8 | cls];
                        if (ambiguous_examples < 4) {
                            ++ambiguous_examples;
                            std::cout << "ambiguous key base=0x" << std::hex
                                      << ((sprite.group >> 40) & 0xffffff) << std::dec
                                      << " class=" << ((sprite.group >> 32) & 0xff)
                                      << " origins(current):";
                            for (unsigned k = 0; k < scene.sprite_count; ++k)
                                if (scene.sprites[k].group == sprite.group)
                                    std::cout << " slot" << k << "=("
                                              << scene.sprites[k].group_x << ","
                                              << scene.sprites[k].group_y << ")";
                            std::cout << std::endl;
                        }
                    } else if (prev_keys.contains(sprite.group)) {
                        ++gen_changed;
                    } else if (prev_base_class.contains(uint64_t(base) << 8 | cls)) {
                        ++gen_changed;
                    } else if (prev_bases.contains(base)) {
                        ++class_changed;
                    } else if (i < prev_count && prev_sprites[i].group == 0) {
                        ++prev_group_zero;
                    } else if (i >= prev_count) {
                        ++absent;
                    } else {
                        ++base_changed;
                    }
                }
                const bool moved = motion.state.sprite_x[i] != sprite.x ||
                                   motion.state.sprite_y[i] != sprite.y;
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
                    std::max(std::abs(expected_x - motion.state.sprite_x[i]),
                             std::abs(expected_y - motion.state.sprite_y[i]));
                max_error = std::max<uint64_t>(max_error, uint64_t(error));
                if (error <= 1) ++verified;
                else ++cross_paired;
            }
        }
        previous = std::move(current);
        previous_ambiguous = std::move(current_ambiguous);
        prev_sprites.assign(scene.sprites.begin(), scene.sprites.begin() + scene.sprite_count);
        prev_count = scene.sprite_count;
        prev_keys.clear();
        prev_bases.clear();
        prev_base_class.clear();
        for (unsigned i = 0; i < scene.sprite_count; ++i) {
            const uint64_t key = scene.sprites[i].group;
            if (!key) continue;
            prev_keys.insert(key);
            const uint32_t base = uint32_t((key >> 40) & 0xffffff);
            prev_bases.insert(base);
            prev_base_class.insert(uint64_t(base) << 8 | unsigned((key >> 32) & 0xff));
        }
    }
    std::cout << "motion pairing: frames=" << runs << " paired_frames=" << paired_frames
              << " grouped_sprites=" << grouped
              << " paired_group_sprites=" << paired_group_sprites
              << " moving_group_sprites=" << moving_grouped << " verified_delta=" << verified
              << " cross_paired=" << cross_paired << " max_error=" << max_error
              << " key_changed_sprites=" << key_changed
              << " key_change_moved=" << key_change_moved
              << " group_rejections=" << rejected_group << std::endl;
    const auto groups = m->sprites->report();
    std::cout << "key churn: prev_group_zero=" << prev_group_zero
              << " base_changed=" << base_changed << " class_changed=" << class_changed
              << " gen_changed=" << gen_changed << " ambiguous=" << ambiguous
              << " absent=" << absent << " multi_origin_keys=" << multi_origin_keys
              << " different_pc_pairs=" << different_pc_pairs
              << " pc_split_unique=" << pc_split_unique
              << " still_ambiguous_after_pc=" << still_pc_ambiguous
              << " multi_part_split=" << multi_part_split
              << " generation_bumps=" << groups.generation_bumps
              << " periodic_suppressed=" << groups.periodic_resets << " top_bump_pcs:";
    for (const auto &pc : groups.top_bump_pcs)
        std::cout << " 0x" << std::hex << pc.pc << std::dec << "=" << pc.count;
    std::cout << std::endl;
    std::vector<std::pair<uint64_t, uint64_t>> top(ambiguous_pc_class.begin(),
                                                   ambiguous_pc_class.end());
    std::sort(top.begin(), top.end(),
              [](const auto &a, const auto &b) { return a.second > b.second; });
    std::cout << "ambiguous top (pc,class) pairs:";
    for (size_t i = 0; i < top.size() && i < 8; ++i)
        std::cout << " (0x" << std::hex << (top[i].first >> 8) << std::dec << ","
                  << (top[i].first & 0xff) << ")=" << top[i].second;
    std::cout << std::endl;
    EXPECT_GT(paired_frames, 0u) << "No consecutive paired frame was captured";
    EXPECT_GT(paired_group_sprites, 0u) << "No sprite paired by group key";
    EXPECT_EQ(cross_paired, 0u) << "A paired sprite moved by a delta other than its group delta";
    EXPECT_EQ(key_change_moved, 0u) << "A new/ambiguous group key was interpolated instead of snapping";
}

// ---- ROM-backed coverage ----------------------------------------------------------------

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
    const bool has_groups = !f3rt::SpriteSystem::game_table().writes.empty();
    bool has_full_detail = false;
    for (const auto *b : f3rt::registered_sprite_behaviours()) {
        if (std::string_view(b->name) == "full-detail") {
            has_full_detail = true;
            break;
        }
    }
    if (!has_groups && !has_full_detail) {
        GTEST_SKIP() << "Configured game declares no sprite groups and no full-detail behaviour";
    }

    f3rt::runner::SpriteCheckArgs args;
    args.rom_dir = rom_dir;
    args.set = F3RT_DEFAULT_SET;
    args.frames = 6000;
    if (const char *frames_env = std::getenv("SPRITE_CHECK_FRAMES")) {
        args.frames = std::stoull(frames_env);
    }
    if (has_full_detail) args.behaviours.push_back("full-detail");

    const auto res = f3rt::runner::run_sprite_check(args);
    EXPECT_TRUE(res.passed) << res.error;
    EXPECT_GT(res.frames, 0u);
    if (has_groups) {
        EXPECT_GT(res.group_entries, 0u) << "The generated table must group drawn sprites";
        EXPECT_GT(res.max_groups_frame, 0u);
    }
    if (has_full_detail) EXPECT_GT(res.spliced, 0u);
}

#endif
