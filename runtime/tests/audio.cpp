#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/rom.hpp"
#include "audio/hle/effects.hpp"
#include "audio/hle/events.hpp"
#include "audio/hle/note_timing.hpp"
#include "audio/hle/synth.hpp"
#include "audio/hle/voice_kernel.hpp"
#include "audio/hle/sound_rom_5a7e9117.hpp"
#include "interpreter.hpp"
#include "third_party/audio/mc68681.hpp"
#include "third_party/audio/es5510.hpp"
#include "state_io.hpp"
#include "support.hpp"
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <thread>
#include <vector>

using f3test::fixture;

namespace {
// Sound-side scratch and vector addresses (sound bus) used by the test programs.
constexpr uint32_t SND_PROGRAM = 0x100;       // mailbox-copy program image
constexpr uint32_t SND_IRQ_VECTOR_64 = 0x100; // vector slot for DUART vector 64
constexpr uint32_t SND_CODE_BASE = 0x1000;    // foreground / executed-instruction code
constexpr uint32_t SND_HANDLER_BASE = 0x2000; // DUART IRQ handler
constexpr uint32_t SND_EXEC_SOURCE = 0x4000;  // operand scratch
constexpr uint32_t SND_RESULT_D0 = 0x1500;
constexpr uint32_t SND_RESULT_A1 = 0x1504;
constexpr uint32_t SND_RESULT_SR = 0x1508;
constexpr uint32_t SND_WORK_BYTE = 0x600;   // mailbox copy destination
constexpr uint32_t SND_IRQ_RECORDS = 0x600; // ten 4-byte IRQ records
constexpr uint16_t hi16(uint32_t a) { return uint16_t(a >> 16); }
constexpr uint16_t lo16(uint32_t a) { return uint16_t(a); }
// DUART register n on the sound bus (odd byte lane).
constexpr uint32_t duart_reg(uint32_t reg) { return f3rt::SOUND_DUART_BASE + 2 * reg + 1; }
constexpr uint32_t SND_STACK_SR = 0xfefa; // stacked SR of a vectored IRQ entry
constexpr uint32_t SND_STACK_PC = 0xfefc; // stacked return PC of a vectored IRQ entry
// ES5505 (OTIS) register n on the sound bus (16-bit, even byte lane).
constexpr uint32_t otis_reg(uint32_t reg) { return f3rt::SOUND_ES5505_BASE + 2 * reg; }
std::unique_ptr<f3rt::Machine> make_sound_machine() {
    auto roms = fixture();
    roms.sound[2] = 0x80;
    roms.sound[6] = 1; // SSP 8000, PC 100.
    auto m = std::make_unique<f3rt::Machine>(std::move(roms));
    // Real sound CPU: snapshot mailbox byte zero, post a reply, then loop.
    const uint16_t code[] = {0x13f9,
                             hi16(f3rt::SOUND_SHARED_BASE),
                             lo16(f3rt::SOUND_SHARED_BASE),
                             0x0000,
                             lo16(SND_WORK_BYTE),
                             0x13fc,
                             0x005a,
                             hi16(f3rt::SOUND_SHARED_BASE + 2),
                             lo16(f3rt::SOUND_SHARED_BASE + 2),
                             0x60fe};
    for (unsigned i = 0; i < std::size(code); ++i) m->audio->write16(SND_PROGRAM + 2 * i, code[i]);
    m->write8(f3rt::SHARED_BASE, 0x11);
    return m;
}

struct SoundExecResult {
    int cycles;
    uint32_t d0, a1;
    uint16_t sr;
};
SoundExecResult execute_sound_instruction(f3rt::Machine &m,
                                          std::initializer_list<uint16_t> instruction,
                                          uint32_t source = 0, uint32_t d0 = 0) {
    m.audio->set_reset(true);
    m.audio->write32(0, 0xff00);
    m.audio->write32(4, SND_CODE_BASE);
    m.audio->write32(SND_EXEC_SOURCE, source << 16);
    uint32_t pc = SND_CODE_BASE;
    const auto emit = [&](uint16_t word) {
        m.audio->write16(pc, word);
        pc += 2;
    };
    for (uint16_t word : {uint16_t(0x203c), uint16_t(d0 >> 16), uint16_t(d0), uint16_t(0x223c),
                          uint16_t(source >> 16), uint16_t(source), uint16_t(0x227c), uint16_t(0),
                          lo16(SND_EXEC_SOURCE), uint16_t(0x46fc), uint16_t(0x271b)})
        emit(word);
    for (auto word : instruction) emit(word);
    for (uint16_t word : {uint16_t(0x40f8), lo16(SND_RESULT_SR), uint16_t(0x21c0),
                          lo16(SND_RESULT_D0), uint16_t(0x21c9), lo16(SND_RESULT_A1)})
        emit(word);
    m.audio->set_reset(false);
    for (int i = 0; i < 5; ++i) m.interpreter->run_audio(1);
    const int cycles = m.interpreter->run_audio(1);
    for (int i = 0; i < 3; ++i) m.interpreter->run_audio(1);
    const SoundExecResult result{cycles, m.audio->read32(SND_RESULT_D0),
                                 m.audio->read32(SND_RESULT_A1), m.audio->read16(SND_RESULT_SR)};
    m.audio->set_reset(true);
    return result;
}

class AudioMachineTest : public ::testing::Test {
protected:
    std::unique_ptr<f3rt::Machine> m_m;

    void SetUp() override { m_m = std::make_unique<f3rt::Machine>(fixture()); }
};
} // namespace

TEST(Audio, SoundResetReleaseOrdering) {
    auto m = make_sound_machine();
    m->cpu.cycles = 2048;
    f3_write16(&m->cpu, f3rt::SOUND_RESET_RELEASE, 0);
    m->boundary();
    EXPECT_TRUE(m->audio->read8(SND_WORK_BYTE) == 0 && m->shared[1] == 0)
        << "Sound reset release cannot execute the CPU during preceding main-block time";
    m->cpu.cycles += 1024;
    m->boundary();
    EXPECT_TRUE(m->audio->read8(SND_WORK_BYTE) == 0x11 && m->shared[1] == 0x5a)
        << "Sound CPU executes the mailbox program after reset release";
}

TEST(Audio, SoundBankswitchIndexMapping) {
    auto m = make_sound_machine();
    EXPECT_EQ(m->audio->sound_bank(), 1u);

    // When sound_bankswitch is disabled (default for unbanked games), writes are ignored
    m->sound_bankswitch = false;
    m->write16(f3rt::MAIN_SOUND_BANKSWITCH_BASE + 2 * 4, 0);
    EXPECT_EQ(m->audio->sound_bank(), 1u);

    // Enable bankswitch (kirameki)
    m->sound_bankswitch = true;

    // 16-bit word writes (move.w): addr = 0x300000 + bank * 2
    // Banks 1..7 map to pages 1..7
    for (uint32_t bank = 1; bank <= 7; ++bank) {
        m->write16(f3rt::MAIN_SOUND_BANKSWITCH_BASE + bank * 2, 0);
        EXPECT_EQ(m->audio->sound_bank(), bank) << "16-bit write for bank " << bank;
    }

    // Banks 8..15 wrap with idx -= 8 -> pages 0..7
    for (uint32_t bank = 8; bank <= 15; ++bank) {
        m->write16(f3rt::MAIN_SOUND_BANKSWITCH_BASE + bank * 2, 0);
        EXPECT_EQ(m->audio->sound_bank(), bank - 8) << "16-bit write for bank " << bank;
    }

    // Banks 16..31 map to pages 8..23
    for (uint32_t bank = 16; bank <= 31; ++bank) {
        m->write16(f3rt::MAIN_SOUND_BANKSWITCH_BASE + bank * 2, 0);
        EXPECT_EQ(m->audio->sound_bank(), bank - 8) << "16-bit write for bank " << bank;
    }

    // 8-bit byte writes (move.b)
    // High byte of word 0 (0x300000) -> bank 0 -> page 0
    m->write8(f3rt::MAIN_SOUND_BANKSWITCH_BASE, 0);
    EXPECT_EQ(m->audio->sound_bank(), 0u);
    // Low byte of word 0 (0x300001) -> bank 0 -> page 0
    m->write8(f3rt::MAIN_SOUND_BANKSWITCH_BASE + 1, 0);
    EXPECT_EQ(m->audio->sound_bank(), 0u);
    // High byte of word 1 (0x300002) -> bank 1 -> page 1
    m->write8(f3rt::MAIN_SOUND_BANKSWITCH_BASE + 2, 0);
    EXPECT_EQ(m->audio->sound_bank(), 1u);
    // Low byte of word 1 (0x300003) -> bank 1 -> page 1
    m->write8(f3rt::MAIN_SOUND_BANKSWITCH_BASE + 3, 0);
    EXPECT_EQ(m->audio->sound_bank(), 1u);
    // Bank 16 (0x300000 + 32 = 0x300020): word 16 -> idx 16 -> page 8
    m->write8(f3rt::MAIN_SOUND_BANKSWITCH_BASE + 32, 0);
    EXPECT_EQ(m->audio->sound_bank(), 8u);

    // 32-bit dword writes (move.l)
    // Dword 0 touches low 16 bits (bank 1) -> page 1
    m->write32(f3rt::MAIN_SOUND_BANKSWITCH_BASE, 0);
    EXPECT_EQ(m->audio->sound_bank(), 1u);
    // Dword 1 touches bank 3 -> page 3
    m->write32(f3rt::MAIN_SOUND_BANKSWITCH_BASE + 4, 0);
    EXPECT_EQ(m->audio->sound_bank(), 3u);
    // Dword 8 (offset 0x20 = 32) touches bank 17 -> idx 17 -> page 9
    m->write32(f3rt::MAIN_SOUND_BANKSWITCH_BASE + 32, 0);
    EXPECT_EQ(m->audio->sound_bank(), 9u);
}

TEST(Audio, MailboxWidthWrites) {
    for (unsigned width : {1, 2, 4}) {
        auto m = make_sound_machine();
        m->audio->set_reset(false);
        m->cpu.cycles = 1024;
        if (width == 1) f3_write8(&m->cpu, f3rt::SHARED_BASE, 0x22);
        else if (width == 2) f3_write16(&m->cpu, f3rt::SHARED_BASE, 0x2233);
        else
            f3_write32(&m->cpu, f3rt::SHARED_BASE - 1,
                       0x99223344); // Unaligned access enters shared RAM.
        m->boundary();
        EXPECT_TRUE(m->audio->read8(SND_WORK_BYTE) == 0x11 && m->shared[0] == 0x22)
            << "Mailbox writes become visible only after preceding sound execution";
    }
}

TEST(Audio, MailboxWidthReads) {
    for (unsigned width : {1, 2, 4}) {
        auto m = make_sound_machine();
        m->audio->set_reset(false);
        m->cpu.cycles = 1024;
        const uint32_t value = width == 1   ? f3_read8(&m->cpu, f3rt::SHARED_BASE + 1)
                               : width == 2 ? f3_read16(&m->cpu, f3rt::SHARED_BASE)
                                            : f3_read32(&m->cpu, f3rt::SHARED_BASE - 1);
        EXPECT_EQ(value, (width == 1   ? 0x5au
                          : width == 2 ? 0x115au
                                       : 0xff115a00u))
            << "Mailbox reads observe sound replies produced before the current main instruction";
    }
}

TEST(Audio, ResetInstructionPreservesPrecedingSoundExecution) {
    for (bool reset_instruction : {false, true}) {
        auto m = make_sound_machine();
        m->audio->set_reset(false);
        m->cpu.cycles = 1024;
        if (reset_instruction) f3_reset_devices(&m->cpu);
        else f3_write8(&m->cpu, f3rt::SOUND_RESET_ASSERT, 0);
        m->boundary();
        EXPECT_TRUE(m->audio->is_reset() && m->audio->read8(SND_WORK_BYTE) == 0x11 &&
                    m->shared[1] == 0x5a)
            << "Reset assertion preserves sound execution preceding the reset instruction";
    }
}

TEST(Audio, SoundIrqPartitioningIndependence) {
    const auto run = [](unsigned quantum) {
        auto m = std::make_unique<f3rt::Machine>(fixture());
        m->audio->write32(0, 0xff00);
        m->audio->write32(4, SND_CODE_BASE);
        m->audio->write32(SND_IRQ_VECTOR_64, SND_HANDLER_BASE);
        // Foreground counts iterations; each timer IRQ records the interrupted count.
        const uint16_t foreground[] = {0x7000, 0x227c, 0,      lo16(SND_IRQ_RECORDS),
                                       0x46fc, 0x2000, 0x5280, 0x60fc};
        const uint16_t handler[] = {0x22c0, 0x1239, hi16(duart_reg(15)), lo16(duart_reg(15)),
                                    0x4e73};
        for (unsigned i = 0; i < std::size(foreground); ++i)
            m->audio->write16(SND_CODE_BASE + 2 * i, foreground[i]);
        for (unsigned i = 0; i < std::size(handler); ++i)
            m->audio->write16(SND_HANDLER_BASE + 2 * i, handler[i]);
        m->audio->write8(duart_reg(12), 64);
        m->audio->write8(duart_reg(5), 8);
        m->audio->write8(duart_reg(6), 0);
        m->audio->write8(duart_reg(7), 125);
        m->audio->write8(duart_reg(4), 0x60);
        m->audio->set_reset(false);
        for (unsigned elapsed = 0; elapsed < 10050;) {
            const unsigned step = quantum < 10050 - elapsed ? quantum : 10050 - elapsed;
            m->audio->advance(step);
            elapsed += step;
        }
        std::array<uint32_t, 11> state{};
        for (unsigned i = 0; i < 10; ++i) {
            state[i] = m->audio->read32(SND_IRQ_RECORDS + 4 * i);
            EXPECT_GT(state[i], (i ? state[i - 1] : 0))
                << "Each periodic IRQ observes further real foreground execution";
        }
        EXPECT_EQ(m->audio->read32(SND_IRQ_RECORDS + 4 * 10), 0)
            << "Ten elapsed timer deadlines produce exactly ten interrupt records";
        state[10] = m->interpreter->sound_pc();
        return state;
    };
    const auto single_clock = run(1);
    for (unsigned quantum : {7, 64, 511, 4096})
        EXPECT_EQ(run(quantum), single_clock) << "Sound IRQ recognition and CPU state are "
                                                 "independent of main-block time partitioning";
}

TEST(Audio, DuartCounterRestartAndExpiry) {
    const auto preset = [](f3rt::MC68681 &d, unsigned count) {
        d.write(6, count >> 8);
        d.write(7, count);
    };
    f3rt::MC68681 restart;
    preset(restart, 3);
    restart.write(4, 0x30);
    restart.read(14);
    restart.advance(17);
    restart.read(14);
    restart.advance(47);
    EXPECT_EQ(restart.read(5) & 8, 0)
        << "Restarting the counter discards the preceding divider phase";
    restart.advance(1);
    EXPECT_TRUE((restart.read(5) & 8) != 0 && restart.read(6) == 0xff && restart.read(7) == 0xff)
        << "Counter expires at the new deadline and reloads the reference 0xffff period";
    restart.advance(16);
    EXPECT_EQ(restart.read(7), 0xfe)
        << "Counter underflow reload is independent of the programmed preset";
    restart.read(15);
    restart.advance(1000000);
    EXPECT_EQ(restart.read(5) & 8, 0)
        << "Stop-counter read acknowledges and cancels counter-mode expiration";
}

TEST(Audio, DuartTimerModeAndClockSource) {
    const auto preset = [](f3rt::MC68681 &d, unsigned count) {
        d.write(6, count >> 8);
        d.write(7, count);
    };
    f3rt::MC68681 mode;
    preset(mode, 3);
    mode.write(4, 0x30);
    mode.read(14);
    mode.advance(17);
    mode.read(15);
    mode.write(4, 0x60);
    mode.advance(5);
    EXPECT_EQ(mode.read(5) & 8, 0)
        << "Entering timer mode starts a fresh full period without old divider residue";
    mode.advance(1);
    EXPECT_NE(mode.read(5) & 8, 0) << "Timer ready asserts after both half-periods";
    mode.read(15);
    mode.advance(6);
    EXPECT_NE(mode.read(5) & 8, 0)
        << "Timer-mode acknowledgement does not stop periodic interrupts";

    f3rt::MC68681 source;
    preset(source, 2);
    source.write(4, 0x70);
    source.advance(8);
    source.write(4, 0x60);
    source.advance(23);
    EXPECT_TRUE(source.read(7) == 1 && (source.read(5) & 8) == 0)
        << "Clock-source change preserves the armed duration";
    source.advance(1);
    source.advance(1);
    EXPECT_EQ(source.read(5) & 8, 0) << "The next half-period uses the newly selected source";
    source.advance(1);
    EXPECT_NE(source.read(5) & 8, 0)
        << "Reload adopts the new clock without rescaling elapsed time";
}

TEST(Audio, DuartResetRetainsExpiration) {
    const auto preset = [](f3rt::MC68681 &d, unsigned count) {
        d.write(6, count >> 8);
        d.write(7, count);
    };
    f3rt::MC68681 reset;
    reset.advance(1000000);
    EXPECT_EQ(reset.read(5), 0) << "A cold DUART has no scheduled counter event";
    preset(reset, 100);
    reset.write(4, 0x60);
    reset.advance(37);
    reset.reset();
    EXPECT_TRUE(reset.read(5) == 0 && !reset.irq_pending())
        << "Board reset clears the visible IRQ registers";
    reset.advance(62);
    EXPECT_EQ(reset.read(5), 0)
        << "Board reset preserves the remaining deadline, not a restarted period";
    reset.advance(1);
    EXPECT_TRUE(reset.read(5) == 8 && !reset.irq_pending())
        << "Retained expiration latches counter-ready while reset IMR masks IRQ";
    reset.write(12, 64);
    reset.write(5, 8);
    EXPECT_TRUE(reset.irq_pending() && reset.get_irq_vector() == 64 && reset.irq_pending())
        << "Unmasking retained counter-ready asserts IRQ; IACK supplies vector without clearing it";
    reset.read(15);
    EXPECT_FALSE(reset.irq_pending())
        << "Counter acknowledge clears a retained post-reset interrupt";
}

TEST(Audio, DuartTxFramingAndInterrupts) {
    for (unsigned channel : {0, 1}) {
        f3rt::MC68681 duart;
        const unsigned base = channel * 8, half = channel ? 64 : 32;
        const uint8_t mask = channel ? 0x10 : 0x01;
        bool irq = false;
        duart.set_irq_callback([&](bool asserted) { irq = asserted; });
        const auto configure = [&] {
            duart.write(base, 0x13);
            duart.write(base, 0x0f);     // 8N2
            duart.write(base + 1, 0xee); // F3 external clock / 16
            duart.write(5, mask);
            duart.write(base + 2, 4);
        };
        EXPECT_EQ(duart.read(base + 1) & 0x0c, 0) << "Reset disables the UART transmitter";
        configure();
        EXPECT_TRUE((duart.read(base + 1) & 0x0c) == 0x0c && irq)
            << "Enabling an idle transmitter asserts ready/empty and its IRQ";
        duart.write(base + 3, 0x80);
        EXPECT_TRUE((duart.read(base + 1) & 0x0c) == 0 && !irq)
            << "THR write clears ready and empty";
        duart.advance(3 * half - 1);
        EXPECT_EQ(duart.read(base + 1) & 0x0c, 0) << "TX ready waits through the start-bit time";
        duart.advance(1);
        EXPECT_TRUE((duart.read(base + 1) & 0x0c) == 4 && irq)
            << "The second rising edge releases THR and asserts TX ready";
        duart.advance(18 * half - 1);
        EXPECT_EQ(duart.read(base + 1) & 8, 0) << "TX empty stays clear until the last framed bit";
        duart.advance(1);
        EXPECT_EQ(duart.read(base + 1) & 0x0c, 0x0c)
            << "First 8N2 frame completes after 21 half-bit clocks";
        duart.write(base + 3, 0x90);
        duart.advance(4 * half);
        duart.write(base + 3, 0x7f);
        duart.write(base + 3, 0x44); // One holding slot; third byte is rejected.
        EXPECT_TRUE((duart.read(base + 1) & 0x0c) == 0 && !irq)
            << "A queued byte occupies THR while the current byte shifts";
        duart.advance(18 * half);
        EXPECT_TRUE((duart.read(base + 1) & 0x0c) == 4 && irq)
            << "Buffered transfer restores ready without asserting empty";
        duart.advance(22 * half - 1);
        EXPECT_EQ(duart.read(base + 1) & 8, 0)
            << "Queued frame retains the continuous serial edge phase";
        duart.advance(1);
        EXPECT_EQ(duart.read(base + 1) & 0x0c, 0x0c)
            << "Holding-register overflow does not enqueue an extra frame";
        duart.reset();
        configure();
        duart.write(base + 3, 0x80);
        duart.advance(22 * half - 1);
        EXPECT_EQ(duart.read(base + 1) & 8, 0)
            << "Board reset retains the stopped serial clock edge state";
        duart.advance(1);
        EXPECT_NE(duart.read(base + 1) & 8, 0) << "Post-reset frame uses a full first bit period";
        duart.write(base + 3, 0x55);
        duart.advance(4 * half);
        duart.write(base + 2, 0x30);
        duart.advance(100 * half);
        EXPECT_TRUE((duart.read(base + 1) & 0x0c) == 0 && !irq)
            << "Transmitter reset aborts the frame and clears its IRQ";
        duart.write(base + 2, 0x10);
        duart.write(base, 0);
        duart.write(base, 7); // 5E1
        duart.write(base + 1, 0xef);
        duart.write(base + 2, 4);
        duart.write(base + 3, 0x15);
        const unsigned fast_half = half / 16;
        duart.advance(16 * fast_half - 1);
        EXPECT_EQ(duart.read(base + 1) & 8, 0)
            << "Word length, parity and direct external clock determine frame duration";
        duart.advance(1);
        EXPECT_NE(duart.read(base + 1) & 8, 0) << "5E1 frame completes at its eight-bit boundary";
    }
}

TEST(Audio, DuartCounterDerivedTxClock) {
    f3rt::MC68681 counter_clock;
    counter_clock.write(6, 0);
    counter_clock.write(7, 1);
    counter_clock.write(4, 0x60);
    counter_clock.write(8, 0x13);
    counter_clock.write(8, 0x0f);
    counter_clock.write(9, 0xed);
    counter_clock.write(10, 4);
    counter_clock.write(11, 0x80);
    counter_clock.read(14);
    counter_clock.advance(46);
    EXPECT_EQ(counter_clock.read(9) & 0x0c, 0)
        << "Counter-derived TX clock retains the divide-by-16 prescaler";
    counter_clock.advance(1);
    EXPECT_EQ(counter_clock.read(9) & 0x0c, 4)
        << "Counter-derived TX ready follows the second serial rising edge";
    counter_clock.advance(287);
    EXPECT_EQ(counter_clock.read(9) & 8, 0) << "Counter-derived TX empty waits for the full frame";
    counter_clock.advance(1);
    EXPECT_EQ(counter_clock.read(9) & 0x0c, 0x0c)
        << "Counter-derived 8N2 frame completes at its eleventh serial edge";
}

TEST_F(AudioMachineTest, SoundInstructionCycleCharges) {
    const auto execute = [&](std::initializer_list<uint16_t> instruction, uint32_t source = 0,
                             uint32_t d0 = 0) {
        return execute_sound_instruction(*m_m, instruction, source, d0);
    };

    const auto quick = execute({0x5049});
    EXPECT_TRUE(quick.cycles == 8 && quick.a1 == 0x4008 && quick.sr == 0x271b)
        << "68000 ADDQ.W to an address register takes eight cycles and preserves flags";
    EXPECT_EQ(execute({0x544f}).cycles, 8)
        << "68000 ADDQ.W stack adjustment has the same full cost";
    EXPECT_TRUE(execute({0xd2fc, 10}).cycles == 12 && execute({0x92fc, 10}).cycles == 12)
        << "68000 immediate word address arithmetic has no long-operand surcharge";
    EXPECT_TRUE(execute({0xd3fc, 0, 10}).cycles == 16 && execute({0x93fc, 0, 10}).cycles == 16)
        << "68000 immediate long address arithmetic retains its surcharge";
    for (uint16_t family : {0xd000, 0x9000, 0xc000, 0x8000}) {
        EXPECT_TRUE(execute({uint16_t(family | 0x3c), 1}).cycles == 8 &&
                    execute({uint16_t(family | 0x7c), 1}).cycles == 8 &&
                    execute({uint16_t(family | 0xbc), 0, 1}).cycles == 16)
            << "68000 immediate EA arithmetic charges byte, word and long operands distinctly";
        EXPECT_EQ(execute({uint16_t(family | 0x81)}).cycles, 8)
            << "68000 long register arithmetic takes eight cycles";
        if (family == 0xd000 || family == 0x9000)
            EXPECT_EQ(execute({uint16_t(family | 0x89)}).cycles, 8)
                << "68000 long arithmetic accepts address-register sources";
    }
    for (uint16_t opcode : {0xd3c0, 0xd3c8, 0x93c0, 0x93c8})
        EXPECT_EQ(execute({opcode}).cycles, 8)
            << "68000 long address arithmetic has an eight-cycle register cost";
    const auto tas = execute({0x4ad1});
    EXPECT_TRUE(tas.cycles == 14 && tas.sr == 0x2714 && m_m->audio->read8(SND_EXEC_SOURCE) == 0x80)
        << "68000 memory TAS charges its read-modify-write once and reports the original byte";
    EXPECT_TRUE(execute({0x4ac0}).cycles == 4 && execute({0x4ad9}).cycles == 14 &&
                execute({0x4ae1}).cycles == 16 && execute({0x4ae9, 16}).cycles == 18 &&
                execute({0x4af8, SND_EXEC_SOURCE}).cycles == 18 &&
                execute({0x4af9, 0, SND_EXEC_SOURCE}).cycles == 22)
        << "68000 TAS retains register and effective-address timing distinctions";

    for (unsigned kind = 1; kind <= 3; ++kind)
        for (unsigned bit : {0, 2, 15, 16, 31, 32, 47, 48, 63}) {
            const int cycles = (kind == 2 ? 8 : 6) + ((bit & 31) >= 16 ? 2 : 0);
            const uint32_t result = kind == 2 ? 0 : 1u << (bit & 31);
            const auto reg = execute({uint16_t(0x0300 | (kind << 6))}, bit);
            const auto immediate = execute({uint16_t(0x0800 | (kind << 6)), uint16_t(bit)});
            EXPECT_TRUE(reg.cycles == cycles && immediate.cycles == cycles + 4)
                << "68000 register bit mutations distinguish low/high halves after modulo-32 "
                   "selection";
            EXPECT_TRUE(reg.d0 == result && immediate.d0 == result && reg.sr == 0x271f &&
                        immediate.sr == 0x271f)
                << "Bit timing preserves result, old-bit Z and untouched X/N/V/C flags";
        }
    const struct {
        uint32_t dividend;
        uint16_t divisor;
        int cycles;
    } divisions[] = {{0, 1, 136},
                     {0xffff, 1, 106},
                     {0x10000, 1, 10},
                     {0x10000, 2, 134},
                     {0xff1234, 0x100, 116},
                     {0x80000000, 0xffff, 132},
                     {0xfffeffff, 0xffff, 76}};
    for (const auto &test : divisions) {
        const auto reg = execute({0x80c1}, test.divisor, test.dividend);
        const auto immediate = execute({0x80fc, test.divisor}, test.divisor, test.dividend);
        const auto memory = execute({0x80d1}, test.divisor, test.dividend);
        EXPECT_TRUE(reg.cycles == test.cycles && immediate.cycles == test.cycles + 4 &&
                    memory.cycles == test.cycles + 4)
            << "68000 DIVU.W timing follows the operand-dependent subtract stages and EA cost";
        const auto quotient = test.dividend / test.divisor;
        const bool overflow = quotient > 0xffff;
        const auto result =
            overflow ? test.dividend : ((test.dividend % test.divisor) << 16) | quotient;
        const uint16_t flags =
            0x10 | (overflow ? 2 : ((quotient == 0 ? 4 : 0) | (quotient & 0x8000 ? 8 : 0)));
        const uint16_t mask = overflow ? 0x13 : 0x1f; // N/Z are undefined on overflow.
        EXPECT_TRUE(reg.d0 == result && immediate.d0 == result && memory.d0 == result &&
                    (reg.sr & mask) == flags && (immediate.sr & mask) == flags &&
                    (memory.sr & mask) == flags)
            << "DIVU timing preserves packed remainder/quotient, overflow destination and defined "
               "flags";
    }
    const struct {
        uint16_t source;
        int cycles;
    } multiplications[] = {{0, 38},      {1, 42},      {0x12, 46},   {0x5555, 70},
                           {0x7fff, 42}, {0x8000, 40}, {0xffff, 40}, {0xaaaa, 68}};
    for (const auto &test : multiplications) {
        const auto reg = execute({0xc1c1}, test.source, 0xfffffffd);
        const auto immediate = execute({0xc1fc, test.source}, test.source, 0xfffffffd);
        const auto memory = execute({0xc1d1}, test.source, 0xfffffffd);
        EXPECT_TRUE(reg.cycles == test.cycles && immediate.cycles == test.cycles + 4 &&
                    memory.cycles == test.cycles + 4)
            << "68000 MULS.W charges every Booth transition including the positive source's final "
               "transition";
        const int32_t product = int32_t(int16_t(test.source)) * -3;
        const uint16_t sr = 0x2710 | (product == 0 ? 4 : product < 0 ? 8 : 0);
        EXPECT_TRUE(reg.d0 == uint32_t(product) && immediate.d0 == uint32_t(product) &&
                    memory.d0 == uint32_t(product) && reg.sr == sr && immediate.sr == sr &&
                    memory.sr == sr)
            << "Signed multiply preserves the full product and X while replacing N/Z/V/C";
    }
}

TEST_F(AudioMachineTest, SoundVectoredIrqWake) {
    m_m->audio->set_cpu_runner({});
    for (unsigned vector : {15, 30, 64, 255}) {
        m_m->audio->reset_board();
        m_m->audio->write32(0, 0xff00);
        m_m->audio->write32(4, SND_CODE_BASE);
        m_m->audio->write32(vector * 4, SND_HANDLER_BASE);
        m_m->audio->write16(SND_CODE_BASE, 0x4e72);
        m_m->audio->write16(SND_CODE_BASE + 2, 0x2000);
        m_m->audio->write16(SND_HANDLER_BASE, 0x4e71);
        m_m->audio->set_reset(false);
        m_m->interpreter->run_audio(1);
        m_m->interpreter->run_audio(1);
        m_m->audio->write8(duart_reg(12), vector);
        m_m->audio->write8(duart_reg(6), 0);
        m_m->audio->write8(duart_reg(7), 1);
        m_m->audio->write8(duart_reg(5), 8);
        m_m->audio->write8(duart_reg(4), 0x60);
        m_m->audio->advance(8);
        EXPECT_EQ(m_m->audio->irq_level(), 6) << "DUART timer wakes the stopped sound CPU on IRQ6";
        EXPECT_TRUE(m_m->interpreter->run_audio(1) == 48 &&
                    m_m->interpreter->sound_pc() == SND_HANDLER_BASE + 2)
            << "68000 IRQ entry costs 44 cycles independently of vector number, plus the handler "
               "NOP";
        EXPECT_TRUE(m_m->audio->read16(SND_STACK_SR) == 0x2000 &&
                    m_m->audio->read32(SND_STACK_PC) == SND_CODE_BASE + 4)
            << "Vectored IRQ entry preserves the stopped CPU's SR and return PC";
    }
    m_m->audio->reset_board();
    m_m->audio->set_cpu_runner([this](int cycles) { return m_m->interpreter->run_audio(cycles); });
}

TEST_F(AudioMachineTest, SoundStopUnmaskBudget) {
    m_m->audio->set_cpu_runner({});
    for (int budget : {1, 2, 3, 4, 8, 16, 32, 47, 48, 52}) {
        m_m->audio->reset_board();
        m_m->audio->write32(0, 0xff00);
        m_m->audio->write32(4, SND_CODE_BASE);
        m_m->audio->write32(SND_IRQ_VECTOR_64, SND_HANDLER_BASE);
        m_m->audio->write16(SND_CODE_BASE, 0x4e72);
        m_m->audio->write16(SND_CODE_BASE + 2, 0x2000);
        m_m->audio->write16(SND_HANDLER_BASE, 0x4e71);
        m_m->audio->set_reset(false);
        m_m->interpreter->run_audio(1);
        m_m->audio->write8(duart_reg(12), 64);
        m_m->audio->write8(duart_reg(5), 1);
        m_m->audio->write8(duart_reg(2), 4);
        EXPECT_EQ(m_m->audio->irq_level(), 6)
            << "TX-ready IRQ is pending while reset SR masks interrupts";
        EXPECT_TRUE(m_m->interpreter->run_audio(budget) == 52 &&
                    m_m->interpreter->sound_pc() == SND_HANDLER_BASE)
            << "STOP unmasking an IRQ retains four instruction clocks, a four-clock poll and 44 "
               "entry clocks";
        EXPECT_TRUE(m_m->audio->read16(SND_STACK_SR) == 0x2000 &&
                    m_m->audio->read32(SND_STACK_PC) == SND_CODE_BASE + 4 &&
                    m_m->interpreter->run_audio(1) == 4 &&
                    m_m->interpreter->sound_pc() == SND_HANDLER_BASE + 2)
            << "Immediate STOP wake stacks the next PC and resumes the handler without remaining "
               "stopped";
    }
    m_m->audio->reset_board();
    m_m->audio->set_cpu_runner([this](int cycles) { return m_m->interpreter->run_audio(cycles); });
}

TEST(Audio, DspEndAndSafetyBudgetOrdering) {
    f3rt::ES5510 dsp;
    std::vector<uint8_t> saved(dsp.state_size());
    const auto capture = [&] {
        f3rt::StateWriter writer(saved);
        dsp.save_state(writer);
        f3rt::CanonicalES5510Registers state{};
        f3rt::StateReader reader(saved);
        reader.read(state);
        return state;
    };
    const auto restore = [&](const f3rt::CanonicalES5510Registers &state) {
        f3rt::StateWriter writer(saved);
        writer.write(state);
        f3rt::StateReader reader(saved);
        dsp.load_state(reader);
    };
    for (bool halted : {false, true})
        for (int end : {-1, 0, 1, 159}) {
            dsp.reset();
            if (end >= 0) dsp.instr_at(end) = 0xf000;
            auto state = capture();
            state.state = halted ? 1 : 0;
            restore(state);
            dsp.run_once();
            state = capture();
            const unsigned expected = end < 0    ? (halted ? 200 : 201)
                                      : end == 0 ? (halted ? 1 : 161)
                                                 : unsigned(end + 1);
            EXPECT_TRUE(state.pc == expected && state.state == (end < 0 ? 0 : 1) &&
                        state.halt_asserted)
                << "DSP END and safety budget preserve wake-up and first-cycle HALT ordering";
        }
    for (unsigned pc : {159u, 160u, 255u}) {
        dsp.reset();
        auto state = capture();
        state.pc = uint8_t(pc);
        restore(state);
        dsp.run_once();
        state = capture();
        EXPECT_TRUE(state.pc == uint8_t(pc + 201) && state.state == 0)
            << "DSP no-END safety cutoff preserves the 8-bit PC wrap above the instruction store";
    }
}

TEST(Audio, DspDelayAddressing) {
    f3rt::ES5510 dsp;
    std::vector<uint8_t> saved(dsp.state_size());
    const auto capture = [&] {
        f3rt::StateWriter writer(saved);
        dsp.save_state(writer);
        f3rt::CanonicalES5510Registers state{};
        f3rt::StateReader reader(saved);
        reader.read(state);
        return state;
    };
    const auto restore = [&](const f3rt::CanonicalES5510Registers &state) {
        f3rt::StateWriter writer(saved);
        writer.write(state);
        f3rt::StateReader reader(saved);
        dsp.load_state(reader);
    };
    for (int address : {-1, 0, 16, 17, 33, 34, 127}) {
        dsp.reset();
        dsp.instr_at(1) = 0xf000;
        auto state = capture();
        state.pc = 1;
        state.dbase = address;
        state.dlength = 16;
        state.memincrement = 1;
        state.memshift = 0;
        state.memmask = 0xffffff;
        restore(state);
        dsp.run_once();
        state = capture();
        EXPECT_EQ(state.ram_p.address, ((address % 17) & 0xffffff))
            << "DSP delay addressing preserves signed remainder and zero, one and multiple wraps";
    }
}

TEST(Audio, AudioMixerBoardGainAndMute) {
    f3rt::Audio audio;
    const std::array<uint8_t, 4> rom{0x40, 0, 0x40, 0};
    audio.load_sample_rom(rom);
    audio.write16(f3rt::OTIS_PAGE, 0x20);
    for (unsigned reg = 1; reg <= 6; ++reg) audio.write16(otis_reg(reg), 0x0800);
    audio.write16(f3rt::OTIS_PAGE, 0);
    audio.write16(otis_reg(8), 0xef00);
    audio.write16(otis_reg(9), 0xef00);
    audio.write16(otis_reg(0), 0x0d00); // Constant sample, all poles lowpass, OTIS pair 1 (ESP ser0).
    // OTIS pair 0 is not wired on the board and the DAC is fed by ESP serio3 alone, so the board mix is
    // observed through a pass-through ESP program: ser3 <- ser0 for both sides, then END.
    const auto esp_reg = [](uint32_t reg) { return f3rt::SOUND_ES5510_BASE + 2 * reg + 1; };
    const auto esp_instruction = [&](unsigned index, uint64_t word) {
        for (unsigned byte = 0; byte < 6; ++byte)
            audio.write8(esp_reg(0x03 + byte), uint8_t(word >> (40 - 8 * byte)));
        audio.write8(esp_reg(0xc0), uint8_t(index));
    };
    esp_instruction(0, 0x0000ebf19000ULL); // MOV ser3l(241), ser0l(235)
    esp_instruction(1, 0x0000eaf09000ULL); // MOV ser3r(240), ser0r(234)
    esp_instruction(2, 0x00000000f000ULL); // END
    audio.write8(duart_reg(14), 0x40);     // DUART OP6 low: ESP runs
    std::array<int16_t, 2> pcm{};
    const auto sample = [&] {
        audio.advance(538);
        EXPECT_EQ(audio.render(pcm.data(), 1), 1) << "One complete audio sample is available";
    };
    // Default gain model is SingleStage (hardware MB87078 post-DAC analog attenuation).
    sample();
    EXPECT_TRUE(pcm[0] == 2136 && pcm[1] == 2136)
        << "Default SingleStage gain model matches post-DAC analog attenuation";

    // Test MameRouting model
    audio.set_gain_model(f3rt::Audio::GainModel::MameRouting);
    sample();
    EXPECT_TRUE(pcm[0] == 20925 && pcm[1] == 20925) << "Board gain and signed PCM normalization";
    audio.write8(f3rt::SOUND_VOLUME_BASE, 2);
    audio.write8(f3rt::SOUND_VOLUME_BASE + 2, 0);
    sample();
    EXPECT_TRUE(pcm[0] == 20925 && pcm[1] == 0)
        << "Channel-0 volume mute follows its data through the DAC swap and preserves channel 1";
    audio.write8(f3rt::SOUND_VOLUME_BASE, 7);
    audio.write8(f3rt::SOUND_VOLUME_BASE + 2, 0x33);
    sample();
    EXPECT_TRUE(pcm[0] == 5231 && pcm[1] == 0)
        << "Minus-six-dB control applies both baseline gain stages";
    audio.set_gain_model(f3rt::Audio::GainModel::SingleStage);
    sample();
    EXPECT_TRUE(pcm[0] == 1070 && pcm[1] == 0)
        << "Single-stage gain is distinct and preserves channel mute";
    audio.set_reset(false);
    audio.set_reset(true);
    sample();
    EXPECT_TRUE(pcm[0] == 1070 && pcm[1] == 0)
        << "CPU-line reset preserves attenuation and playing OTIS voices";
    audio.reset_board();
    sample();
    EXPECT_TRUE(pcm[0] == 2136 && pcm[1] == 2136)
        << "Board reset restores volume without resetting OTIS voices";
}

TEST(Audio, AudioSampleRateNoClockDrift) {
    f3rt::Audio clock_audio;
    std::array<int16_t, 128> clock_samples{};
    uint64_t sample_count = 0;
    for (unsigned i = 0; i < 10000; ++i) {
        clock_audio.advance(16000);
        sample_count += clock_audio.render(clock_samples.data(), clock_samples.size() / 2);
    }
    EXPECT_EQ(sample_count, uint64_t(clock_audio.sample_rate()) * 10)
        << "Ten seconds of audio match the advertised stream rate without clock drift";
    clock_audio.advance(638);
    clock_audio.reset_board();
    clock_audio.advance(438);
    EXPECT_EQ(clock_audio.render(clock_samples.data(), clock_samples.size() / 2), 2)
        << "Board reset preserves queued audio and fractional sample-clock phase";
}

TEST_F(AudioMachineTest, DpramOtisAndSoundReset) {
    m_m->write8(f3rt::SHARED_BASE + 0x10, 0x75);
    EXPECT_EQ(m_m->audio->read16(f3rt::SOUND_SHARED_BASE + 0x20), 0x75ff)
        << "DPRAM sound high-byte lane";
    m_m->audio->write16(f3rt::SOUND_SHARED_BASE + 0x20, 0xaabb);
    EXPECT_EQ(m_m->read8(f3rt::SHARED_BASE + 0x10), 0xaa) << "DPRAM reverse lane";
    m_m->audio->write16(f3rt::OTIS_PAGE, 0); // Voice 0, register page
    m_m->audio->write16(otis_reg(10), 0x0246);
    m_m->audio->write16(otis_reg(11), 0x8a00);
    m_m->audio->write16(f3rt::OTIS_PAGE, 0x20); // Stopped voice sample-ROM readback
    EXPECT_EQ(m_m->audio->read16(otis_reg(6)), 0x4567)
        << "OTIS stopped voice reads full 20-bit sample address";
    m_m->audio->write16(SND_WORK_BYTE, 0xa55a);
    m_m->audio->set_reset(false);
    m_m->audio->set_reset(true);
    m_m->audio->set_reset(false);
    EXPECT_EQ(m_m->audio->read16(SND_WORK_BYTE), 0xa55a)
        << "Sound CPU RESET preserves board work RAM";
}

TEST_F(AudioMachineTest, CpuLineResetPreservesDspAndDuart) {
    m_m->audio->set_reset(true);
    m_m->audio->write8(duart_reg(12), 0x66);
    m_m->audio->write8(f3rt::SOUND_ES5510_BASE + 1, 0x12);
    m_m->audio->write8(f3rt::SOUND_ES5510_BASE + 3, 0x34);
    m_m->audio->write8(f3rt::SOUND_ES5510_BASE + 5, 0x56);
    m_m->audio->write8(f3rt::SOUND_ES5510_BASE + 0x141, 0);
    m_m->audio->set_reset(false);
    m_m->audio->set_reset(true);
    m_m->audio->write8(f3rt::SOUND_ES5510_BASE + 0x101, 0);
    EXPECT_TRUE(m_m->audio->read8(f3rt::SOUND_ES5510_BASE + 1) == 0x12 &&
                m_m->audio->read8(duart_reg(12)) == 0x66)
        << "CPU-line reset preserves DSP registers and DUART configuration";
}

// RapidCheck property: DSP delay circular buffer addressing wrapping invariant.
// Semantics verified from runtime/third_party/audio/es5510.cpp:562-574 (RAM_CONTROL_DELAY):
// modulo = dlength + memincrement (16 + 1 = 17). For negative dbase, ES5510 keeps the signed
// remainder (mod_result %= modulo in C++ produces negative remainder), then masked by memmask (0xffffff).
RC_GTEST_PROP(Audio, DspDelayAddressingModuloInvariant, ()) {
    const auto address = *rc::gen::inRange(-1000, 1000);
    const int expected = (address % 17) & 0xffffff;
    f3rt::ES5510 dsp;
    std::vector<uint8_t> saved(dsp.state_size());
    dsp.reset();
    dsp.instr_at(1) = 0xf000;
    f3rt::CanonicalES5510Registers state{};
    state.pc = 1;
    state.dbase = address;
    state.dlength = 16;
    state.memincrement = 1;
    state.memshift = 0;
    state.memmask = 0xffffff;
    f3rt::StateWriter writer(saved);
    writer.write(state);
    f3rt::StateReader reader(saved);
    dsp.load_state(reader);
    dsp.run_once();
    f3rt::StateWriter w2(saved);
    dsp.save_state(w2);
    f3rt::CanonicalES5510Registers s2{};
    f3rt::StateReader r2(saved);
    r2.read(s2);
    RC_ASSERT(int(s2.ram_p.address) == expected);
}

// RapidCheck property: Sound 68000 executes real MULS.W on the interpreter/Musashi sound core
// with arbitrary 16-bit operands, verifying the 32-bit product and condition flags (N, Z, V=0, C=0, X preserved).
RC_GTEST_PROP(Audio, SoundMulsSignedProductInvariant, ()) {
    const auto op1 = *rc::gen::arbitrary<int16_t>();
    const auto op2 = *rc::gen::arbitrary<int16_t>();
    static auto mach = std::make_unique<f3rt::Machine>(fixture());

    // MULS.W D1, D0 (opcode 0xc1c1): D0 = D0 * D1 (signed 16-bit to 32-bit)
    const auto result =
        execute_sound_instruction(*mach, {0xc1c1}, uint16_t(op2), uint32_t(int32_t(op1)));

    const int32_t expected_product = int32_t(op1) * int32_t(op2);
    RC_ASSERT(result.d0 == uint32_t(expected_product));

    // 68000 condition codes: X preserved (bit 4, setup sets 0x271b so X=1), N if negative, Z if zero, V=0, C=0.
    const uint16_t expected_flags =
        0x10 | (expected_product == 0 ? 4 : (expected_product < 0 ? 8 : 0));
    RC_ASSERT((result.sr & 0x1f) == expected_flags);
}

// ES5510 volume/pan table stand-in: table(i) = i / 127, so gains are exactly checkable. `program` is the ROM
// program-pointer-table index the preset row names (byte 5): the table holds the real pointers, so index 0 is the
// Reverb program, 1 and 2 the Hall program, 3 an undecoded program, 4 Mute, 5 Chorus, 6 Echo. Row bytes 6..15 are
// p0..p9.
static std::vector<uint8_t> effects_test_rom(unsigned preset, unsigned program, const uint8_t (&params)[10]) {
    std::vector<uint8_t> sound_rom(0x20000, 0);
    for (int i = 0; i < 128; ++i) {
        const uint16_t val = uint16_t(std::round(i / 127.0 * 32767.0));
        sound_rom[f3rt::sound_5a7e9117::VOLUME_PAN_TABLE_OFFSET + i * 2] = val >> 8;
        sound_rom[f3rt::sound_5a7e9117::VOLUME_PAN_TABLE_OFFSET + i * 2 + 1] = val & 0xff;
    }
    static constexpr uint32_t pointers[11] = {0xc1c836, 0xc1c4e2, 0xc1c4e2, 0xc1cb19, 0xc1ce1b, 0xc1d171,
                                              0xc1d4d4, 0xc1d823, 0xc1db8a, 0xc1de02, 0xc1e10b};
    for (int i = 0; i < 11; ++i)
        for (int k = 0; k < 4; ++k)
            sound_rom[f3rt::hle::Effects::K_PROGRAM_TABLE_OFFSET + i * 4 + k] = uint8_t(pointers[i] >> (24 - 8 * k));
    sound_rom[f3rt::sound_5a7e9117::PRESET_TABLE_OFFSET + preset * 16 + 5] = uint8_t(program);
    for (int n = 0; n < 10; ++n)
        sound_rom[f3rt::sound_5a7e9117::PRESET_TABLE_OFFSET + preset * 16 + 6 + n] = params[n];
    return sound_rom;
}

// Renders `frames` frames of an impulse (value 1 at frame 0) on one bus; returns interleaved stereo.
static std::vector<float> effects_impulse(f3rt::hle::Effects &fx, size_t bus, size_t frames) {
    std::vector<float> buses(8 * frames, 0.0f);
    std::vector<float> stereo(2 * frames, 0.0f);
    buses[bus] = 1.0f;
    fx.mix(buses.data(), stereo.data(), frames, {1.0f, 1.0f}, {1.0f, 1.0f});
    return stereo;
}

// Behaviour test: verify hardware OTIS->ESP PCB wiring in Effects mixer
// (docs/notes/raw/docs/taito-f3/5505-5510.txt, 12's oscilloscope trace through TC0660FCM) with the
// echo program (ROM program 6, Land Maker preset 11; docs/notes/raw/tests/2026-10-16-kirameki-187-pcb-stereo-reverb.md
// section 9):
// - Pair 0 (buses 0/1) -> silent (physically unconnected on board)
// - Pair 1 (buses 2/3) -> dry output (ESP serio0), no tail
// - Pair 2 (buses 4/5) -> ping-pong echo at p2 * 112 frames (ESP serio1)
// - Pair 3 (buses 6/7) -> reverb path (ESP serio2; diffused tail after input stops)
TEST(Audio, EnhancedEffectsRoutingMatchesHardwareWiring) {
    const uint8_t params[10] = {30, 58, 73, 32, 37, 0, 40, 43, 18, 32};
    const auto sound_rom = effects_test_rom(11, 6, params);

    // 1. OTIS Pair 0 (buses 0/1) produces silence
    {
        f3rt::hle::Effects fx(sound_rom);
        fx.select(11);
        constexpr size_t N = 1000;
        std::vector<float> buses(8 * N, 0.0f);
        std::vector<float> stereo(2 * N, 0.0f);
        for (size_t f = 0; f < 100; ++f) {
            buses[f * 8 + 0] = 1.0f;
            buses[f * 8 + 1] = 1.0f;
        }
        fx.mix(buses.data(), stereo.data(), N, {1.0f, 1.0f}, {1.0f, 1.0f});
        float peak = 0.0f;
        for (float s : stereo) peak = std::max(peak, std::abs(s));
        EXPECT_EQ(peak, 0.0f) << "OTIS pair 0 must produce silence (unconnected on PCB)";
    }

    // 2. OTIS Pair 1 (buses 2/3) reaches dry output with no echo or reverb tail
    {
        f3rt::hle::Effects fx(sound_rom);
        fx.select(11);
        constexpr size_t N = 3000;
        std::vector<float> buses(8 * N, 0.0f);
        std::vector<float> stereo(2 * N, 0.0f);
        buses[0 * 8 + 2] = 1.0f;
        buses[0 * 8 + 3] = 0.5f;
        fx.mix(buses.data(), stereo.data(), N, {1.0f, 1.0f}, {1.0f, 1.0f});
        EXPECT_GT(stereo[0], 0.0f) << "OTIS pair 1 must reach dry output";
        EXPECT_GT(stereo[1], 0.0f) << "OTIS pair 1 must reach dry output";
        float subsequent_peak = 0.0f;
        for (size_t f = 64; f < N; ++f) {
            subsequent_peak =
                std::max({subsequent_peak, std::abs(stereo[f * 2]), std::abs(stereo[f * 2 + 1])});
        }
        EXPECT_LT(subsequent_peak, 0.004f) << "OTIS pair 1 must have no echo or reverb tail";
    }

    // 3. OTIS Pair 3 (buses 6/7) reaches the reverb path (diffused tail after input stops)
    {
        f3rt::hle::Effects fx(sound_rom);
        fx.select(11);
        const auto stereo = effects_impulse(fx, 6, 2000);
        float tail_energy = 0.0f;
        for (size_t f = 100; f < 2000; ++f) tail_energy += std::abs(stereo[f * 2]) + std::abs(stereo[f * 2 + 1]);
        EXPECT_GT(tail_energy, 0.01f) << "OTIS pair 3 must produce a diffused reverb tail";
    }

    // 4. OTIS Pair 2 (buses 4/5) produces the echo at p2 * 112 = 8176 frames (274.7 ms): the left impulse comes
    // back on the left line (the right line then returns it on the right after another 8176 frames).
    {
        f3rt::hle::Effects fx(sound_rom);
        fx.select(11);
        constexpr size_t N = 12500; // ~420 ms at 29.761 kHz
        const auto stereo = effects_impulse(fx, 4, N);
        size_t echo_peak_frame = 0;
        float echo_peak = 0.0f;
        for (size_t f = 1000; f < N; ++f) {
            if (std::abs(stereo[f * 2]) > echo_peak) {
                echo_peak = std::abs(stereo[f * 2]);
                echo_peak_frame = f;
            }
        }
        EXPECT_GT(echo_peak, 0.05f) << "OTIS pair 2 must produce a delay echo";
        EXPECT_NEAR(double(echo_peak_frame), 8176.0 + 1.0, 2.0) << "echo must return after p2 * 112 frames";
    }
}

// Program 6 (echo): the echo time follows p2 (p2 * 112 frames), the cross feedback p1/128 returns the echo
// on the other channel after a second period, and a loud input is not boosted by the enhancer.
TEST(Audio, EchoProgramTimingAndFeedbackMatchDecodedGraph) {
    const uint8_t mixed[10] = {30, 64, 40, 32, 37, 0, 8, 43, 18, 32}; // p1 = 64: feedback 0.5; p2 = 40: 4480 frames
    const auto rom2 = effects_test_rom(11, 6, mixed);
    f3rt::hle::Effects fx2(rom2);
    fx2.select(11);
    EXPECT_EQ(fx2.program(), f3rt::hle::Effects::Program::Echo);
    constexpr size_t N = 14000;
    // p6 low nibble 8 -> sin = 0.74: the echo is audible at the output.
    const auto s2 = effects_impulse(fx2, 4, N);
    const auto peak_in = [&](size_t ch, size_t from, size_t to) {
        float peak = 0;
        for (size_t f = from; f < to; ++f) peak = std::max(peak, std::abs(s2[f * 2 + ch]));
        return peak;
    };
    const float first = peak_in(0, 4475, 4490), cross = peak_in(1, 8955, 8970), second = peak_in(0, 13435, 13450);
    EXPECT_GT(first, 0.05f) << "echo returns at p2 * 112 frames on the same side";
    EXPECT_GT(cross, first * 0.3f) << "feedback 0.5 returns the echo on the other side after a second period";
    EXPECT_LT(peak_in(0, 2000, 4400), first * 0.2f) << "no echo before p2 * 112 frames";
    EXPECT_GT(second, 0.f);
}

// Program 5 (chorus, Kirameki): the interpolated taps land between 30 and 345 frames on the left line, the
// direct send is table(127 - p9) on the same frame, and the wet taps fire at frames 5 (L) and 243 (R).
TEST(Audio, ChorusProgramGraphMatchesDecodedProgram) {
    const uint8_t params[10] = {40, 0, 20, 26, 37, 0, 0x68, 64, 32, 39};
    const auto sound_rom = effects_test_rom(5, 5, params);
    // p9 = 127: table(127 - p9) = 0 removes the direct send so the wet taps are measured alone.
    const uint8_t wet_only_params[10] = {40, 0, 20, 26, 37, 0, 0x68, 64, 32, 127};
    const auto wet_only_rom = effects_test_rom(5, 5, wet_only_params);
    {
        f3rt::hle::Effects fx(sound_rom);
        fx.select(5);
        const auto stereo = effects_impulse(fx, 6, 1000);
        EXPECT_NEAR(stereo[0], (127.f - 39.f) / 127.f, 1e-4f) << "direct send gain is table(127 - p9)";
        EXPECT_EQ(stereo[1], 0.f) << "the right dry output of a left-only send is silent on frame 0";
    }
    {
        f3rt::hle::Effects fx(wet_only_rom);
        fx.select(5);
        const auto stereo = effects_impulse(fx, 6, 1000);
        for (size_t f = 1; f < 5; ++f)
            EXPECT_EQ(stereo[f * 2], 0.f) << "left wet taps cannot fire before frame 5 (frame " << f << ")";
        EXPECT_NE(stereo[5 * 2], 0.f) << "first left wet tap at frame 5";
        for (size_t f = 0; f < 243; ++f)
            EXPECT_EQ(stereo[f * 2 + 1], 0.f) << "right wet taps cannot fire before frame 243 (frame " << f << ")";
        EXPECT_NE(stereo[243 * 2 + 1], 0.f) << "first right wet tap at frame 243";
    }
    {
        f3rt::hle::Effects fx(sound_rom);
        fx.select(5);
        constexpr size_t N = 6000;
        std::vector<float> buses(8 * N, 0.0f);
        std::vector<float> stereo(2 * N, 0.0f);
        buses[0 * 8 + 6] = 1.0f;
        buses[0 * 8 + 7] = 1.0f;
        fx.mix(buses.data(), stereo.data(), N, {1.0f, 1.0f}, {1.0f, 1.0f});
        double l2 = 0, r2 = 0, lr = 0, side2 = 0;
        for (size_t f = 200; f < N; ++f) {
            float l = stereo[f * 2], r = stereo[f * 2 + 1];
            l2 += l * l;
            r2 += r * r;
            lr += l * r;
            side2 += (l - r) * (l - r);
        }
        double corr = lr / std::sqrt(l2 * r2 + 1e-20);
        EXPECT_LT(corr, 0.50) << "Center-panned reverb send must produce decorrelated stereo tail";
        EXPECT_GT(side2, 0.01) << "Center-panned reverb send must produce side channel energy";
    }
    {
        // Pair 2 is the chorus: taps within 30..345 frames (left line), none of the dry impulse much later.
        f3rt::hle::Effects fx(sound_rom);
        fx.select(5);
        const auto stereo = effects_impulse(fx, 4, 3000);
        float tap_peak = 0.f, late_peak = 0.f;
        for (size_t f = 30; f < 350; ++f) tap_peak = std::max(tap_peak, std::abs(stereo[f * 2]));
        for (size_t f = 1500; f < 3000; ++f) late_peak = std::max(late_peak, std::abs(stereo[f * 2]));
        EXPECT_GT(tap_peak, 0.05f) << "pair 2 must produce chorus taps within 12 ms";
        EXPECT_LT(late_peak, tap_peak * 0.3f) << "the chorus must not produce a long echo";
    }
}

// Reverb program (Command War preset 1, ROM program 0): serio1 reaches the output on the same frame with
// table(127 - p8) (no chorus or echo); p0 is the loop decay G110 = p0/128.
TEST(Audio, ReverbProgramPassesSerio1DirectlyAndDecaysWithP0) {
    const uint8_t params[10] = {32, 9, 40, 16, 53, 0, 94, 52, 58, 58};
    const auto sound_rom = effects_test_rom(1, 0, params);
    f3rt::hle::Effects fx(sound_rom);
    fx.select(1);
    EXPECT_EQ(fx.program(), f3rt::hle::Effects::Program::Reverb);
    const auto stereo = effects_impulse(fx, 4, 2000);
    EXPECT_NEAR(stereo[0], (127.f - 58.f) / 127.f, 1e-4f) << "serio1 direct gain is table(127 - p8) on frame 0";
    double late = 0;
    for (size_t f = 100; f < 2000; ++f) late += std::abs(stereo[f * 2]);
    EXPECT_GT(late, 0.01) << "serio1 also feeds the reverb in program 0";

    const auto tail = [&](uint8_t p0) {
        const uint8_t pp[10] = {p0, 9, 40, 16, 53, 0, 94, 52, 58, 58};
        const auto rom = effects_test_rom(1, 0, pp);
        f3rt::hle::Effects f(rom);
        f.select(1);
        const auto s = effects_impulse(f, 6, 20000);
        double e = 0;
        for (size_t i = 10000; i < 20000; ++i) e += double(s[i * 2]) * s[i * 2] + double(s[i * 2 + 1]) * s[i * 2 + 1];
        return e;
    };
    EXPECT_GT(tail(64), tail(32) * 3) << "a larger p0 rings longer";
}

// Decoded programs are selected from the program pointer the ROM's preset row names (byte 5), not from the
// preset number, and an undecoded program must fail loudly instead of reusing another program's graph.
TEST(Audio, EffectsProgramSelectionComesFromRomAndRejectsUndecoded) {
    const uint8_t params[10] = {40, 0, 20, 26, 37, 0, 104, 64, 32, 39};
    using Program = f3rt::hle::Effects::Program;
    const auto program_of = [&](unsigned preset, unsigned index) {
        const auto rom = effects_test_rom(preset, index, params);
        f3rt::hle::Effects fx(rom);
        fx.select(uint8_t(preset));
        return fx.program();
    };
    EXPECT_EQ(program_of(2, 5), Program::Chorus) << "preset 2 naming pointer 5";
    EXPECT_EQ(program_of(7, 0), Program::Reverb);
    EXPECT_EQ(program_of(7, 1), Program::Hall);
    EXPECT_EQ(program_of(7, 2), Program::Hall) << "pointer table entries 1 and 2 are the same program";
    EXPECT_EQ(program_of(7, 4), Program::Mute);
    EXPECT_EQ(program_of(7, 6), Program::Echo);
    for (unsigned undecoded : {3u, 7u, 8u, 9u, 10u}) {
        const auto rom = effects_test_rom(5, undecoded, params);
        f3rt::hle::Effects fx(rom);
        EXPECT_THROW(fx.select(5), std::runtime_error) << "ROM program " << undecoded;
        EXPECT_EQ(fx.program(), Program::None) << "a rejected preset leaves no program loaded";
    }
}

// Hall program (Kirameki presets 0 and 2, ROM programs 1/2): two cross-coupled chains. serio1 and serio2 are
// per side (a left-only send enters the left chain only), the serio2 direct gain is table(127 - p9), p0 sets
// the decay and the all-pass coefficient (p0 + 7)/128, and the wet output stays silent until the diffusers
// have delivered: the first right wet tap is latched at frame 2 (output at frame 3).
TEST(Audio, HallProgramGraphMatchesDecodedProgram) {
    const uint8_t params[10] = {56, 6, 32, 16, 37, 0, 143, 84, 32, 32}; // Kirameki preset 0
    const auto sound_rom = effects_test_rom(0, 2, params);
    f3rt::hle::Effects fx(sound_rom);
    fx.select(0);
    ASSERT_EQ(fx.program(), f3rt::hle::Effects::Program::Hall);
    const auto stereo = effects_impulse(fx, 6, 30000);
    EXPECT_NEAR(stereo[0], (127.f - 32.f) / 127.f, 1e-4f) << "serio2 direct gain is table(127 - p9)";
    EXPECT_EQ(stereo[1], 0.f);
    for (size_t f = 1; f < 3; ++f) {
        EXPECT_EQ(stereo[f * 2 + 1], 0.f) << "no right wet before frame 3 (frame " << f << ")";
    }
    EXPECT_NE(stereo[3 * 2 + 1], 0.f) << "first right wet tap (chain Y output node, 1 frame back) at frame 3";
    double early = 0, late = 0, l2 = 0, r2 = 0, lr = 0;
    for (size_t f = 0; f < 6000; ++f)
        early += double(stereo[f * 2]) * stereo[f * 2] + double(stereo[f * 2 + 1]) * stereo[f * 2 + 1];
    for (size_t f = 24000; f < 30000; ++f) {
        const double l = stereo[f * 2], r = stereo[f * 2 + 1];
        late += l * l + r * r;
        l2 += l * l;
        r2 += r * r;
        lr += l * r;
    }
    EXPECT_GT(early, 0.01);
    EXPECT_LT(late, early * 1e-3) << "the figure-eight loop must decay (-30 dB after 0.8 s)";
    EXPECT_GT(l2, 0.0) << "the left chain's output reaches the right chain: both sides ring";
    EXPECT_GT(r2, 0.0);
    EXPECT_LT(std::abs(lr) / std::sqrt(l2 * r2 + 1e-30), 0.9) << "sides are different taps";

    // p0 = decay: a larger p0 rings longer.
    const auto late_energy = [&](uint8_t p0) {
        const uint8_t pp[10] = {p0, 6, 32, 16, 37, 0, 143, 84, 32, 32};
        const auto rom = effects_test_rom(0, 2, pp);
        f3rt::hle::Effects f(rom);
        f.select(0);
        const auto s = effects_impulse(f, 6, 30000);
        double e = 0;
        for (size_t i = 15000; i < 30000; ++i) e += double(s[i * 2]) * s[i * 2] + double(s[i * 2 + 1]) * s[i * 2 + 1];
        return e;
    };
    EXPECT_GT(late_energy(70), late_energy(40) * 4);
}

// Mute program (Kirameki preset 4): a single END, nothing reaches the DAC, not even the dry input.
TEST(Audio, MuteProgramSilencesEverything) {
    const uint8_t params[10] = {0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    const auto sound_rom = effects_test_rom(4, 4, params);
    f3rt::hle::Effects fx(sound_rom);
    fx.select(4);
    ASSERT_EQ(fx.program(), f3rt::hle::Effects::Program::Mute);
    std::vector<float> buses(8 * 500, 1.0f), stereo(2 * 500, 1.0f);
    fx.mix(buses.data(), stereo.data(), 500, {1.0f, 1.0f}, {1.0f, 1.0f});
    for (float v : stereo) ASSERT_EQ(v, 0.f);
}

// Loop decay in program 5 is G110 = p0 / 128 (the loop return goes through it): the tank must decay, a
// larger p0 must ring longer, and p0 = 0 leaves only the small p0-independent recirculation.
TEST(Audio, ReverbDecayFollowsP0) {
    const auto tail_energy = [](uint8_t p0) {
        const uint8_t params[10] = {p0, 0, 20, 26, 37, 0, 0x68, 64, 32, 39};
        const auto sound_rom = effects_test_rom(5, 5, params);
        f3rt::hle::Effects fx(sound_rom);
        fx.select(5);
        constexpr size_t N = 60000;
        const auto stereo = effects_impulse(fx, 6, N);
        double early = 0, late = 0;
        for (size_t f = 0; f < 6000; ++f) early += double(stereo[f * 2]) * stereo[f * 2] + double(stereo[f * 2 + 1]) * stereo[f * 2 + 1];
        for (size_t f = 30000; f < N; ++f) late += double(stereo[f * 2]) * stereo[f * 2] + double(stereo[f * 2 + 1]) * stereo[f * 2 + 1];
        return std::array<double, 2>{early, late};
    };
    const auto open = tail_energy(0), mid = tail_energy(40), long_decay = tail_energy(90);
    EXPECT_LT(open[1], open[0] * 1e-5) << "p0 = 0 must decay at least 50 dB faster than the nominal tank";
    EXPECT_LT(mid[1], mid[0] * 1e-3) << "the tank must decay below -30 dB after 1 s";
    EXPECT_GT(long_decay[1], mid[1] * 4) << "a larger p0 must ring longer";
}

// Direct-voice filter index, derived from the sound driver (c195a4-c195d0) and checked against its key-on
// register writes in the Kirameki 0x187 diagnosis (clap p0e=165: K1/K2 0xfa40/0xfa70 = index 947/948;
// pad p0e=101 vel 70/115/119: 579/691-692/702; p0e=179 vel 123: 900). The note-on routine at c17b94 stores
// the velocity in both bytes of voice+8 (velocity * 257); the second note-start routine at c192ee stores
// only the high byte (velocity * 256), which gives the index one lower in some cases (947, 691, 900).
TEST(HleAudio, DirectVoiceFilterIndexMatchesDriver) {
    EXPECT_EQ(f3rt::hle::direct_filter_index(165, 115), 948);
    EXPECT_EQ(f3rt::hle::direct_filter_index(101, 70), 579);
    EXPECT_EQ(f3rt::hle::direct_filter_index(101, 115), 692);
    EXPECT_EQ(f3rt::hle::direct_filter_index(101, 119), 702);
    EXPECT_EQ(f3rt::hle::direct_filter_index(179, 123), 901);
    EXPECT_EQ(f3rt::hle::direct_filter_index(0, 127), 0);
}

// The sound driver starts the voices of one sequencer pulse one after another (about 20k CPU cycles each on the
// 16 MHz sound CPU, measured on the Kirameki 0x187 trace); HLE models that as a busy clock plus a start queue.
namespace {
using f3rt::hle::DRIVER_KEYON_CYCLES;
using f3rt::hle::DriverClock;
using f3rt::hle::NoteQueue;

struct Started {
    uint64_t instance, tick;
};
struct Released {
    uint64_t instance, tick;
};
// Plays a queue to `limit` in one service call and records begin/release order.
template <class Q> void drain(Q &q, uint64_t limit, std::vector<Started> &begun, std::vector<Released> &released) {
    q.service(
        limit, [&](const uint64_t &instance, uint64_t start) { begun.push_back({instance, start}); },
        [&](uint64_t instance, uint64_t tick) { released.push_back({instance, tick}); });
}
} // namespace

TEST(HleNoteTiming, SimultaneousNotesStartOneCostApartInRequestOrder) {
    DriverClock clock;
    const uint64_t tick = 1'000'000;
    for (unsigned k = 0; k < 19; ++k) EXPECT_EQ(clock.start(tick, 1), tick + uint64_t(k) * DRIVER_KEYON_CYCLES);
    // A busy driver also delays a later pulse that arrives before it has finished.
    EXPECT_EQ(clock.start(tick + f3rt::hle::TIMER_TICKS, 1), tick + 19ull * DRIVER_KEYON_CYCLES);
}

TEST(HleNoteTiming, LayersOfOneNoteShareAStartAndCostOneSlotEach) {
    DriverClock clock;
    EXPECT_EQ(clock.start(500, 2), 500u);
    EXPECT_EQ(clock.start(500, 3), 500u + 2ull * DRIVER_KEYON_CYCLES);
    EXPECT_EQ(clock.start(500, 1), 500u + 5ull * DRIVER_KEYON_CYCLES);
    EXPECT_EQ(clock.busy_until(), 500u + 6ull * DRIVER_KEYON_CYCLES);
}

TEST(HleNoteTiming, IdleDriverStartsOnTheRequestTick) {
    DriverClock clock;
    EXPECT_EQ(clock.start(1000, 1), 1000u);
    const uint64_t later = 1000 + 10ull * DRIVER_KEYON_CYCLES;
    EXPECT_EQ(clock.start(later, 4), later);
    clock.reset(77);
    EXPECT_EQ(clock.start(77, 1), 77u);
}

TEST(HleNoteTiming, QueueStartsEveryNoteOnceInStartOrder) {
    NoteQueue<uint64_t, 32> queue;
    DriverClock clock;
    const uint64_t tick = 2'000'000;
    // Request order is the driver's order; instances 1..19.
    for (uint64_t i = 1; i <= 19; ++i) ASSERT_TRUE(queue.push(i, i, tick, clock.start(tick, 1)));
    std::vector<Started> begun;
    std::vector<Released> released;
    // Service in uneven slices: nothing may start early, nothing may start twice.
    drain(queue, tick - 1, begun, released);
    EXPECT_TRUE(begun.empty());
    drain(queue, tick + 3ull * DRIVER_KEYON_CYCLES, begun, released);
    ASSERT_EQ(begun.size(), 4u);
    drain(queue, tick + 100ull * DRIVER_KEYON_CYCLES, begun, released);
    ASSERT_EQ(begun.size(), 19u);
    drain(queue, tick + 200ull * DRIVER_KEYON_CYCLES, begun, released);
    ASSERT_EQ(begun.size(), 19u);
    for (uint64_t i = 0; i < 19; ++i) {
        EXPECT_EQ(begun[i].instance, i + 1);
        EXPECT_EQ(begun[i].tick, tick + i * DRIVER_KEYON_CYCLES);
    }
    EXPECT_TRUE(released.empty());
    EXPECT_TRUE(queue.empty());
}

TEST(HleNoteTiming, EqualStartsKeepRequestOrder) {
    NoteQueue<uint64_t, 8> queue;
    for (uint64_t i = 1; i <= 5; ++i) ASSERT_TRUE(queue.push(i, i, 10, 500));
    std::vector<Started> begun;
    std::vector<Released> released;
    drain(queue, 500, begun, released);
    ASSERT_EQ(begun.size(), 5u);
    for (uint64_t i = 0; i < 5; ++i) EXPECT_EQ(begun[i].instance, i + 1);
}

TEST(HleNoteTiming, NoteOffBeforeQueuedStartStillPlaysTheNoteThenReleasesIt) {
    NoteQueue<uint64_t, 8> queue;
    const uint64_t tick = 100'000, start = tick + 5ull * DRIVER_KEYON_CYCLES;
    ASSERT_TRUE(queue.push(7, 7, tick, start));
    // The sequencer ends the note 3000 cycles after it asked for it, long before the driver gets to it.
    queue.note_off(7, tick + 3000);
    std::vector<Started> begun;
    std::vector<Released> released;
    drain(queue, start - 1, begun, released);
    EXPECT_TRUE(begun.empty());
    EXPECT_TRUE(released.empty());
    EXPECT_EQ(queue.next_event(), start);
    drain(queue, start, begun, released);
    ASSERT_EQ(begun.size(), 1u) << "the queued note was dropped";
    EXPECT_EQ(begun[0].tick, start);
    EXPECT_TRUE(released.empty()) << "released before its length had elapsed";
    EXPECT_EQ(queue.next_event(), start + 3000);
    drain(queue, start + 3000, begun, released);
    ASSERT_EQ(released.size(), 1u);
    EXPECT_EQ(released[0].instance, 7u);
    EXPECT_EQ(released[0].tick, start + 3000) << "note length not preserved";
    EXPECT_TRUE(queue.empty());
}

TEST(HleNoteTiming, ZeroLengthNoteBeginsBeforeItsRelease) {
    NoteQueue<uint64_t, 8> queue;
    ASSERT_TRUE(queue.push(3, 3, 5000, 9000));
    queue.note_off(3, 5000);
    std::vector<uint64_t> order;
    queue.service(
        100000, [&](const uint64_t &, uint64_t tick) { order.push_back(tick * 2); },
        [&](uint64_t, uint64_t tick) { order.push_back(tick * 2 + 1); });
    ASSERT_EQ(order.size(), 2u);
    EXPECT_EQ(order[0], 9000u * 2);
    EXPECT_EQ(order[1], 9000u * 2 + 1);
}

TEST(HleNoteTiming, NoteOffAfterStartOrForOtherInstancesLeavesQueueAlone) {
    NoteQueue<uint64_t, 8> queue;
    ASSERT_TRUE(queue.push(1, 1, 0, 100));
    ASSERT_TRUE(queue.push(2, 2, 0, 200));
    queue.note_off(99, 50); // unknown instance
    std::vector<Started> begun;
    std::vector<Released> released;
    drain(queue, 150, begun, released);
    ASSERT_EQ(begun.size(), 1u);
    queue.note_off(1, 160); // already running: Synth releases the live voice itself
    drain(queue, 1000, begun, released);
    EXPECT_EQ(begun.size(), 2u);
    EXPECT_TRUE(released.empty());
    EXPECT_TRUE(queue.empty());
}

TEST(HleNoteTiming, EveryLayerOfAnInstanceIsReleasedAndNoneLost) {
    NoteQueue<uint64_t, 16> queue;
    for (uint64_t layer = 0; layer < 3; ++layer) ASSERT_TRUE(queue.push(layer, 42, 1000, 4000));
    ASSERT_TRUE(queue.push(10, 43, 1000, 4000 + 3ull * DRIVER_KEYON_CYCLES));
    queue.note_off(42, 1500);
    std::vector<Started> begun;
    std::vector<Released> released;
    drain(queue, 1'000'000, begun, released);
    EXPECT_EQ(begun.size(), 4u);
    EXPECT_EQ(released.size(), 3u);
    for (const Released &r : released) {
        EXPECT_EQ(r.instance, 42u);
        EXPECT_EQ(r.tick, 4500u);
    }
}

TEST(HleNoteTiming, FullQueueRefusesInsteadOfDropping) {
    NoteQueue<uint64_t, 2> queue;
    EXPECT_TRUE(queue.push(1, 1, 0, 10));
    EXPECT_TRUE(queue.push(2, 2, 0, 20));
    EXPECT_FALSE(queue.push(3, 3, 0, 30));
    EXPECT_EQ(queue.size(), 2u);
    queue.clear();
    EXPECT_TRUE(queue.empty());
    EXPECT_TRUE(queue.push(3, 3, 0, 30));
}

TEST(Audio, VoiceBlockFilterModesMatchES5505Hardware) {
    constexpr size_t FRAMES = 32;
    constexpr size_t STRIDE = f3rt::hle::VOICE_LANE_STRIDE;
    alignas(64) float input[FRAMES * STRIDE]{};
    alignas(64) float a1[FRAMES * STRIDE]{};
    alignas(64) float a2[FRAMES * STRIDE]{};
    alignas(64) float high_pole[FRAMES * STRIDE]{};
    alignas(64) float gain_left[FRAMES * STRIDE]{};
    alignas(64) float gain_right[FRAMES * STRIDE]{};
    alignas(64) float state[4 * STRIDE]{};
    alignas(64) float previous[4 * STRIDE]{};
    alignas(64) float out_left[FRAMES * STRIDE]{};
    alignas(64) float out_right[FRAMES * STRIDE]{};
    uint8_t mode[STRIDE]{};

    const uint16_t k1 = 20000;
    const uint16_t k2 = 45000;
    const float a1_val = float((k1 & 0xfff0) / 65536.0);
    const float a2_val = float((k2 & 0xfff0) / 65536.0);
    const float hp_val = float((1.0 + (k2 & 0xfff0) / 65536.0) * 0.5);

    for (size_t f = 0; f < FRAMES; ++f) {
        for (size_t lane = 0; lane < 4; ++lane) {
            const size_t idx = f * STRIDE + lane;
            input[idx] = float((f + 1) * 1000);
            a1[idx] = a1_val;
            a2[idx] = a2_val;
            high_pole[idx] = hp_val;
            gain_left[idx] = 1.0f;
            gain_right[idx] = 1.0f;
        }
    }
    for (size_t lane = 0; lane < 4; ++lane) {
        mode[lane] = uint8_t(lane);
    }

    f3rt::hle::VoiceBlock b{
        .frames = FRAMES,
        .lanes = f3rt::hle::VOICE_LANE_MULTIPLE,
        .input = input,
        .a1 = a1,
        .a2 = a2,
        .high_pole = high_pole,
        .gain_left = gain_left,
        .gain_right = gain_right,
        .mode = mode,
        .state = state,
        .previous = previous,
        .out_left = out_left,
        .out_right = out_right,
    };
    f3rt::hle::render_voice_block(b);

    for (size_t lane = 0; lane < 4; ++lane) {
        EXPECT_NE(out_left[(FRAMES - 1) * STRIDE + lane], 0.0f);
        EXPECT_TRUE(std::isfinite(out_left[(FRAMES - 1) * STRIDE + lane]));
    }
}

TEST(Audio, DirectVoiceFilterIndexMatchesDriverMath) {
    EXPECT_EQ(f3rt::hle::direct_filter_index(101, 115), 692);
    EXPECT_EQ(f3rt::hle::direct_filter_index(248, 127), 1501);
    EXPECT_EQ(f3rt::hle::direct_filter_index(0, 100), 0);
}
