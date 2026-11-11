#include "f3rt/machine.hpp"
#include "f3rt/abi/audio.h"
#include "support.hpp"
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <initializer_list>
#include <memory>
#include <string>
#include <vector>

using f3test::fixture;

namespace {
// Native dispatch probe PCs (synthetic table layout, not game addresses).
constexpr uint32_t NATIVE_SPARSE_0 = 0x100;
constexpr uint32_t NATIVE_SPARSE_1 = 0x104;
constexpr uint32_t NATIVE_SPARSE_2 = 0xffe;
constexpr uint32_t NATIVE_SPARSE_3 = 0x1000;
constexpr uint32_t NATIVE_SPARSE_4 = 0x1ffffe;
constexpr uint32_t NATIVE_SPARSE_5 = 0x200000;
constexpr uint32_t NATIVE_ALIAS_OF_0 = 0xff000100;
constexpr uint32_t NATIVE_ODD_PC = 0x101;
constexpr uint32_t DENSE_0 = 0x200;
constexpr uint32_t DENSE_1 = 0x202;
constexpr uint32_t DENSE_2 = 0x204;
constexpr uint32_t DENSE_PAGE_PRE = 0x1fe;
constexpr uint32_t DENSE_ODD_PC = 0x201;
constexpr uint32_t DENSE_PAGE_POST = 0x206;
constexpr uint32_t NATIVE_MISS_BETWEEN_0_AND_1 = 0x102;
constexpr uint32_t NATIVE_MISS_BEFORE_SPARSE_2 = 0xffc;
constexpr uint32_t NATIVE_MISS_AFTER_SPARSE_3 = 0x1002;
constexpr uint32_t NATIVE_MISS_HIGH_PAGE = 0x1100;
constexpr uint32_t NATIVE_MISS_BEFORE_SPARSE_4 = 0x1ffffc;
constexpr uint32_t NATIVE_MISS_PCS[] = {NATIVE_ODD_PC,
                                        NATIVE_MISS_BETWEEN_0_AND_1,
                                        NATIVE_MISS_BEFORE_SPARSE_2,
                                        NATIVE_MISS_AFTER_SPARSE_3,
                                        NATIVE_MISS_HIGH_PAGE,
                                        NATIVE_MISS_BEFORE_SPARSE_4,
                                        NATIVE_SPARSE_5,
                                        NATIVE_ALIAS_OF_0};
// Bus boundaries probed by the wide-access test.
constexpr uint32_t WIDE_BUS_BOUNDARIES[] = {0u,
                                            f3rt::ROM_END,
                                            f3rt::RAM_BASE,
                                            f3rt::RAM_PHYSICAL_END,
                                            f3rt::RAM_END,
                                            f3rt::PALETTE_END,
                                            f3rt::COIN_BANK0,
                                            f3rt::EEPROM_PINS,
                                            f3rt::TIMER_CONTROL,
                                            f3rt::GRAPHICS_BASE,
                                            f3rt::GRAPHICS_END,
                                            f3rt::CONTROL_BASE,
                                            f3rt::SHARED_BASE,
                                            f3rt::SHARED_END,
                                            f3rt::SOUND_RESET_RELEASE,
                                            f3rt::SOUND_RESET_ASSERT,
                                            f3rt::SOUND_RAM_HIGH_END};
constexpr uint32_t BUS_ALIAS_HIGH = 0x5a000000;
constexpr size_t ROM_PROBE_OFFSET = 0x104;
constexpr uint32_t TEST_RESET_PC = 0x100;
constexpr uint32_t WORK_USP = f3rt::RAM_BASE + 0x800;          // user stack pointer
constexpr uint32_t WORK_STACK_TOP = f3rt::RAM_BASE + 0x1000;   // supervisor stack top
constexpr uint32_t WORK_EXCEPTION_SP = f3rt::RAM_BASE + 0xff8; // SP after one exception frame
constexpr uint32_t WORK_TRAP_CODE = f3rt::RAM_BASE + 0x700;    // TRAP #n test instruction
constexpr uint32_t WORK_TRAP_HANDLER = f3rt::RAM_BASE + 0x720; // TRAP vector target
constexpr uint32_t WORK_IRQ_HANDLER = f3rt::RAM_BASE + 0x300;  // IRQ vector target
constexpr uint32_t WORK_RTE_HANDLER = f3rt::RAM_BASE + 0x310;  // exception vector target
constexpr uint32_t ROM_WRITE_PROBE = 0x100;                    // ROM byte probed for read-only
constexpr uint32_t EXCLUDED_START = 0x200;                     // synthetic excluded range
constexpr uint32_t EXCLUDED_END = 0x204;
constexpr uint32_t EXCLUDED_ALIAS_BASE = 0xff000200; // physical alias of EXCLUDED_START
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

// The audio board and EEPROM are process-wide: the RapidCheck properties share one machine, and every
// test that builds its own releases it first.
std::unique_ptr<f3rt::Machine> g_shared_machine;

f3rt::Machine &shared_machine() {
    if (!g_shared_machine) {
        g_shared_machine = std::make_unique<f3rt::Machine>(fixture());
        for (size_t i = 0; i < g_shared_machine->ram.size(); ++i)
            g_shared_machine->ram[i] = uint8_t(i * 37 + 11);
    }
    return *g_shared_machine;
}

std::unique_ptr<f3rt::Machine> make_machine() {
    g_shared_machine.reset();
    return std::make_unique<f3rt::Machine>(fixture());
}

class CpuTest : public ::testing::Test {
protected:
    std::unique_ptr<f3rt::Machine> m_m;

    void SetUp() override { m_m = make_machine(); }
};
} // namespace

TEST(Cpu, NativeLookup) {
    auto m = make_machine();
    auto &cpu = m->cpu;
    const auto hit = [&](uint32_t pc, uint32_t value) {
        cpu.pc = pc;
        cpu.sr = 0x2700;
        cpu.d[0] = 0;
        const auto count = m->native_blocks;
        EXPECT_TRUE(f3_dispatch(&cpu) && cpu.pc == pc + 2 && cpu.d[0] == value &&
                    m->native_blocks == count + 1)
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
        EXPECT_TRUE(rejected && cpu.pc == pc && m->native_blocks == count)
            << "Page gaps, odd PCs and aliases must not execute a nearby entry";
    };
    const f3_block sparse[] = {{NATIVE_SPARSE_0, native}, {NATIVE_SPARSE_1, native_other},
                               {NATIVE_SPARSE_2, native}, {NATIVE_SPARSE_3, native_other},
                               {NATIVE_SPARSE_4, native}, {NATIVE_SPARSE_5, native_other}};
    EXPECT_TRUE(f3_register_blocks(&cpu, sparse, std::size(sparse)))
        << "Sparse ROM table registers";
    hit(NATIVE_SPARSE_0, 99);
    hit(NATIVE_SPARSE_1, 17);
    hit(NATIVE_SPARSE_2, 99);
    hit(NATIVE_SPARSE_3, 17);
    hit(NATIVE_SPARSE_4, 99);
    for (uint32_t pc : NATIVE_MISS_PCS) miss(pc);
    const f3_block duplicate[] = {{NATIVE_SPARSE_0, native}, {NATIVE_SPARSE_0, native_other}};
    const f3_block unsorted[] = {{NATIVE_SPARSE_1, native}, {NATIVE_SPARSE_0, native_other}};
    const f3_block odd[] = {{NATIVE_ODD_PC, native}}, missing[] = {{NATIVE_SPARSE_0, nullptr}};
    EXPECT_TRUE(!f3_register_blocks(&cpu, duplicate, 2) && !f3_register_blocks(&cpu, unsorted, 2) &&
                !f3_register_blocks(&cpu, odd, 1) && !f3_register_blocks(&cpu, missing, 1))
        << "Invalid replacement tables are rejected atomically";
    hit(NATIVE_SPARSE_1, 17);
    const f3_block dense[] = {{DENSE_0, native}, {DENSE_1, native_other}, {DENSE_2, native}};
    EXPECT_TRUE(f3_register_blocks(&cpu, dense, 3)) << "Dense partial page replaces sparse table";
    hit(DENSE_0, 99);
    hit(DENSE_1, 17);
    hit(DENSE_2, 99);
    miss(NATIVE_SPARSE_0);
    miss(DENSE_PAGE_PRE);
    miss(DENSE_ODD_PC);
    miss(DENSE_PAGE_POST);
    m->reset();
    hit(DENSE_1, 17);
    std::vector<uint8_t> saved(m->state_size());
    m->save_state(saved);
    hit(DENSE_2, 99);
    m->load_state(saved);
    hit(DENSE_1, 17);
    cpu.pc = DENSE_1;
    cpu.sr = 0xa700;
    bool traced = false;
    try {
        f3_dispatch(&cpu);
    } catch (const std::runtime_error &) {
        traced = true;
    }
    EXPECT_TRUE(traced && cpu.pc == DENSE_1)
        << "Trace mode still rejects block execution in strict-native mode";
    EXPECT_TRUE(f3_register_blocks(&cpu, nullptr, 0))
        << "Empty table replaces indexed registration";
    miss(DENSE_1);
}

// The audio board and EEPROM are process-wide, so one machine plays both parts: `wide` and `bytes` are its
// complete saved states, swapped in around each access.
TEST(Cpu, WideBusBoundaries) {
    auto m = make_machine();
    const auto seed = [](auto &region) {
        for (size_t i = 0; i < region.size(); ++i) region[i] = uint8_t(i * 37 + 11);
    };
    seed(m->roms.main);
    seed(m->ram);
    seed(m->palette);
    seed(m->graphics);
    seed(m->shared);
    std::vector<uint8_t> wide_state(m->state_size());
    m->save_state(wide_state);
    std::vector<uint8_t> bytes_state = wide_state;
    const auto on = [&](std::vector<uint8_t> &state, auto &&access) {
        m->load_state(state);
        access();
        m->save_state(state);
    };
    for (uint32_t boundary : WIDE_BUS_BOUNDARIES) {
        for (uint32_t alias : {0u, BUS_ALIAS_HIGH})
            for (unsigned delta = 0; delta < 7; ++delta) {
                const uint32_t a = boundary - 3 + delta + alias;
                uint16_t expected16 = 0;
                uint32_t expected32 = 0;
                on(bytes_state, [&] {
                    expected16 = uint16_t(uint16_t(m->read8(a)) << 8 | m->read8(a + 1));
                    for (unsigned i = 0; i < 4; ++i)
                        expected32 = (expected32 << 8) | m->read8(a + i);
                });
                uint16_t wide16 = 0;
                uint32_t wide32 = 0;
                on(wide_state, [&] {
                    wide16 = m->read16(a);
                    wide32 = m->read32(a);
                });
                EXPECT_TRUE(wide16 == expected16 && wide32 == expected32)
                    << "Wide reads preserve byte order across regions, mirrors and address "
                       "rollover";
                on(wide_state, [&] {
                    m->write16(a, 0xa1b2);
                    m->write32(a, 0x31415926);
                });
                on(bytes_state, [&] {
                    m->write8(a, 0xa1);
                    m->write8(a + 1, 0xb2);
                    for (unsigned i = 0; i < 4; ++i)
                        m->write8(a + i, uint8_t(0x31415926u >> (24 - i * 8)));
                });
                EXPECT_TRUE(wide_state == bytes_state)
                    << "Wide writes preserve ordered byte effects at memory and MMIO boundaries";
            }
    }
    EXPECT_TRUE(wide_state == bytes_state)
        << "Wide access preserves complete device state, including reset and EEPROM side effects";
}

TEST_F(CpuTest, MainRomBindingAndColdReset) {
    const uint32_t main_crc = f3rt::crc32(m_m->roms.main.data(), m_m->roms.main.size());
    EXPECT_TRUE(f3_validate_main_rom(&m_m->cpu, m_m->roms.main.size(), main_crc))
        << "Native main image binding accepts the loaded revision";
    m_m->roms.main[ROM_PROBE_OFFSET] = 1;
    bool mismatched = false;
    try {
        f3_validate_main_rom(&m_m->cpu, m_m->roms.main.size(), main_crc);
    } catch (const std::runtime_error &) {
        mismatched = true;
    }
    EXPECT_TRUE(mismatched && !m_m->blocks)
        << "Wrong native image is rejected before dispatch registration";
    m_m->roms.main[ROM_PROBE_OFFSET] = 0;
    mismatched = false;
    try {
        f3_validate_main_rom(&m_m->cpu, 0x100000, main_crc);
    } catch (const std::runtime_error &) {
        mismatched = true;
    }
    EXPECT_TRUE(mismatched) << "Native image length is bound even when its CRC matches";
    EXPECT_TRUE(m_m->cpu.cycles == 4 && m_m->cpu.pc == TEST_RESET_PC && m_m->cpu.d[0] == 0)
        << "Cold reset charges four cycles without executing the first opcode";
    EXPECT_EQ(m_m->cpu.dispatch_deadline, 0) << "Reset requires a fresh scheduling boundary";
}

TEST_F(CpuTest, VblankSchedule) {
    const auto raster_tick = [](uint64_t pixels) {
        return (pixels * f3rt::Machine::MAIN_CLOCK + f3rt::Machine::PIXEL_CLOCK - 1) /
               f3rt::Machine::PIXEL_CLOCK;
    };
    const auto boundary_at = [&](uint64_t tick) {
        m_m->cpu.cycles = tick;
        m_m->boundary();
    };
    const auto first_vblank = raster_tick(f3rt::Machine::FRAME_PIXELS);
    boundary_at(first_vblank - 1);
    EXPECT_TRUE(m_m->frame == 0 && m_m->pending_irqs == 0)
        << "First vblank waits one full frame from the VBSTART epoch";
    EXPECT_EQ(m_m->cpu.dispatch_deadline, first_vblank)
        << "Native deadline is the first vblank event";
    boundary_at(first_vblank);
    EXPECT_TRUE(m_m->frame == 1 && m_m->pending_irqs == (1 << 2))
        << "First vblank renders and requests IRQ2 at its deadline";
    EXPECT_EQ(m_m->cpu.dispatch_deadline, first_vblank + 10000)
        << "Delayed IRQ3 becomes the next native deadline";
    boundary_at(first_vblank + 9999);
    EXPECT_EQ(m_m->pending_irqs, 1 << 2) << "IRQ3 is not requested before its 10000-cycle delay";
    boundary_at(first_vblank + 10000);
    EXPECT_EQ(m_m->pending_irqs, (1 << 2) | (1 << 3))
        << "IRQ3 is requested at its delayed deadline";
    EXPECT_EQ(m_m->cpu.dispatch_deadline, raster_tick(2ull * f3rt::Machine::FRAME_PIXELS))
        << "Serviced timer deadlines advance to the next absolute frame";
    m_m->pending_irqs = 0;
    const auto second_vblank = raster_tick(2ull * f3rt::Machine::FRAME_PIXELS);
    boundary_at(second_vblank - 1);
    EXPECT_TRUE(m_m->frame == 1 && m_m->pending_irqs == 0)
        << "Next vblank retains the absolute raster phase";
    boundary_at(second_vblank);
    EXPECT_TRUE(m_m->frame == 2 && m_m->pending_irqs == (1 << 2))
        << "Second vblank uses the full-frame epoch";
}

TEST_F(CpuTest, TrapCycles) {
    const auto old_vbr = m_m->cpu.vbr;
    for (unsigned trap = 0; trap < 16; ++trap) {
        {
            m_m->cpu.pc = WORK_TRAP_CODE;
            m_m->cpu.sr = 0x2700;
            m_m->cpu.a[7] = WORK_STACK_TOP;
            m_m->cpu.vbr = f3rt::RAM_BASE;
            m_m->write16(WORK_TRAP_CODE, uint16_t(0x4e40 | trap));
            m_m->write32(m_m->cpu.vbr + (32 + trap) * 4, WORK_TRAP_HANDLER);
            const auto before = m_m->cpu.cycles;
            f3_exception(&m_m->cpu, 32 + trap, WORK_TRAP_CODE + 2);
            EXPECT_TRUE(m_m->cpu.cycles - before == 24 && m_m->cpu.pc == WORK_TRAP_HANDLER)
                << "TRAP #n charges 24 cycles before entering its vector";
            EXPECT_TRUE(m_m->cpu.a[7] == WORK_EXCEPTION_SP &&
                        m_m->read16(m_m->cpu.a[7]) == 0x2700 &&
                        m_m->read32(m_m->cpu.a[7] + 2) == WORK_TRAP_CODE + 2 &&
                        m_m->read16(m_m->cpu.a[7] + 6) == (32 + trap) * 4)
                << "TRAP #n stacks the resume PC in a format-0 frame";
        }
    }
    m_m->cpu.vbr = old_vbr;
}

TEST_F(CpuTest, WorkRamAndRomMemory) {
    m_m->write32(f3rt::RAM_BASE + 1, 0x12345678);
    EXPECT_EQ(m_m->read32(f3rt::RAM_PHYSICAL_END + 1), 0x12345678)
        << "BE misaligned work RAM mirror";
    m_m->write32(f3rt::RAM_PHYSICAL_END - 2, 0xaabbccdd);
    EXPECT_TRUE(m_m->read16(f3rt::RAM_BASE) == 0xccdd &&
                m_m->read16(f3rt::RAM_PHYSICAL_END - 2) == 0xaabb)
        << "Work RAM wrap across mirror";
    m_m->write8(ROM_WRITE_PROBE, 0xff);
    EXPECT_EQ(m_m->read16(ROM_WRITE_PROBE), 0x702a) << "ROM is read-only";
}

TEST_F(CpuTest, SrMaskAndWatchdogArming) {
    auto &cpu = m_m->cpu;
    cpu.usp = WORK_USP;
    cpu.a[7] = WORK_STACK_TOP;
    cpu.sr = 0x2600;
    EXPECT_TRUE(m_m->boundary() == 0 && cpu.dispatch_deadline > cpu.cycles)
        << "Runnable boundary publishes a future deadline";
    const auto masked_deadline = cpu.dispatch_deadline;
    f3_set_sr(&cpu, 0x2715);
    EXPECT_EQ(cpu.dispatch_deadline, masked_deadline)
        << "Raising the IRQ mask preserves the event deadline";
    f3_set_sr(&cpu, 0);
    EXPECT_TRUE(cpu.a[7] == WORK_USP && cpu.ssp == WORK_STACK_TOP)
        << "Supervisor to user stack switch";
    EXPECT_EQ(cpu.dispatch_deadline, 0) << "Lowering the IRQ mask invalidates the cached deadline";
    m_m->write8(f3rt::WATCHDOG, 0);
    EXPECT_EQ(cpu.dispatch_deadline, 0)
        << "Watchdog strobe cannot suppress an outstanding IRQ recheck";
}

TEST_F(CpuTest, IrqRedirectAndFrame) {
    auto &cpu = m_m->cpu;
    cpu.usp = WORK_USP;
    cpu.ssp = WORK_STACK_TOP;
    cpu.a[7] = WORK_USP;
    cpu.sr = 0;
    m_m->write32(f3rt::RAM_BASE + 26 * 4, WORK_IRQ_HANDLER);
    cpu.vbr = f3rt::RAM_BASE;
    cpu.pc = TEST_RESET_PC;
    cpu.stopped = 1;
    m_m->pending_irqs = 1 << 2;
    EXPECT_NE(f3_boundary(&cpu), 0) << "IRQ redirects boundary";
    EXPECT_TRUE(cpu.pc == WORK_IRQ_HANDLER && !cpu.stopped && (cpu.sr & 0x2700) == 0x2200)
        << "IRQ releases STOP and raises mask";
    EXPECT_EQ(cpu.dispatch_deadline, 0) << "IRQ redirect requires lookup through a fresh boundary";
    EXPECT_TRUE(cpu.a[7] == WORK_EXCEPTION_SP && m_m->read32(cpu.a[7] + 2) == TEST_RESET_PC &&
                m_m->read16(cpu.a[7] + 6) == 104)
        << "68020 interrupt frame";
}

TEST_F(CpuTest, ExceptionFrames) {
    auto &cpu = m_m->cpu;
    cpu.vbr = f3rt::RAM_BASE;
    cpu.a[7] = WORK_EXCEPTION_SP;
    m_m->write32(cpu.vbr + 5 * 4, WORK_RTE_HANDLER);
    cpu.pc = TEST_RESET_PC;
    cpu.sr = 0x2700;
    const auto exception_cycles = cpu.cycles;
    f3_exception(&cpu, 5, TEST_RESET_PC + 2);
    EXPECT_TRUE(cpu.cycles - exception_cycles == 38 && m_m->read16(cpu.a[7] + 6) == 0x2014 &&
                m_m->read32(cpu.a[7] + 8) == TEST_RESET_PC)
        << "Divide-by-zero full charge and format-2 instruction PC";
}

TEST_F(CpuTest, NativeDispatchFallbackAndExclusions) {
    auto &cpu = m_m->cpu;
    const f3_block blocks[] = {{TEST_RESET_PC, native}};
    EXPECT_EQ(f3_register_blocks(&cpu, blocks, 1), 1) << "Valid block table";
    cpu.pc = TEST_RESET_PC;
    cpu.sr = 0x2700;
    EXPECT_TRUE(f3_dispatch(&cpu) && cpu.d[0] == 99) << "Native dispatch executes matching block";
    cpu.d[0] = 0xdeadbeef;
    cpu.sr = 0x2015;
    const auto reset_cycles = cpu.cycles;
    m_m->reset_main_cpu();
    EXPECT_TRUE(cpu.d[0] == 0xdeadbeef && cpu.sr == 0x2715 && cpu.pc == TEST_RESET_PC)
        << "Reset preserves canonical native D/CCR";
    EXPECT_EQ(cpu.cycles - reset_cycles, 4)
        << "Warm reset charges its four-cycle latency exactly once";
    bool rejected = false;
    try {
        f3_fallback(&cpu);
    } catch (const std::runtime_error &e) {
        rejected = std::string(e.what()).find("0x100") != std::string::npos;
    }
    EXPECT_TRUE(rejected && cpu.pc == TEST_RESET_PC && cpu.d[0] == 0xdeadbeef)
        << "An untranslated instruction is rejected with its PC and does not execute";
    const f3_block bad[] = {{TEST_RESET_PC, native}, {TEST_RESET_PC, native}};
    EXPECT_FALSE(f3_register_blocks(&cpu, bad, 2)) << "Duplicate PCs rejected";
    const f3_excluded_range excluded[] = {
        {EXCLUDED_START, EXCLUDED_END, "synthetic data", "fixture-owned words"}};
    EXPECT_TRUE(f3_register_exclusions(&cpu, excluded, 1)) << "Exclusion complement registers";
    const f3_block overlapping[] = {{EXCLUDED_START, native}};
    EXPECT_FALSE(f3_register_blocks(&cpu, overlapping, 1))
        << "Native entries cannot bypass exclusions";
    for (uint32_t pc : {EXCLUDED_START, EXCLUDED_START + 1, EXCLUDED_START + 2, EXCLUDED_START + 3,
                        EXCLUDED_ALIAS_BASE, EXCLUDED_ALIAS_BASE + 3}) {
        cpu.pc = pc;
        cpu.halted = 0;
        bool excluded_rejected = false;
        try {
            f3_fallback(&cpu);
        } catch (const std::runtime_error &e) {
            excluded_rejected =
                std::string(e.what()).find("Excluded main CPU") != std::string::npos;
        }
        EXPECT_TRUE(excluded_rejected && cpu.halted)
            << "Even, odd and physical-alias excluded targets fail as excluded code";
    }
    cpu.pc = EXCLUDED_END;
    cpu.halted = 0;
    bool end_untranslated = false;
    try {
        f3_fallback(&cpu);
    } catch (const std::runtime_error &e) {
        end_untranslated = std::string(e.what()).find("Untranslated main CPU") != std::string::npos;
    }
    EXPECT_TRUE(end_untranslated) << "Exclusive end is not excluded code";
    EXPECT_EQ(f3_read16(&cpu, EXCLUDED_START), 0)
        << "Excluded instruction bytes remain readable as data";
    EXPECT_TRUE(f3_register_exclusions(&cpu, nullptr, 0))
        << "Clearing immutable exclusion metadata";
}

TEST_F(CpuTest, WatchdogWholeBoardReset) {
    auto &cpu = m_m->cpu;
    cpu.usp = WORK_USP;
    cpu.a[7] = WORK_STACK_TOP;
    cpu.sr = 0x2600;
    m_m->boundary();
    f3_set_sr(&cpu, 0);
    m_m->write8(f3rt::WATCHDOG, 0);
    const auto watchdog_deadline = cpu.cycles + 3ull * f3rt::Machine::MAIN_CLOCK;

    f3rt_audio_set_reset(false);
    cpu.sr = 0x2700;
    cpu.cycles = watchdog_deadline - 1;
    EXPECT_TRUE(m_m->boundary() == 0 && cpu.dispatch_deadline == watchdog_deadline)
        << "Watchdog expiry can precede the next raster interrupt";
    cpu.stopped = 1;
    EXPECT_TRUE(m_m->boundary() != 0 && cpu.cycles == watchdog_deadline &&
                cpu.dispatch_deadline == 0)
        << "STOP advances to watchdog expiry without skipping it";
    EXPECT_TRUE(m_m->boundary() != 0) << "Watchdog expiry resets the board";
    EXPECT_EQ(cpu.dispatch_deadline, 0) << "Watchdog reset invalidates the native deadline";
}

// RapidCheck property: Work RAM 32-bit writes are identically readable from the mirror offset.
RC_GTEST_PROP(Cpu, WorkRamMirrorRoundTrip, ()) {
    const auto offset = (*rc::gen::inRange(0u, 0x20000u / 4)) * 4;
    const auto val = *rc::gen::arbitrary<uint32_t>();
    auto &m = shared_machine();
    m.write32(f3rt::RAM_BASE + offset, val);
    const uint32_t mirror_read = m.read32(f3rt::RAM_PHYSICAL_END + offset);
    RC_ASSERT(mirror_read == val);
}

// RapidCheck property: Wide 16/32-bit reads match the byte-lane concatenation in RAM.
RC_GTEST_PROP(Cpu, WideBusReadsMatchByteLanes, ()) {
    const auto offset = (*rc::gen::inRange(0u, 0x1ff00u / 4)) * 4;
    auto &m = shared_machine();
    const uint32_t a = f3rt::RAM_BASE + offset;
    const uint16_t expected16 = uint16_t((m.read8(a) << 8) | m.read8(a + 1));
    const uint32_t expected32 = (uint32_t(m.read8(a)) << 24) | (uint32_t(m.read8(a + 1)) << 16) |
                                (uint32_t(m.read8(a + 2)) << 8) | uint32_t(m.read8(a + 3));
    RC_ASSERT(m.read16(a) == expected16);
    RC_ASSERT(m.read32(a) == expected32);
}
