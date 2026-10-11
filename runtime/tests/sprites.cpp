#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/rom.hpp"
#include "interpreter.hpp"
#include "state_io.hpp"
#include "sprites/units.hpp"
#include "sprites/behaviour.hpp"
#include "renderer/decode.hpp"
#include "sprite_check.hpp"
#include "support.hpp"
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <memory>
#include <set>
#include <sstream>
#include <string>

#if defined(F3RT_GENERATED)
#include "program.h"
#endif

using f3test::fixture;

namespace {
// Emit-unit replay fixtures: unit 0 bumps a RAM counter and draws entries 3-4 of bank 0;
// unit 1 touches I/O and must abort. Blocks follow the generated hook order (hook first).
constexpr uint32_t synth_starts0[] = {0x100}, synth_ends0[] = {0x108};
constexpr uint32_t synth_starts1[] = {0x200}, synth_ends1[] = {0x208};
constexpr f3rt::EmitUnit synth_unit0{0, "synth_bump", synth_starts0, synth_ends0, f3rt::UnitRegister::A3, 0x20, f3rt::UnitOwner::Unit};
constexpr f3rt::EmitUnit synth_unit1{1, "synth_io", synth_starts1, synth_ends1, f3rt::UnitRegister::A3, 0, f3rt::UnitOwner::Unit};
constexpr std::array<const f3rt::EmitUnit *, 2> synth_units{&synth_unit0, &synth_unit1};

F3RT_SPRITE_BEHAVIOUR(synth_bump_behaviour, synth_unit0, "Adds 5 to the unit's first word",
    [](const f3rt::Unit<synth_unit0> &u, f3rt::Patch<synth_unit0> &p) {
        p.u16<0>(uint16_t(u.u16<0>() + 5));
        return true;
    });

void synth_start(f3_cpu *cpu) {
    f3_unit_enter(cpu, 0);
    const uint16_t v = f3_read16(cpu, cpu->a[3]);
    f3_write16(cpu, cpu->a[3] + 2, uint16_t(v + 1));
    f3_write16(cpu, 0x600030, 0xabcd);
    f3_write16(cpu, 0x600032, v);
    cpu->pc = 0x104;
    cpu->cycles += 8;
}

void synth_mid(f3_cpu *cpu) {
    f3_write16(cpu, 0x600040, 0x1111);
    cpu->pc = 0x108;
    cpu->cycles += 4;
}

void synth_end(f3_cpu *cpu) {
    if (f3_unit_exit(cpu, 0)) return;
    cpu->pc = 0x10a;
    cpu->cycles += 4;
}

void synth_io(f3_cpu *cpu) {
    f3_unit_enter(cpu, 1);
    cpu->d[2] = f3_read32(cpu, 0x4a0000);
    cpu->pc = 0x204;
    cpu->cycles += 4;
}

void synth_io_mid(f3_cpu *cpu) {
    cpu->pc = 0x208;
    cpu->cycles += 4;
}

void synth_io_end(f3_cpu *cpu) {
    if (f3_unit_exit(cpu, 1)) return;
    cpu->pc = 0x20a;
    cpu->cycles += 4;
}

const f3_block synth_blocks[] = {{0x100, synth_start}, {0x104, synth_mid}, {0x108, synth_end},
                                {0x200, synth_io}, {0x204, synth_io_mid}, {0x208, synth_io_end}};

std::unique_ptr<f3rt::Machine> synth_machine(bool units, bool check, bool behaviour) {
    auto m = std::make_unique<f3rt::Machine>(fixture());
    m->allow_main_fallback = false;
    EXPECT_TRUE(f3_register_blocks(&m->cpu, synth_blocks, std::size(synth_blocks)))
        << "Synthetic unit blocks register";
    if (units) {
        f3rt::SpriteUnits::Options options;
        options.check = check;
        if (behaviour) options.behaviours.push_back(&synth_bump_behaviour);
        m->sprite_units = std::make_unique<f3rt::SpriteUnits>(*m, f3rt::SpriteUnitTable{synth_units, {}}, std::move(options));
    }
    m->write16(0x400100, 0x1234);
    m->cpu.sr = 0x2700;
    m->cpu.a[3] = 0x400100;
    return m;
}

void synth_span(f3rt::Machine &m) {
    m.cpu.pc = 0x100;
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
        : ram(m.ram), graphics(m.graphics), palette(m.palette), pc(m.cpu.pc), sr((f3_cc_flush(&m.cpu), m.cpu.sr)),
          cycles(m.cpu.cycles), native_blocks(m.native_blocks), crc(m.state_crc()) {
        std::copy(std::begin(m.cpu.d), std::end(m.cpu.d), d.begin());
        std::copy(std::begin(m.cpu.a), std::end(m.cpu.a), a.begin());
    }

    bool operator==(const SynthSnapshot &o) const {
        return ram == o.ram && graphics == o.graphics && palette == o.palette && d == o.d && a == o.a &&
               pc == o.pc && sr == o.sr && cycles == o.cycles && native_blocks == o.native_blocks &&
               crc == o.crc;
    }
};
} // namespace

TEST(SpriteUnits, BehaviourNameHyphenation) {
    EXPECT_EQ(std::string(synth_bump_behaviour.name), "synth-bump-behaviour")
        << "Behaviour names replace underscores with hyphens";
}

TEST(SpriteUnits, CompletedReplayLeavesMachineUntouched) {
    auto m = synth_machine(true, true, false);
    m->cpu.pc = 0x100;
    const SynthSnapshot at_unit(*m);
    f3_unit_enter(&m->cpu, 0);
    EXPECT_TRUE(SynthSnapshot(*m) == at_unit)
        << "Sandbox replay leaves RAM, graphics, cycles, native_blocks and CPU untouched";
    const auto report = m->sprite_units->report();
    EXPECT_TRUE(report.replays == 1 && report.completed == 1 && report.aborted == 0)
        << "Unit replay runs to its end PC";
}

TEST(SpriteUnits, DeviceAccessAbortsReplay) {
    auto m = synth_machine(true, true, false);
    m->cpu.pc = 0x200;
    const SynthSnapshot at_unit(*m);
    f3_unit_enter(&m->cpu, 1);
    EXPECT_TRUE(SynthSnapshot(*m) == at_unit)
        << "Aborted replay leaves machine state untouched";
    const auto report = m->sprite_units->report();
    EXPECT_TRUE(report.replays == 1 && report.completed == 0 && report.aborted == 1 &&
                report.units[1].aborts[size_t(f3rt::SpriteUnits::Abort::Device)] == 1)
        << "I/O read aborts the replay";
}

TEST(SpriteUnits, UnpatchedReplayMatchesRealSpan) {
    auto on = synth_machine(true, true, false);
    auto off = synth_machine(false, false, false);
    synth_span(*on);
    synth_span(*off);
    EXPECT_TRUE(SynthSnapshot(*on) == SynthSnapshot(*off))
        << "Replay on/off yields identical state, cycles and native_blocks";
    EXPECT_TRUE(on->ram[0x102] == 0x12 && on->ram[0x103] == 0x35)
        << "Real span still executes against unpatched RAM";
    const auto report = on->sprite_units->report();
    EXPECT_TRUE(report.matched == 1 && report.mismatched == 0 && report.aborted == 0 &&
                report.stray_writes == 0 && on->sprite_units->passed())
        << "Check mode matches the real written entries";
    const auto presented = on->sprite_units->presentation();
    EXPECT_TRUE(presented.identity.size() == 0x800 && presented.splices.empty())
        << "No behaviour means no splice";
    EXPECT_TRUE(presented.identity[3] && presented.identity[4] &&
                presented.identity[3] != presented.identity[4] &&
                !presented.identity[2] && !presented.identity[5])
        << "Written sprite entries receive per-entry identities";
    on->write16(0x600030, 0x0001);
    EXPECT_TRUE(!on->sprite_units->presentation().identity[3] && on->sprite_units->presentation().identity[4])
        << "A write outside any unit clears that slot's identity";
    EXPECT_EQ(on->sprite_units->report().stray_unaccounted, 2)
        << "Check mode records unaccounted writers";
}

TEST(SpriteUnits, BehaviourSplicesReplacementEntries) {
    auto m = synth_machine(true, false, true);
    synth_span(*m);
    EXPECT_TRUE(m->ram[0x102] == 0x12 && m->ram[0x103] == 0x35 &&
                m->graphics[0x32] == 0x12 && m->graphics[0x33] == 0x34)
        << "Patched replay never reaches real RAM or sprite RAM";
    const auto presented = m->sprite_units->presentation();
    EXPECT_EQ(presented.splices.size(), 1) << "Behaviour produces one splice";
    ASSERT_FALSE(presented.splices.empty());
    const auto &splice = presented.splices[0];
    EXPECT_TRUE(!splice.bank && splice.first == 3 && splice.last == 4 &&
                splice.real.size() == 32 && splice.replacement.size() == 32 &&
                std::equal(splice.real.begin(), splice.real.end(), m->graphics.begin() + 0x30) &&
                splice.replacement[0] == 0xab && splice.replacement[1] == 0xcd &&
                splice.replacement[2] == 0x12 && splice.replacement[3] == 0x39 &&
                splice.replacement[16] == 0x11 && splice.identity)
        << "Splice holds the real range and the patched replay's entries";
    EXPECT_EQ(m->sprite_units->report().spliced, 1) << "Splice is counted";
}

// RapidCheck property: The real emit-unit replay and splice pipeline transforms arbitrary RAM counter
// values into spliced replacement entries (value + 5) while leaving real RAM unmodified by the behaviour.
RC_GTEST_PROP(SpriteUnits, ReplayPipelineSplicesPatchedCounter, ()) {
    const auto counter_val = *rc::gen::arbitrary<uint16_t>();
    auto m = std::make_unique<f3rt::Machine>(fixture());
    m->allow_main_fallback = false;
    f3_register_blocks(&m->cpu, synth_blocks, std::size(synth_blocks));
    f3rt::SpriteUnits::Options options;
    options.check = false;
    options.behaviours.push_back(&synth_bump_behaviour);
    m->sprite_units = std::make_unique<f3rt::SpriteUnits>(*m, f3rt::SpriteUnitTable{synth_units, {}}, std::move(options));

    m->write16(0x400100, counter_val);
    m->cpu.sr = 0x2700;
    m->cpu.a[3] = 0x400100;
    synth_span(*m);

    // 1. Real RAM is untouched by the behaviour patch:
    // synth_start wrote counter_val + 1 to 0x400102.
    const uint16_t real_ram_val = uint16_t((m->ram[0x102] << 8) | m->ram[0x103]);
    RC_ASSERT(real_ram_val == uint16_t(counter_val + 1));

    // 2. Real graphics RAM was untouched by the replay:
    const uint16_t real_gfx_val = uint16_t((m->graphics[0x32] << 8) | m->graphics[0x33]);
    RC_ASSERT(real_gfx_val == counter_val);

    // 3. The presentation splice received the behaviour's patched overlay (counter_val + 5):
    const auto presented = m->sprite_units->presentation();
    RC_ASSERT(presented.splices.size() == 1);
    const auto &splice = presented.splices[0];
    const uint16_t spliced_val = uint16_t((splice.replacement[2] << 8) | splice.replacement[3]);
    RC_ASSERT(spliced_val == uint16_t(counter_val + 5));
}

#if defined(F3RT_GENERATED)

TEST(SpriteUnits, RomReplayAndInvariants) {
    const char *rom_env = std::getenv("F3_ROM_DIR");
#ifdef F3RT_DEFAULT_ROM_DIR
    if (!rom_env) rom_env = F3RT_DEFAULT_ROM_DIR;
#endif
    if (!rom_env || !*rom_env) {
        GTEST_SKIP() << "F3_ROM_DIR not set; skipping ROM-backed sprite units check";
    }

    std::string set = "commandw";
#ifdef F3RT_DEFAULT_SET
    set = F3RT_DEFAULT_SET;
#endif
    if (set != "commandw") {
        GTEST_SKIP() << "Configured game does not declare emit units";
    }

    const std::filesystem::path rom_dir(rom_env);
    if (!std::filesystem::exists(rom_dir)) {
        GTEST_SKIP() << "ROM directory does not exist: " << rom_dir;
    }

    f3rt::runner::SpriteCheckArgs args;
    args.rom_dir = rom_dir;
    args.set = set;
    args.frames = 6000;
    if (const char *frames_env = std::getenv("SPRITE_CHECK_FRAMES")) {
        args.frames = std::stoull(frames_env);
    }
    args.behaviours.push_back("full-detail");

    const auto res = f3rt::runner::run_sprite_check(args);
    EXPECT_TRUE(res.passed) << res.error;
    EXPECT_GT(res.invocations, 0u);
    EXPECT_EQ(res.unaccounted_writes, 0u);
    EXPECT_EQ(res.mismatches, 0u);
    EXPECT_EQ(res.aborted, 0u);
    EXPECT_EQ(res.violations, 0u) << res.first_violation;
}

#endif

