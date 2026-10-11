#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "interpreter.hpp"
#include "support.hpp"
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

using f3test::fixture;

namespace {
void native(f3_cpu *cpu) {
    cpu->d[0] = 99;
    cpu->pc += 2;
    cpu->cycles += 4;
}

void native_other(f3_cpu *cpu) {
    cpu->d[0] = 17;
    cpu->pc += 2;
    cpu->cycles += 4;
}

class CpuTest : public ::testing::Test {
protected:
    std::unique_ptr<f3rt::Machine> m;

    void SetUp() override {
        m = std::make_unique<f3rt::Machine>(fixture());
    }
};
} // namespace

TEST(Cpu, NativeLookup) {
    auto m = std::make_unique<f3rt::Machine>(fixture());
    auto &cpu = m->cpu;
    m->allow_main_fallback = false;
    const auto hit = [&](uint32_t pc, uint32_t value) {
        cpu.pc = pc;
        cpu.sr = 0x2700;
        cpu.d[0] = 0;
        const auto count = m->native_blocks;
        EXPECT_TRUE(f3_dispatch(&cpu) && cpu.pc == pc + 2 && cpu.d[0] == value && m->native_blocks == count + 1)
            << "Indexed dispatch executes the correct registered entry";
    };
    const auto miss = [&](uint32_t pc) {
        cpu.pc = pc;
        cpu.sr = 0x2700;
        const auto count = m->native_blocks;
        bool rejected = false;
        try {
            f3_dispatch(&cpu);
        } catch (const std::runtime_error &e) {
            rejected = std::string(e.what()).find("Untranslated") != std::string::npos;
        }
        EXPECT_TRUE(rejected && cpu.pc == pc && m->native_blocks == count && !m->fallback_instructions)
            << "Page gaps, odd PCs and aliases must not execute a nearby entry";
    };
    const f3_block sparse[] = {{0x100, native}, {0x104, native_other}, {0xffe, native},
                              {0x1000, native_other}, {0x1ffffe, native}, {0x200000, native_other}};
    EXPECT_TRUE(f3_register_blocks(&cpu, sparse, std::size(sparse))) << "Sparse ROM table registers";
    hit(0x100, 99);
    hit(0x104, 17);
    hit(0xffe, 99);
    hit(0x1000, 17);
    hit(0x1ffffe, 99);
    for (uint32_t pc : {0x101u, 0x102u, 0xffcu, 0x1002u, 0x1100u, 0x1ffffcu, 0x200000u, 0xff000100u})
        miss(pc);
    const f3_block duplicate[] = {{0x100, native}, {0x100, native_other}};
    const f3_block unsorted[] = {{0x104, native}, {0x100, native_other}};
    const f3_block odd[] = {{0x101, native}}, missing[] = {{0x100, nullptr}};
    EXPECT_TRUE(!f3_register_blocks(&cpu, duplicate, 2) && !f3_register_blocks(&cpu, unsorted, 2) &&
                !f3_register_blocks(&cpu, odd, 1) && !f3_register_blocks(&cpu, missing, 1))
        << "Invalid replacement tables are rejected atomically";
    hit(0x104, 17);
    const f3_block dense[] = {{0x200, native}, {0x202, native_other}, {0x204, native}};
    EXPECT_TRUE(f3_register_blocks(&cpu, dense, 3)) << "Dense partial page replaces sparse table";
    hit(0x200, 99);
    hit(0x202, 17);
    hit(0x204, 99);
    miss(0x100);
    miss(0x1fe);
    miss(0x201);
    miss(0x206);
    m->reset();
    hit(0x202, 17);
    std::vector<uint8_t> saved(m->state_size());
    m->save_state(saved);
    hit(0x204, 99);
    m->load_state(saved);
    hit(0x202, 17);
    cpu.pc = 0x202;
    cpu.sr = 0xa700;
    bool traced = false;
    try {
        f3_dispatch(&cpu);
    } catch (const std::runtime_error &) {
        traced = true;
    }
    EXPECT_TRUE(traced && cpu.pc == 0x202) << "Trace mode still rejects block execution in strict-native mode";
    EXPECT_TRUE(f3_register_blocks(&cpu, nullptr, 0)) << "Empty table replaces indexed registration";
    miss(0x202);
}

TEST(Cpu, WideBusBoundaries) {
    auto wide = std::make_unique<f3rt::Machine>(fixture());
    auto bytes = std::make_unique<f3rt::Machine>(fixture());
    const auto seed = [](auto &region) {
        for (size_t i = 0; i < region.size(); ++i) region[i] = uint8_t(i * 37 + 11);
    };
    for (auto *m : {wide.get(), bytes.get()}) {
        seed(m->roms.main);
        seed(m->ram);
        seed(m->palette);
        seed(m->graphics);
        seed(m->shared);
    }
    for (uint32_t boundary : {0u, 0x200000u, 0x400000u, 0x420000u, 0x440000u, 0x448000u,
                             0x4a0004u, 0x4a0013u, 0x4c0000u, 0x600000u, 0x640000u, 0x660000u,
                             0xc00000u, 0xc00800u, 0xc80000u, 0xc80100u, 0x1000000u}) {
        for (uint32_t alias : {0u, 0x5a000000u})
            for (unsigned delta = 0; delta < 7; ++delta) {
                const uint32_t a = boundary - 3 + delta + alias;
                const uint16_t expected16 = uint16_t(uint16_t(bytes->read8(a)) << 8 | bytes->read8(a + 1));
                uint32_t expected32 = 0;
                for (unsigned i = 0; i < 4; ++i) expected32 = (expected32 << 8) | bytes->read8(a + i);
                EXPECT_TRUE(wide->read16(a) == expected16 && wide->read32(a) == expected32)
                    << "Wide reads preserve byte order across regions, mirrors and address rollover";
                wide->write16(a, 0xa1b2);
                bytes->write8(a, 0xa1);
                bytes->write8(a + 1, 0xb2);
                wide->write32(a, 0x31415926);
                for (unsigned i = 0; i < 4; ++i) bytes->write8(a + i, uint8_t(0x31415926u >> (24 - i * 8)));
                EXPECT_TRUE(wide->ram == bytes->ram && wide->palette == bytes->palette &&
                            wide->graphics == bytes->graphics && wide->shared == bytes->shared &&
                            wide->control == bytes->control && wide->coin_count == bytes->coin_count &&
                            wide->coin_word == bytes->coin_word && wide->timer_control == bytes->timer_control)
                    << "Wide writes preserve ordered byte effects at memory and MMIO boundaries";
            }
    }
    std::vector<uint8_t> a(wide->state_size()), b(bytes->state_size());
    wide->save_state(a);
    bytes->save_state(b);
    EXPECT_EQ(a, b)
        << "Wide access preserves complete device state, including reset and EEPROM side effects";
}

TEST_F(CpuTest, MainRomBindingAndColdReset) {
    const uint32_t main_crc = f3rt::crc32(m->roms.main.data(), m->roms.main.size());
    EXPECT_TRUE(f3_validate_main_rom(&m->cpu, m->roms.main.size(), main_crc))
        << "Native main image binding accepts the loaded revision";
    m->roms.main[0x104] = 1;
    bool mismatched = false;
    try {
        f3_validate_main_rom(&m->cpu, m->roms.main.size(), main_crc);
    } catch (const std::runtime_error &) {
        mismatched = true;
    }
    EXPECT_TRUE(mismatched && !m->blocks)
        << "Wrong native image is rejected before dispatch registration";
    m->roms.main[0x104] = 0;
    mismatched = false;
    try {
        f3_validate_main_rom(&m->cpu, 0x100000, main_crc);
    } catch (const std::runtime_error &) {
        mismatched = true;
    }
    EXPECT_TRUE(mismatched) << "Native image length is bound even when its CRC matches";
    EXPECT_TRUE(m->cpu.cycles == 4 && m->cpu.pc == 0x100 && m->cpu.d[0] == 0)
        << "Cold reset charges four cycles without executing the first opcode";
    EXPECT_EQ(m->cpu.dispatch_deadline, 0) << "Reset requires a fresh scheduling boundary";
}

TEST_F(CpuTest, VblankSchedule) {
    const auto raster_tick = [](uint64_t pixels) {
        return (pixels * f3rt::Machine::main_clock + f3rt::Machine::pixel_clock - 1) / f3rt::Machine::pixel_clock;
    };
    const auto boundary_at = [&](uint64_t tick) {
        m->cpu.cycles = tick;
        m->boundary();
    };
    const auto first_vblank = raster_tick(f3rt::Machine::frame_pixels);
    boundary_at(first_vblank - 1);
    EXPECT_TRUE(m->frame == 0 && m->pending_irqs == 0)
        << "First vblank waits one full frame from the VBSTART epoch";
    EXPECT_EQ(m->cpu.dispatch_deadline, first_vblank) << "Native deadline is the first vblank event";
    boundary_at(first_vblank);
    EXPECT_TRUE(m->frame == 1 && m->pending_irqs == (1 << 2))
        << "First vblank renders and requests IRQ2 at its deadline";
    EXPECT_EQ(m->cpu.dispatch_deadline, first_vblank + 10000)
        << "Delayed IRQ3 becomes the next native deadline";
    boundary_at(first_vblank + 9999);
    EXPECT_EQ(m->pending_irqs, 1 << 2) << "IRQ3 is not requested before its 10000-cycle delay";
    boundary_at(first_vblank + 10000);
    EXPECT_EQ(m->pending_irqs, (1 << 2) | (1 << 3)) << "IRQ3 is requested at its delayed deadline";
    EXPECT_EQ(m->cpu.dispatch_deadline, raster_tick(2ull * f3rt::Machine::frame_pixels))
        << "Serviced timer deadlines advance to the next absolute frame";
    m->pending_irqs = 0;
    const auto second_vblank = raster_tick(2ull * f3rt::Machine::frame_pixels);
    boundary_at(second_vblank - 1);
    EXPECT_TRUE(m->frame == 1 && m->pending_irqs == 0) << "Next vblank retains the absolute raster phase";
    boundary_at(second_vblank);
    EXPECT_TRUE(m->frame == 2 && m->pending_irqs == (1 << 2)) << "Second vblank uses the full-frame epoch";
}

TEST_F(CpuTest, Movem) {
    for (uint16_t opcode : {0x4891, 0x48d1, 0x48a1, 0x48e1, 0x4c99, 0x4cd9}) {
        const bool load = opcode & 0x0400, wide = opcode & 0x0040, predec = (opcode & 0x0038) == 0x20;
        const unsigned size = wide ? 4 : 2;
        const auto execute = [&](uint16_t mask) {
            m->write16(0x400600, opcode);
            m->write16(0x400602, mask);
            m->cpu.pc = 0x400600;
            m->cpu.sr = 0x2700;
            m->cpu.a[1] = 0x400a20;
            m->cpu.d[0] = 0x12345678;
            m->cpu.d[1] = 0x89abcdef;
            if (load) {
                if (wide) {
                    m->write32(0x400a20, 0x12345678);
                    m->write32(0x400a24, 0x89abcdef);
                } else {
                    m->write16(0x400a20, 0x5678);
                    m->write16(0x400a22, 0xcdef);
                }
            }
            const auto before = m->cpu.cycles;
            m->interpreter->run_main(1);
            EXPECT_EQ(m->cpu.pc, 0x400604) << "MOVEM executes exactly one instruction";
            return m->cpu.cycles - before;
        };
        const auto base = execute(0);
        const auto cycles = execute(predec ? 0xc000 : 3);
        EXPECT_EQ(cycles - base, load ? 8u : 6u)
            << "EC020 MOVEM charges three cycles/store and four/load per register";
        if (load) {
            EXPECT_TRUE(m->cpu.d[0] == (wide ? 0x12345678u : 0x5678u) &&
                        m->cpu.d[1] == (wide ? 0x89abcdefu : 0xffffcdefu) &&
                        m->cpu.a[1] == 0x400a20 + 2 * size)
                << "MOVEM load width, sign extension and postincrement";
        } else {
            const uint32_t address = predec ? 0x400a20 - 2 * size : 0x400a20;
            EXPECT_TRUE((wide ? m->read32(address) : m->read16(address)) == (wide ? 0x12345678u : 0x5678u) &&
                        (wide ? m->read32(address + size) : m->read16(address + size)) == (wide ? 0x89abcdefu : 0xcdefu) &&
                        m->cpu.a[1] == address)
                << "MOVEM store width, register order and predecrement";
        }
    }
}

TEST_F(CpuTest, RotateCycles) {
    const auto execute = [&](uint16_t opcode, unsigned count) {
        m->write16(0x400700, opcode);
        m->cpu.pc = 0x400700;
        m->cpu.sr = 0x2710;
        m->cpu.d[0] = 0x12345678;
        m->cpu.d[1] = count;
        const auto before = m->cpu.cycles;
        m->interpreter->run_main(1);
        EXPECT_EQ(m->cpu.pc, 0x400702) << "Shift/rotate executes one instruction";
        return m->cpu.cycles - before;
    };
    for (unsigned kind = 0; kind < 4; ++kind)
        for (unsigned left = 0; left < 2; ++left)
            for (unsigned size = 0; size < 3; ++size) {
                const uint16_t form = uint16_t(0xe000 | (kind << 3) | (left << 8) | (size << 6));
                const uint16_t reg = uint16_t(form | 0x0220);
                const auto base = execute(reg, 0);
                for (unsigned count : {1, 8, 9, 16, 17, 31, 32, 33, 63})
                    EXPECT_EQ(execute(reg, count), base)
                        << "EC020 register shifts/rotates have no count-dependent cycle surcharge";
                EXPECT_EQ(execute(uint16_t(form | 0x0200), 0), execute(form, 0))
                    << "EC020 immediate counts one and eight have identical timing";
            }
    execute(0xe898, 0); // ROR.L #4,D0, used by the ROM's early boot path.
    EXPECT_TRUE(m->cpu.d[0] == 0x81234567 && (m->cpu.sr & 0x1f) == 0x19)
        << "ROR.L result, carry and preserved extend flag";
}

TEST_F(CpuTest, TrapCycles) {
    const auto old_vbr = m->cpu.vbr;
    for (unsigned trap = 0; trap < 16; ++trap)
        for (bool reference : {false, true}) {
            m->cpu.pc = 0x400700;
            m->cpu.sr = 0x2700;
            m->cpu.a[7] = 0x401000;
            m->cpu.vbr = 0x400000;
            m->write16(0x400700, uint16_t(0x4e40 | trap));
            m->write32(m->cpu.vbr + (32 + trap) * 4, 0x400720);
            const auto before = m->cpu.cycles;
            if (reference) m->interpreter->run_main(1);
            else f3_exception(&m->cpu, 32 + trap, 0x400702);
            EXPECT_TRUE(m->cpu.cycles - before == 24 && m->cpu.pc == 0x400720)
                << "TRAP #n charges 24 cycles before entering its vector";
            EXPECT_TRUE(m->cpu.a[7] == 0x400ff8 && m->read16(m->cpu.a[7]) == 0x2700 &&
                        m->read32(m->cpu.a[7] + 2) == 0x400702 && m->read16(m->cpu.a[7] + 6) == (32 + trap) * 4)
                << "TRAP #n stacks the resume PC in a format-0 frame";
        }
    m->cpu.vbr = old_vbr;
}

TEST_F(CpuTest, WorkRamAndRomMemory) {
    m->write32(0x400001, 0x12345678);
    EXPECT_EQ(m->read32(0x420001), 0x12345678) << "BE misaligned work RAM mirror";
    m->write32(0x41fffe, 0xaabbccdd);
    EXPECT_TRUE(m->read16(0x400000) == 0xccdd && m->read16(0x41fffe) == 0xaabb)
        << "Work RAM wrap across mirror";
    m->write8(0x100, 0xff);
    EXPECT_EQ(m->read16(0x100), 0x702a) << "ROM is read-only";
}

TEST_F(CpuTest, SrMaskAndWatchdogArming) {
    auto &cpu = m->cpu;
    cpu.usp = 0x400800;
    cpu.a[7] = 0x401000;
    cpu.sr = 0x2600;
    EXPECT_TRUE(m->boundary() == 0 && cpu.dispatch_deadline > cpu.cycles)
        << "Runnable boundary publishes a future deadline";
    const auto masked_deadline = cpu.dispatch_deadline;
    f3_set_sr(&cpu, 0x2715);
    EXPECT_EQ(cpu.dispatch_deadline, masked_deadline)
        << "Raising the IRQ mask preserves the event deadline";
    f3_set_sr(&cpu, 0);
    EXPECT_TRUE(cpu.a[7] == 0x400800 && cpu.ssp == 0x401000)
        << "Supervisor to user stack switch";
    EXPECT_EQ(cpu.dispatch_deadline, 0) << "Lowering the IRQ mask invalidates the cached deadline";
    m->write8(0x4a0000, 0);
    EXPECT_EQ(cpu.dispatch_deadline, 0)
        << "Watchdog strobe cannot suppress an outstanding IRQ recheck";
}

TEST_F(CpuTest, IrqRedirectAndFrame) {
    auto &cpu = m->cpu;
    cpu.usp = 0x400800;
    cpu.ssp = 0x401000;
    cpu.a[7] = 0x400800;
    cpu.sr = 0;
    m->write32(0x400000 + 26 * 4, 0x400300);
    cpu.vbr = 0x400000;
    cpu.pc = 0x100;
    cpu.stopped = 1;
    m->pending_irqs = 1 << 2;
    EXPECT_NE(f3_boundary(&cpu), 0) << "IRQ redirects boundary";
    EXPECT_TRUE(cpu.pc == 0x400300 && !cpu.stopped && (cpu.sr & 0x2700) == 0x2200)
        << "IRQ releases STOP and raises mask";
    EXPECT_EQ(cpu.dispatch_deadline, 0) << "IRQ redirect requires lookup through a fresh boundary";
    EXPECT_TRUE(cpu.a[7] == 0x400ff8 && m->read32(cpu.a[7] + 2) == 0x100 && m->read16(cpu.a[7] + 6) == 104)
        << "68020 interrupt frame";
}

TEST_F(CpuTest, ExceptionFramesAndRte) {
    auto &cpu = m->cpu;
    cpu.vbr = 0x400000;
    cpu.a[7] = 0x400ff8;
    m->write32(cpu.vbr + 5 * 4, 0x400310);
    m->write16(0x400310, 0x4e73); // RTE handler
    cpu.pc = 0x100;
    cpu.sr = 0x2700;
    const auto exception_cycles = cpu.cycles;
    const auto exception_sp = cpu.a[7];
    f3_exception(&cpu, 5, 0x102);
    EXPECT_TRUE(cpu.cycles - exception_cycles == 38 && m->read16(cpu.a[7] + 6) == 0x2014 &&
                m->read32(cpu.a[7] + 8) == 0x100)
        << "Divide-by-zero full charge and format-2 instruction PC";
    EXPECT_TRUE(f3_fallback(&cpu) && cpu.pc == 0x102 && cpu.a[7] == exception_sp && cpu.sr == 0x2700)
        << "Real RTE restores the format-2 resume PC and stack";
    EXPECT_TRUE(m->boundary() == 0 && cpu.dispatch_deadline > cpu.cycles)
        << "Restored masked CPU refreshes its event deadline";
    m->write16(0x400320, 0x46fc);
    m->write16(0x400322, 0x2000);
    cpu.pc = 0x400320;
    m->interpreter->run_main(1);
    EXPECT_TRUE(cpu.sr == 0x2000 && cpu.dispatch_deadline == 0)
        << "Interpreted SR lowering also invalidates the native deadline";
}

TEST_F(CpuTest, NativeDispatchFallbackAndExclusions) {
    auto &cpu = m->cpu;
    const f3_block blocks[] = {{0x100, native}};
    EXPECT_EQ(f3_register_blocks(&cpu, blocks, 1), 1) << "Valid block table";
    cpu.pc = 0x100;
    cpu.sr = 0x2700;
    EXPECT_TRUE(f3_dispatch(&cpu) && cpu.d[0] == 99) << "Native dispatch executes matching block";
    cpu.pc = 0x100;
    cpu.sr = 0x2700;
    EXPECT_TRUE(f3_fallback(&cpu) && cpu.d[0] == 42 && cpu.pc == 0x102)
        << "Fallback executes exactly one real instruction";
    cpu.d[0] = 0xdeadbeef;
    cpu.sr = 0x2015;
    const auto reset_cycles = cpu.cycles;
    m->reset_main_cpu();
    EXPECT_TRUE(cpu.d[0] == 0xdeadbeef && cpu.sr == 0x2715 && cpu.pc == 0x100)
        << "Reset preserves canonical native D/CCR, not stale fallback context";
    EXPECT_EQ(cpu.cycles - reset_cycles, 4) << "Warm reset charges its four-cycle latency exactly once";
    m->allow_main_fallback = false;
    const auto fallback_count = m->fallback_instructions;
    bool rejected = false;
    try {
        f3_fallback(&cpu);
    } catch (const std::runtime_error &e) {
        rejected = std::string(e.what()).find("0x100") != std::string::npos;
    }
    EXPECT_TRUE(rejected && cpu.pc == 0x100 && cpu.d[0] == 0xdeadbeef && m->fallback_instructions == fallback_count)
        << "Native-only fallback rejection reports PC without executing or counting an instruction";
    const f3_block bad[] = {{0x100, native}, {0x100, native}};
    EXPECT_FALSE(f3_register_blocks(&cpu, bad, 2)) << "Duplicate PCs rejected";
    const f3_excluded_range excluded[] = {{0x200, 0x204, "synthetic data", "fixture-owned words"}};
    EXPECT_TRUE(f3_register_exclusions(&cpu, excluded, 1)) << "Exclusion complement registers";
    const f3_block overlapping[] = {{0x200, native}};
    EXPECT_FALSE(f3_register_blocks(&cpu, overlapping, 1)) << "Native entries cannot bypass exclusions";
    m->allow_main_fallback = true;
    for (uint32_t pc : {0x200u, 0x201u, 0x202u, 0x203u, 0xff000200u, 0xff000203u}) {
        cpu.pc = pc;
        cpu.halted = 0;
        bool excluded_rejected = false;
        try {
            f3_fallback(&cpu);
        } catch (const std::runtime_error &e) {
            excluded_rejected = std::string(e.what()).find("Excluded main CPU") != std::string::npos;
        }
        EXPECT_TRUE(excluded_rejected && cpu.halted && m->fallback_instructions == fallback_count)
            << "Even, odd and physical-alias excluded targets fail before enabled interpretation";
    }
    cpu.pc = 0x204;
    cpu.halted = 0;
    const auto before_end_fallback = m->fallback_instructions;
    EXPECT_TRUE(f3_fallback(&cpu) && m->fallback_instructions == before_end_fallback + 1)
        << "Exclusive end remains executable";
    EXPECT_EQ(f3_read16(&cpu, 0x200), 0) << "Excluded instruction bytes remain readable as data";
    EXPECT_TRUE(f3_register_exclusions(&cpu, nullptr, 0)) << "Clearing immutable exclusion metadata";
}

TEST_F(CpuTest, WatchdogWholeBoardReset) {
    auto &cpu = m->cpu;
    cpu.usp = 0x400800;
    cpu.a[7] = 0x401000;
    cpu.sr = 0x2600;
    m->boundary();
    f3_set_sr(&cpu, 0);
    m->write8(0x4a0000, 0);
    const auto watchdog_deadline = cpu.cycles + 3ull * f3rt::Machine::main_clock;

    m->audio->write16(0x600, 0xa55a);
    m->audio->set_reset(true);
    m->audio->write8(0x280019, 0x66);
    m->audio->write8(0x260001, 0x12);
    m->audio->write8(0x260003, 0x34);
    m->audio->write8(0x260005, 0x56);
    m->audio->write8(0x260141, 0);
    m->audio->set_reset(false);
    m->audio->set_reset(true);
    m->audio->write8(0x260101, 0);
    m->audio->write32(0, 0xabcdef12);
    cpu.sr = 0x2700;
    cpu.cycles = watchdog_deadline - 1;
    EXPECT_TRUE(m->boundary() == 0 && cpu.dispatch_deadline == watchdog_deadline)
        << "Watchdog expiry can precede the next raster interrupt";
    cpu.stopped = 1;
    EXPECT_TRUE(m->boundary() != 0 && cpu.cycles == watchdog_deadline && cpu.dispatch_deadline == 0)
        << "STOP advances to watchdog expiry without skipping it";
    EXPECT_TRUE(m->boundary() != 0 && m->audio->is_reset())
        << "Watchdog holds the sound CPU in reset";
    EXPECT_EQ(cpu.dispatch_deadline, 0) << "Watchdog reset invalidates the native deadline";
    EXPECT_EQ(m->audio->read8(0x280019), 0x0f) << "Watchdog restores the DUART interrupt vector";
    EXPECT_EQ(m->audio->read16(0x600), 0xa55a) << "Whole-board reset preserves sound work RAM";
    EXPECT_EQ(m->audio->read32(0), 0) << "Whole-board reset reloads boot vectors from sound ROM";
    m->audio->write8(0x260101, 0);
    EXPECT_TRUE(m->audio->read8(0x260001) == 0 && m->audio->read8(0x260003) == 0 &&
                m->audio->read8(0x260005) == 0)
        << "Watchdog clears DSP general-purpose registers";
}

// RapidCheck property: Work RAM 32-bit writes are identically readable from the mirror offset.
RC_GTEST_PROP(Cpu, WorkRamMirrorRoundTrip, ()) {
    const auto offset = (*rc::gen::inRange(0u, 0x20000u / 4)) * 4;
    const auto val = *rc::gen::arbitrary<uint32_t>();
    static auto m = std::make_unique<f3rt::Machine>(fixture());
    m->write32(0x400000 + offset, val);
    const uint32_t mirror_read = m->read32(0x420000 + offset);
    RC_ASSERT(mirror_read == val);
}

// RapidCheck property: Wide 16/32-bit reads match the byte-lane concatenation in RAM.
RC_GTEST_PROP(Cpu, WideBusReadsMatchByteLanes, ()) {
    const auto offset = (*rc::gen::inRange(0u, 0x1ff00u / 4)) * 4;
    static auto m = [] {
        auto mach = std::make_unique<f3rt::Machine>(fixture());
        for (size_t i = 0; i < mach->ram.size(); ++i) mach->ram[i] = uint8_t(i * 37 + 11);
        return mach;
    }();
    const uint32_t a = 0x400000 + offset;
    const uint16_t expected16 = uint16_t((m->read8(a) << 8) | m->read8(a + 1));
    const uint32_t expected32 = (uint32_t(m->read8(a)) << 24) |
                                (uint32_t(m->read8(a + 1)) << 16) |
                                (uint32_t(m->read8(a + 2)) << 8) |
                                uint32_t(m->read8(a + 3));
    RC_ASSERT(m->read16(a) == expected16);
    RC_ASSERT(m->read32(a) == expected32);
}
