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
constexpr uint32_t NATIVE_MISS_PCS[] = {NATIVE_ODD_PC, NATIVE_MISS_BETWEEN_0_AND_1, NATIVE_MISS_BEFORE_SPARSE_2,
                                        NATIVE_MISS_AFTER_SPARSE_3, NATIVE_MISS_HIGH_PAGE, NATIVE_MISS_BEFORE_SPARSE_4,
                                        NATIVE_SPARSE_5, NATIVE_ALIAS_OF_0};
// Bus boundaries probed by the wide-access test.
constexpr uint32_t WIDE_BUS_BOUNDARIES[] = {0u, f3rt::ROM_END, f3rt::RAM_BASE, f3rt::RAM_PHYSICAL_END, f3rt::RAM_END,
                                            f3rt::PALETTE_END, f3rt::COIN_BANK0, f3rt::EEPROM_PINS, f3rt::TIMER_CONTROL,
                                            f3rt::GRAPHICS_BASE, f3rt::GRAPHICS_END, f3rt::CONTROL_BASE, f3rt::SHARED_BASE,
                                            f3rt::SHARED_END, f3rt::SOUND_RESET_RELEASE, f3rt::SOUND_RESET_ASSERT,
                                            f3rt::SOUND_RAM_HIGH_END};
constexpr uint32_t BUS_ALIAS_HIGH = 0x5a000000;
constexpr size_t ROM_PROBE_OFFSET = 0x104;
constexpr uint32_t TEST_RESET_PC = 0x100;
constexpr uint32_t WORK_MOVEM_CODE = f3rt::RAM_BASE + 0x600;
constexpr uint32_t WORK_MOVEM_DATA = f3rt::RAM_BASE + 0xa20;
constexpr uint32_t WORK_ROTATE_CODE = f3rt::RAM_BASE + 0x700;
constexpr uint32_t WORK_USP = f3rt::RAM_BASE + 0x800;           // user stack pointer
constexpr uint32_t WORK_STACK_TOP = f3rt::RAM_BASE + 0x1000;    // supervisor stack top
constexpr uint32_t WORK_EXCEPTION_SP = f3rt::RAM_BASE + 0xff8;  // SP after one exception frame
constexpr uint32_t WORK_TRAP_CODE = f3rt::RAM_BASE + 0x700;     // TRAP #n test instruction
constexpr uint32_t WORK_TRAP_HANDLER = f3rt::RAM_BASE + 0x720;  // TRAP vector target
constexpr uint32_t WORK_IRQ_HANDLER = f3rt::RAM_BASE + 0x300;   // IRQ vector target
constexpr uint32_t WORK_RTE_HANDLER = f3rt::RAM_BASE + 0x310;   // RTE handler
constexpr uint32_t WORK_LOW_SR_CODE = f3rt::RAM_BASE + 0x320;   // MOVE #imm,SR test code
constexpr uint32_t ROM_WRITE_PROBE = 0x100;                     // ROM byte probed for read-only
constexpr uint32_t SND_WORK_RAM = 0x600;                        // sound work RAM probe
constexpr uint32_t DUART_IVR = f3rt::SOUND_DUART_BASE + 0x19;   // DUART interrupt vector register
constexpr uint32_t EXCLUDED_START = 0x200;                      // synthetic excluded range
constexpr uint32_t EXCLUDED_END = 0x204;
constexpr uint32_t EXCLUDED_ALIAS_BASE = 0xff000200;            // physical alias of EXCLUDED_START
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
    const f3_block sparse[] = {{NATIVE_SPARSE_0, native}, {NATIVE_SPARSE_1, native_other}, {NATIVE_SPARSE_2, native},
                              {NATIVE_SPARSE_3, native_other}, {NATIVE_SPARSE_4, native}, {NATIVE_SPARSE_5, native_other}};
    EXPECT_TRUE(f3_register_blocks(&cpu, sparse, std::size(sparse))) << "Sparse ROM table registers";
    hit(NATIVE_SPARSE_0, 99);
    hit(NATIVE_SPARSE_1, 17);
    hit(NATIVE_SPARSE_2, 99);
    hit(NATIVE_SPARSE_3, 17);
    hit(NATIVE_SPARSE_4, 99);
    for (uint32_t pc : NATIVE_MISS_PCS)
        miss(pc);
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
    EXPECT_TRUE(traced && cpu.pc == DENSE_1) << "Trace mode still rejects block execution in strict-native mode";
    EXPECT_TRUE(f3_register_blocks(&cpu, nullptr, 0)) << "Empty table replaces indexed registration";
    miss(DENSE_1);
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
    for (uint32_t boundary : WIDE_BUS_BOUNDARIES) {
        for (uint32_t alias : {0u, BUS_ALIAS_HIGH})
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
    m->roms.main[ROM_PROBE_OFFSET] = 1;
    bool mismatched = false;
    try {
        f3_validate_main_rom(&m->cpu, m->roms.main.size(), main_crc);
    } catch (const std::runtime_error &) {
        mismatched = true;
    }
    EXPECT_TRUE(mismatched && !m->blocks)
        << "Wrong native image is rejected before dispatch registration";
    m->roms.main[ROM_PROBE_OFFSET] = 0;
    mismatched = false;
    try {
        f3_validate_main_rom(&m->cpu, 0x100000, main_crc);
    } catch (const std::runtime_error &) {
        mismatched = true;
    }
    EXPECT_TRUE(mismatched) << "Native image length is bound even when its CRC matches";
    EXPECT_TRUE(m->cpu.cycles == 4 && m->cpu.pc == TEST_RESET_PC && m->cpu.d[0] == 0)
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
            m->write16(WORK_MOVEM_CODE, opcode);
            m->write16(WORK_MOVEM_CODE + 2, mask);
            m->cpu.pc = WORK_MOVEM_CODE;
            m->cpu.sr = 0x2700;
            m->cpu.a[1] = WORK_MOVEM_DATA;
            m->cpu.d[0] = 0x12345678;
            m->cpu.d[1] = 0x89abcdef;
            if (load) {
                if (wide) {
                    m->write32(WORK_MOVEM_DATA, 0x12345678);
                    m->write32(WORK_MOVEM_DATA + 4, 0x89abcdef);
                } else {
                    m->write16(WORK_MOVEM_DATA, 0x5678);
                    m->write16(WORK_MOVEM_DATA + 2, 0xcdef);
                }
            }
            const auto before = m->cpu.cycles;
            m->interpreter->run_main(1);
            EXPECT_EQ(m->cpu.pc, WORK_MOVEM_CODE + 4) << "MOVEM executes exactly one instruction";
            return m->cpu.cycles - before;
        };
        const auto base = execute(0);
        const auto cycles = execute(predec ? 0xc000 : 3);
        EXPECT_EQ(cycles - base, load ? 8u : 6u)
            << "EC020 MOVEM charges three cycles/store and four/load per register";
        if (load) {
            EXPECT_TRUE(m->cpu.d[0] == (wide ? 0x12345678u : 0x5678u) &&
                        m->cpu.d[1] == (wide ? 0x89abcdefu : 0xffffcdefu) &&
                        m->cpu.a[1] == WORK_MOVEM_DATA + 2 * size)
                << "MOVEM load width, sign extension and postincrement";
        } else {
            const uint32_t address = predec ? WORK_MOVEM_DATA - 2 * size : WORK_MOVEM_DATA;
            EXPECT_TRUE((wide ? m->read32(address) : m->read16(address)) == (wide ? 0x12345678u : 0x5678u) &&
                        (wide ? m->read32(address + size) : m->read16(address + size)) == (wide ? 0x89abcdefu : 0xcdefu) &&
                        m->cpu.a[1] == address)
                << "MOVEM store width, register order and predecrement";
        }
    }
}

TEST_F(CpuTest, RotateCycles) {
    const auto execute = [&](uint16_t opcode, unsigned count) {
        m->write16(WORK_ROTATE_CODE, opcode);
        m->cpu.pc = WORK_ROTATE_CODE;
        m->cpu.sr = 0x2710;
        m->cpu.d[0] = 0x12345678;
        m->cpu.d[1] = count;
        const auto before = m->cpu.cycles;
        m->interpreter->run_main(1);
        EXPECT_EQ(m->cpu.pc, WORK_ROTATE_CODE + 2) << "Shift/rotate executes one instruction";
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
            m->cpu.pc = WORK_TRAP_CODE;
            m->cpu.sr = 0x2700;
            m->cpu.a[7] = WORK_STACK_TOP;
            m->cpu.vbr = f3rt::RAM_BASE;
            m->write16(WORK_TRAP_CODE, uint16_t(0x4e40 | trap));
            m->write32(m->cpu.vbr + (32 + trap) * 4, WORK_TRAP_HANDLER);
            const auto before = m->cpu.cycles;
            if (reference) m->interpreter->run_main(1);
            else f3_exception(&m->cpu, 32 + trap, WORK_TRAP_CODE + 2);
            EXPECT_TRUE(m->cpu.cycles - before == 24 && m->cpu.pc == WORK_TRAP_HANDLER)
                << "TRAP #n charges 24 cycles before entering its vector";
            EXPECT_TRUE(m->cpu.a[7] == WORK_EXCEPTION_SP && m->read16(m->cpu.a[7]) == 0x2700 &&
                        m->read32(m->cpu.a[7] + 2) == WORK_TRAP_CODE + 2 && m->read16(m->cpu.a[7] + 6) == (32 + trap) * 4)
                << "TRAP #n stacks the resume PC in a format-0 frame";
        }
    m->cpu.vbr = old_vbr;
}

TEST_F(CpuTest, WorkRamAndRomMemory) {
    m->write32(f3rt::RAM_BASE + 1, 0x12345678);
    EXPECT_EQ(m->read32(f3rt::RAM_PHYSICAL_END + 1), 0x12345678) << "BE misaligned work RAM mirror";
    m->write32(f3rt::RAM_PHYSICAL_END - 2, 0xaabbccdd);
    EXPECT_TRUE(m->read16(f3rt::RAM_BASE) == 0xccdd && m->read16(f3rt::RAM_PHYSICAL_END - 2) == 0xaabb)
        << "Work RAM wrap across mirror";
    m->write8(ROM_WRITE_PROBE, 0xff);
    EXPECT_EQ(m->read16(ROM_WRITE_PROBE), 0x702a) << "ROM is read-only";
}

TEST_F(CpuTest, SrMaskAndWatchdogArming) {
    auto &cpu = m->cpu;
    cpu.usp = WORK_USP;
    cpu.a[7] = WORK_STACK_TOP;
    cpu.sr = 0x2600;
    EXPECT_TRUE(m->boundary() == 0 && cpu.dispatch_deadline > cpu.cycles)
        << "Runnable boundary publishes a future deadline";
    const auto masked_deadline = cpu.dispatch_deadline;
    f3_set_sr(&cpu, 0x2715);
    EXPECT_EQ(cpu.dispatch_deadline, masked_deadline)
        << "Raising the IRQ mask preserves the event deadline";
    f3_set_sr(&cpu, 0);
    EXPECT_TRUE(cpu.a[7] == WORK_USP && cpu.ssp == WORK_STACK_TOP)
        << "Supervisor to user stack switch";
    EXPECT_EQ(cpu.dispatch_deadline, 0) << "Lowering the IRQ mask invalidates the cached deadline";
    m->write8(f3rt::WATCHDOG, 0);
    EXPECT_EQ(cpu.dispatch_deadline, 0)
        << "Watchdog strobe cannot suppress an outstanding IRQ recheck";
}

TEST_F(CpuTest, IrqRedirectAndFrame) {
    auto &cpu = m->cpu;
    cpu.usp = WORK_USP;
    cpu.ssp = WORK_STACK_TOP;
    cpu.a[7] = WORK_USP;
    cpu.sr = 0;
    m->write32(f3rt::RAM_BASE + 26 * 4, WORK_IRQ_HANDLER);
    cpu.vbr = f3rt::RAM_BASE;
    cpu.pc = TEST_RESET_PC;
    cpu.stopped = 1;
    m->pending_irqs = 1 << 2;
    EXPECT_NE(f3_boundary(&cpu), 0) << "IRQ redirects boundary";
    EXPECT_TRUE(cpu.pc == WORK_IRQ_HANDLER && !cpu.stopped && (cpu.sr & 0x2700) == 0x2200)
        << "IRQ releases STOP and raises mask";
    EXPECT_EQ(cpu.dispatch_deadline, 0) << "IRQ redirect requires lookup through a fresh boundary";
    EXPECT_TRUE(cpu.a[7] == WORK_EXCEPTION_SP && m->read32(cpu.a[7] + 2) == TEST_RESET_PC && m->read16(cpu.a[7] + 6) == 104)
        << "68020 interrupt frame";
}

TEST_F(CpuTest, ExceptionFramesAndRte) {
    auto &cpu = m->cpu;
    cpu.vbr = f3rt::RAM_BASE;
    cpu.a[7] = WORK_EXCEPTION_SP;
    m->write32(cpu.vbr + 5 * 4, WORK_RTE_HANDLER);
    m->write16(WORK_RTE_HANDLER, 0x4e73); // RTE handler
    cpu.pc = TEST_RESET_PC;
    cpu.sr = 0x2700;
    const auto exception_cycles = cpu.cycles;
    const auto exception_sp = cpu.a[7];
    f3_exception(&cpu, 5, TEST_RESET_PC + 2);
    EXPECT_TRUE(cpu.cycles - exception_cycles == 38 && m->read16(cpu.a[7] + 6) == 0x2014 &&
                m->read32(cpu.a[7] + 8) == TEST_RESET_PC)
        << "Divide-by-zero full charge and format-2 instruction PC";
    EXPECT_TRUE(f3_fallback(&cpu) && cpu.pc == TEST_RESET_PC + 2 && cpu.a[7] == exception_sp && cpu.sr == 0x2700)
        << "Real RTE restores the format-2 resume PC and stack";
    EXPECT_TRUE(m->boundary() == 0 && cpu.dispatch_deadline > cpu.cycles)
        << "Restored masked CPU refreshes its event deadline";
    m->write16(WORK_LOW_SR_CODE, 0x46fc);
    m->write16(WORK_LOW_SR_CODE + 2, 0x2000);
    cpu.pc = WORK_LOW_SR_CODE;
    m->interpreter->run_main(1);
    EXPECT_TRUE(cpu.sr == 0x2000 && cpu.dispatch_deadline == 0)
        << "Interpreted SR lowering also invalidates the native deadline";
}

TEST_F(CpuTest, NativeDispatchFallbackAndExclusions) {
    auto &cpu = m->cpu;
    const f3_block blocks[] = {{TEST_RESET_PC, native}};
    EXPECT_EQ(f3_register_blocks(&cpu, blocks, 1), 1) << "Valid block table";
    cpu.pc = TEST_RESET_PC;
    cpu.sr = 0x2700;
    EXPECT_TRUE(f3_dispatch(&cpu) && cpu.d[0] == 99) << "Native dispatch executes matching block";
    cpu.pc = TEST_RESET_PC;
    cpu.sr = 0x2700;
    EXPECT_TRUE(f3_fallback(&cpu) && cpu.d[0] == 42 && cpu.pc == TEST_RESET_PC + 2)
        << "Fallback executes exactly one real instruction";
    cpu.d[0] = 0xdeadbeef;
    cpu.sr = 0x2015;
    const auto reset_cycles = cpu.cycles;
    m->reset_main_cpu();
    EXPECT_TRUE(cpu.d[0] == 0xdeadbeef && cpu.sr == 0x2715 && cpu.pc == TEST_RESET_PC)
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
    EXPECT_TRUE(rejected && cpu.pc == TEST_RESET_PC && cpu.d[0] == 0xdeadbeef && m->fallback_instructions == fallback_count)
        << "Native-only fallback rejection reports PC without executing or counting an instruction";
    const f3_block bad[] = {{TEST_RESET_PC, native}, {TEST_RESET_PC, native}};
    EXPECT_FALSE(f3_register_blocks(&cpu, bad, 2)) << "Duplicate PCs rejected";
    const f3_excluded_range excluded[] = {{EXCLUDED_START, EXCLUDED_END, "synthetic data", "fixture-owned words"}};
    EXPECT_TRUE(f3_register_exclusions(&cpu, excluded, 1)) << "Exclusion complement registers";
    const f3_block overlapping[] = {{EXCLUDED_START, native}};
    EXPECT_FALSE(f3_register_blocks(&cpu, overlapping, 1)) << "Native entries cannot bypass exclusions";
    m->allow_main_fallback = true;
    for (uint32_t pc : {EXCLUDED_START, EXCLUDED_START + 1, EXCLUDED_START + 2, EXCLUDED_START + 3, EXCLUDED_ALIAS_BASE, EXCLUDED_ALIAS_BASE + 3}) {
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
    cpu.pc = EXCLUDED_END;
    cpu.halted = 0;
    const auto before_end_fallback = m->fallback_instructions;
    EXPECT_TRUE(f3_fallback(&cpu) && m->fallback_instructions == before_end_fallback + 1)
        << "Exclusive end remains executable";
    EXPECT_EQ(f3_read16(&cpu, EXCLUDED_START), 0) << "Excluded instruction bytes remain readable as data";
    EXPECT_TRUE(f3_register_exclusions(&cpu, nullptr, 0)) << "Clearing immutable exclusion metadata";
}

TEST_F(CpuTest, WatchdogWholeBoardReset) {
    auto &cpu = m->cpu;
    cpu.usp = WORK_USP;
    cpu.a[7] = WORK_STACK_TOP;
    cpu.sr = 0x2600;
    m->boundary();
    f3_set_sr(&cpu, 0);
    m->write8(f3rt::WATCHDOG, 0);
    const auto watchdog_deadline = cpu.cycles + 3ull * f3rt::Machine::main_clock;

    m->audio->write16(SND_WORK_RAM, 0xa55a);
    m->audio->set_reset(true);
    m->audio->write8(DUART_IVR, 0x66);
    m->audio->write8(f3rt::SOUND_ES5510_BASE + 1, 0x12);
    m->audio->write8(f3rt::SOUND_ES5510_BASE + 3, 0x34);
    m->audio->write8(f3rt::SOUND_ES5510_BASE + 5, 0x56);
    m->audio->write8(f3rt::SOUND_ES5510_BASE + 0x141, 0);
    m->audio->set_reset(false);
    m->audio->set_reset(true);
    m->audio->write8(f3rt::SOUND_ES5510_BASE + 0x101, 0);
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
    EXPECT_EQ(m->audio->read8(DUART_IVR), 0x0f) << "Watchdog restores the DUART interrupt vector";
    EXPECT_EQ(m->audio->read16(SND_WORK_RAM), 0xa55a) << "Whole-board reset preserves sound work RAM";
    EXPECT_EQ(m->audio->read32(0), 0) << "Whole-board reset reloads boot vectors from sound ROM";
    m->audio->write8(f3rt::SOUND_ES5510_BASE + 0x101, 0);
    EXPECT_TRUE(m->audio->read8(f3rt::SOUND_ES5510_BASE + 1) == 0 && m->audio->read8(f3rt::SOUND_ES5510_BASE + 3) == 0 &&
                m->audio->read8(f3rt::SOUND_ES5510_BASE + 5) == 0)
        << "Watchdog clears DSP general-purpose registers";
}

// RapidCheck property: Work RAM 32-bit writes are identically readable from the mirror offset.
RC_GTEST_PROP(Cpu, WorkRamMirrorRoundTrip, ()) {
    const auto offset = (*rc::gen::inRange(0u, 0x20000u / 4)) * 4;
    const auto val = *rc::gen::arbitrary<uint32_t>();
    static auto m = std::make_unique<f3rt::Machine>(fixture());
    m->write32(f3rt::RAM_BASE + offset, val);
    const uint32_t mirror_read = m->read32(f3rt::RAM_PHYSICAL_END + offset);
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
    const uint32_t a = f3rt::RAM_BASE + offset;
    const uint16_t expected16 = uint16_t((m->read8(a) << 8) | m->read8(a + 1));
    const uint32_t expected32 = (uint32_t(m->read8(a)) << 24) |
                                (uint32_t(m->read8(a + 1)) << 16) |
                                (uint32_t(m->read8(a + 2)) << 8) |
                                uint32_t(m->read8(a + 3));
    RC_ASSERT(m->read16(a) == expected16);
    RC_ASSERT(m->read32(a) == expected32);
}
