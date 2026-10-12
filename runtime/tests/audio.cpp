#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/rom.hpp"
#include "audio/hle/effects.hpp"
#include "audio/hle/events.hpp"
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
constexpr uint32_t SND_PROGRAM = 0x100;          // mailbox-copy program image
constexpr uint32_t SND_IRQ_VECTOR_64 = 0x100;    // vector slot for DUART vector 64
constexpr uint32_t SND_CODE_BASE = 0x1000;       // foreground / executed-instruction code
constexpr uint32_t SND_HANDLER_BASE = 0x2000;    // DUART IRQ handler
constexpr uint32_t SND_EXEC_SOURCE = 0x4000;     // operand scratch
constexpr uint32_t SND_RESULT_D0 = 0x1500;
constexpr uint32_t SND_RESULT_A1 = 0x1504;
constexpr uint32_t SND_RESULT_SR = 0x1508;
constexpr uint32_t SND_WORK_BYTE = 0x600;        // mailbox copy destination
constexpr uint32_t SND_IRQ_RECORDS = 0x600;      // ten 4-byte IRQ records
constexpr uint16_t hi16(uint32_t a) { return uint16_t(a >> 16); }
constexpr uint16_t lo16(uint32_t a) { return uint16_t(a); }
// DUART register n on the sound bus (odd byte lane).
constexpr uint32_t duart_reg(uint32_t reg) { return f3rt::SOUND_DUART_BASE + 2 * reg + 1; }
constexpr uint32_t SND_STACK_SR = 0xfefa;   // stacked SR of a vectored IRQ entry
constexpr uint32_t SND_STACK_PC = 0xfefc;   // stacked return PC of a vectored IRQ entry
// ES5505 (OTIS) register n on the sound bus (16-bit, even byte lane).
constexpr uint32_t otis_reg(uint32_t reg) { return f3rt::SOUND_ES5505_BASE + 2 * reg; }
std::unique_ptr<f3rt::Machine> make_sound_machine() {
    auto roms = fixture();
    roms.sound[2] = 0x80;
    roms.sound[6] = 1; // SSP 8000, PC 100.
    auto m = std::make_unique<f3rt::Machine>(std::move(roms));
    // Real sound CPU: snapshot mailbox byte zero, post a reply, then loop.
    const uint16_t code[] = {0x13f9, hi16(f3rt::SOUND_SHARED_BASE), lo16(f3rt::SOUND_SHARED_BASE), 0x0000, lo16(SND_WORK_BYTE),
                             0x13fc, 0x005a, hi16(f3rt::SOUND_SHARED_BASE + 2), lo16(f3rt::SOUND_SHARED_BASE + 2), 0x60fe};
    for (unsigned i = 0; i < std::size(code); ++i)
        m->audio->write16(SND_PROGRAM + 2 * i, code[i]);
    m->write8(f3rt::SHARED_BASE, 0x11);
    return m;
}

struct SoundExecResult { int cycles; uint32_t d0, a1; uint16_t sr; };
SoundExecResult execute_sound_instruction(f3rt::Machine &m, std::initializer_list<uint16_t> instruction, uint32_t source = 0, uint32_t d0 = 0) {
    m.audio->set_reset(true);
    m.audio->write32(0, 0xff00);
    m.audio->write32(4, SND_CODE_BASE);
    m.audio->write32(SND_EXEC_SOURCE, source << 16);
    uint32_t pc = SND_CODE_BASE;
    const auto emit = [&](uint16_t word) { m.audio->write16(pc, word); pc += 2; };
    for (uint16_t word : {uint16_t(0x203c), uint16_t(d0 >> 16), uint16_t(d0),
                          uint16_t(0x223c), uint16_t(source >> 16), uint16_t(source),
                          uint16_t(0x227c), uint16_t(0), lo16(SND_EXEC_SOURCE), uint16_t(0x46fc), uint16_t(0x271b)})
        emit(word);
    for (auto word : instruction) emit(word);
    for (uint16_t word : {uint16_t(0x40f8), lo16(SND_RESULT_SR), uint16_t(0x21c0), lo16(SND_RESULT_D0), uint16_t(0x21c9), lo16(SND_RESULT_A1)}) emit(word);
    m.audio->set_reset(false);
    for (int i = 0; i < 5; ++i) m.interpreter->run_audio(1);
    const int cycles = m.interpreter->run_audio(1);
    for (int i = 0; i < 3; ++i) m.interpreter->run_audio(1);
    const SoundExecResult result{cycles, m.audio->read32(SND_RESULT_D0), m.audio->read32(SND_RESULT_A1), m.audio->read16(SND_RESULT_SR)};
    m.audio->set_reset(true);
    return result;
}

class AudioMachineTest : public ::testing::Test {
protected:
    std::unique_ptr<f3rt::Machine> m;

    void SetUp() override {
        m = std::make_unique<f3rt::Machine>(fixture());
    }
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

TEST(Audio, MailboxWidthWrites) {
    for (unsigned width : {1, 2, 4}) {
        auto m = make_sound_machine();
        m->audio->set_reset(false);
        m->cpu.cycles = 1024;
        if (width == 1) f3_write8(&m->cpu, f3rt::SHARED_BASE, 0x22);
        else if (width == 2) f3_write16(&m->cpu, f3rt::SHARED_BASE, 0x2233);
        else f3_write32(&m->cpu, f3rt::SHARED_BASE - 1, 0x99223344); // Unaligned access enters shared RAM.
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
        const uint32_t value = width == 1 ? f3_read8(&m->cpu, f3rt::SHARED_BASE + 1) :
            width == 2 ? f3_read16(&m->cpu, f3rt::SHARED_BASE) : f3_read32(&m->cpu, f3rt::SHARED_BASE - 1);
        EXPECT_EQ(value, (width == 1 ? 0x5au : width == 2 ? 0x115au : 0xff115a00u))
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
        EXPECT_TRUE(m->audio->is_reset() && m->audio->read8(SND_WORK_BYTE) == 0x11 && m->shared[1] == 0x5a)
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
        const uint16_t foreground[] = {0x7000, 0x227c, 0, lo16(SND_IRQ_RECORDS), 0x46fc, 0x2000, 0x5280, 0x60fc};
        const uint16_t handler[] = {0x22c0, 0x1239, hi16(duart_reg(15)), lo16(duart_reg(15)), 0x4e73};
        for (unsigned i = 0; i < std::size(foreground); ++i) m->audio->write16(SND_CODE_BASE + 2 * i, foreground[i]);
        for (unsigned i = 0; i < std::size(handler); ++i) m->audio->write16(SND_HANDLER_BASE + 2 * i, handler[i]);
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
        EXPECT_EQ(run(quantum), single_clock)
            << "Sound IRQ recognition and CPU state are independent of main-block time partitioning";
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
            duart.write(base, 0x0f); // 8N2
            duart.write(base + 1, 0xee); // F3 external clock / 16
            duart.write(5, mask);
            duart.write(base + 2, 4);
        };
        EXPECT_EQ(duart.read(base + 1) & 0x0c, 0) << "Reset disables the UART transmitter";
        configure();
        EXPECT_TRUE((duart.read(base + 1) & 0x0c) == 0x0c && irq)
            << "Enabling an idle transmitter asserts ready/empty and its IRQ";
        duart.write(base + 3, 0x80);
        EXPECT_TRUE((duart.read(base + 1) & 0x0c) == 0 && !irq) << "THR write clears ready and empty";
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
        EXPECT_NE(duart.read(base + 1) & 8, 0)
            << "Post-reset frame uses a full first bit period";
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
    EXPECT_EQ(counter_clock.read(9) & 8, 0)
        << "Counter-derived TX empty waits for the full frame";
    counter_clock.advance(1);
    EXPECT_EQ(counter_clock.read(9) & 0x0c, 0x0c)
        << "Counter-derived 8N2 frame completes at its eleventh serial edge";
}

TEST_F(AudioMachineTest, SoundInstructionCycleCharges) {
    const auto execute = [&](std::initializer_list<uint16_t> instruction, uint32_t source = 0, uint32_t d0 = 0) {
        return execute_sound_instruction(*m, instruction, source, d0);
    };

    const auto quick = execute({0x5049});
    EXPECT_TRUE(quick.cycles == 8 && quick.a1 == 0x4008 && quick.sr == 0x271b)
        << "68000 ADDQ.W to an address register takes eight cycles and preserves flags";
    EXPECT_EQ(execute({0x544f}).cycles, 8) << "68000 ADDQ.W stack adjustment has the same full cost";
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
    EXPECT_TRUE(tas.cycles == 14 && tas.sr == 0x2714 && m->audio->read8(SND_EXEC_SOURCE) == 0x80)
        << "68000 memory TAS charges its read-modify-write once and reports the original byte";
    EXPECT_TRUE(execute({0x4ac0}).cycles == 4 && execute({0x4ad9}).cycles == 14 &&
                execute({0x4ae1}).cycles == 16 && execute({0x4ae9, 16}).cycles == 18 &&
                execute({0x4af8, SND_EXEC_SOURCE}).cycles == 18 && execute({0x4af9, 0, SND_EXEC_SOURCE}).cycles == 22)
        << "68000 TAS retains register and effective-address timing distinctions";

    for (unsigned kind = 1; kind <= 3; ++kind)
        for (unsigned bit : {0, 2, 15, 16, 31, 32, 47, 48, 63}) {
            const int cycles = (kind == 2 ? 8 : 6) + ((bit & 31) >= 16 ? 2 : 0);
            const uint32_t result = kind == 2 ? 0 : 1u << (bit & 31);
            const auto reg = execute({uint16_t(0x0300 | (kind << 6))}, bit);
            const auto immediate = execute({uint16_t(0x0800 | (kind << 6)), uint16_t(bit)});
            EXPECT_TRUE(reg.cycles == cycles && immediate.cycles == cycles + 4)
                << "68000 register bit mutations distinguish low/high halves after modulo-32 selection";
            EXPECT_TRUE(reg.d0 == result && immediate.d0 == result && reg.sr == 0x271f && immediate.sr == 0x271f)
                << "Bit timing preserves result, old-bit Z and untouched X/N/V/C flags";
        }
    const struct { uint32_t dividend; uint16_t divisor; int cycles; } divisions[] = {
        {0, 1, 136}, {0xffff, 1, 106}, {0x10000, 1, 10}, {0x10000, 2, 134},
        {0xff1234, 0x100, 116}, {0x80000000, 0xffff, 132}, {0xfffeffff, 0xffff, 76}
    };
    for (const auto &test : divisions) {
        const auto reg = execute({0x80c1}, test.divisor, test.dividend);
        const auto immediate = execute({0x80fc, test.divisor}, test.divisor, test.dividend);
        const auto memory = execute({0x80d1}, test.divisor, test.dividend);
        EXPECT_TRUE(reg.cycles == test.cycles && immediate.cycles == test.cycles + 4 && memory.cycles == test.cycles + 4)
            << "68000 DIVU.W timing follows the operand-dependent subtract stages and EA cost";
        const auto quotient = test.dividend / test.divisor;
        const bool overflow = quotient > 0xffff;
        const auto result = overflow ? test.dividend : ((test.dividend % test.divisor) << 16) | quotient;
        const uint16_t flags = 0x10 | (overflow ? 2 : ((quotient == 0 ? 4 : 0) | (quotient & 0x8000 ? 8 : 0)));
        const uint16_t mask = overflow ? 0x13 : 0x1f; // N/Z are undefined on overflow.
        EXPECT_TRUE(reg.d0 == result && immediate.d0 == result && memory.d0 == result &&
                    (reg.sr & mask) == flags && (immediate.sr & mask) == flags && (memory.sr & mask) == flags)
            << "DIVU timing preserves packed remainder/quotient, overflow destination and defined flags";
    }
    const struct { uint16_t source; int cycles; } multiplications[] = {
        {0, 38}, {1, 42}, {0x12, 46}, {0x5555, 70}, {0x7fff, 42}, {0x8000, 40}, {0xffff, 40}, {0xaaaa, 68}
    };
    for (const auto &test : multiplications) {
        const auto reg = execute({0xc1c1}, test.source, 0xfffffffd);
        const auto immediate = execute({0xc1fc, test.source}, test.source, 0xfffffffd);
        const auto memory = execute({0xc1d1}, test.source, 0xfffffffd);
        EXPECT_TRUE(reg.cycles == test.cycles && immediate.cycles == test.cycles + 4 && memory.cycles == test.cycles + 4)
            << "68000 MULS.W charges every Booth transition including the positive source's final transition";
        const int32_t product = int32_t(int16_t(test.source)) * -3;
        const uint16_t sr = 0x2710 | (product == 0 ? 4 : product < 0 ? 8 : 0);
        EXPECT_TRUE(reg.d0 == uint32_t(product) && immediate.d0 == uint32_t(product) && memory.d0 == uint32_t(product) &&
                    reg.sr == sr && immediate.sr == sr && memory.sr == sr)
            << "Signed multiply preserves the full product and X while replacing N/Z/V/C";
    }
}

TEST_F(AudioMachineTest, SoundVectoredIrqWake) {
    m->audio->set_cpu_runner({});
    for (unsigned vector : {15, 30, 64, 255}) {
        m->audio->reset_board();
        m->audio->write32(0, 0xff00);
        m->audio->write32(4, SND_CODE_BASE);
        m->audio->write32(vector * 4, SND_HANDLER_BASE);
        m->audio->write16(SND_CODE_BASE, 0x4e72);
        m->audio->write16(SND_CODE_BASE + 2, 0x2000);
        m->audio->write16(SND_HANDLER_BASE, 0x4e71);
        m->audio->set_reset(false);
        m->interpreter->run_audio(1);
        m->interpreter->run_audio(1);
        m->audio->write8(duart_reg(12), vector);
        m->audio->write8(duart_reg(6), 0);
        m->audio->write8(duart_reg(7), 1);
        m->audio->write8(duart_reg(5), 8);
        m->audio->write8(duart_reg(4), 0x60);
        m->audio->advance(8);
        EXPECT_EQ(m->audio->irq_level(), 6) << "DUART timer wakes the stopped sound CPU on IRQ6";
        EXPECT_TRUE(m->interpreter->run_audio(1) == 48 && m->interpreter->sound_pc() == SND_HANDLER_BASE + 2)
            << "68000 IRQ entry costs 44 cycles independently of vector number, plus the handler NOP";
        EXPECT_TRUE(m->audio->read16(SND_STACK_SR) == 0x2000 && m->audio->read32(SND_STACK_PC) == SND_CODE_BASE + 4)
            << "Vectored IRQ entry preserves the stopped CPU's SR and return PC";
    }
    m->audio->reset_board();
    m->audio->set_cpu_runner([this](int cycles) { return m->interpreter->run_audio(cycles); });
}

TEST_F(AudioMachineTest, SoundStopUnmaskBudget) {
    m->audio->set_cpu_runner({});
    for (int budget : {1, 2, 3, 4, 8, 16, 32, 47, 48, 52}) {
        m->audio->reset_board();
        m->audio->write32(0, 0xff00);
        m->audio->write32(4, SND_CODE_BASE);
        m->audio->write32(SND_IRQ_VECTOR_64, SND_HANDLER_BASE);
        m->audio->write16(SND_CODE_BASE, 0x4e72);
        m->audio->write16(SND_CODE_BASE + 2, 0x2000);
        m->audio->write16(SND_HANDLER_BASE, 0x4e71);
        m->audio->set_reset(false);
        m->interpreter->run_audio(1);
        m->audio->write8(duart_reg(12), 64);
        m->audio->write8(duart_reg(5), 1);
        m->audio->write8(duart_reg(2), 4);
        EXPECT_EQ(m->audio->irq_level(), 6) << "TX-ready IRQ is pending while reset SR masks interrupts";
        EXPECT_TRUE(m->interpreter->run_audio(budget) == 52 && m->interpreter->sound_pc() == SND_HANDLER_BASE)
            << "STOP unmasking an IRQ retains four instruction clocks, a four-clock poll and 44 entry clocks";
        EXPECT_TRUE(m->audio->read16(SND_STACK_SR) == 0x2000 && m->audio->read32(SND_STACK_PC) == SND_CODE_BASE + 4 &&
                    m->interpreter->run_audio(1) == 4 && m->interpreter->sound_pc() == SND_HANDLER_BASE + 2)
            << "Immediate STOP wake stacks the next PC and resumes the handler without remaining stopped";
    }
    m->audio->reset_board();
    m->audio->set_cpu_runner([this](int cycles) { return m->interpreter->run_audio(cycles); });
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
            const unsigned expected = end < 0 ? (halted ? 200 : 201) : end == 0 ? (halted ? 1 : 161) : unsigned(end + 1);
            EXPECT_TRUE(state.pc == expected && state.state == (end < 0 ? 0 : 1) && state.halt_asserted)
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
    audio.write16(otis_reg(8), 0xff00);
    audio.write16(otis_reg(9), 0xff00);
    audio.write16(otis_reg(0), 0x0c00); // Constant sample, all poles lowpass, auxiliary pair.
    std::array<int16_t, 2> pcm{};
    const auto sample = [&] {
        audio.advance(538);
        EXPECT_EQ(audio.render(pcm.data(), 1), 1) << "One complete audio sample is available";
    };
    sample();
    EXPECT_TRUE(pcm[0] == 20925 && pcm[1] == 20925) << "Board gain and signed PCM normalization";
    audio.write8(f3rt::SOUND_VOLUME_BASE, 2);
    audio.write8(f3rt::SOUND_VOLUME_BASE + 2, 0);
    sample();
    EXPECT_TRUE(pcm[0] == 0 && pcm[1] == 20925) << "Left volume mute preserves the right channel";
    audio.write8(f3rt::SOUND_VOLUME_BASE, 7);
    audio.write8(f3rt::SOUND_VOLUME_BASE + 2, 0x33);
    sample();
    EXPECT_TRUE(pcm[0] == 0 && pcm[1] == 5231) << "Minus-six-dB control applies both baseline gain stages";
    audio.set_gain_model(f3rt::Audio::GainModel::SingleStage);
    sample();
    EXPECT_TRUE(pcm[0] == 0 && pcm[1] == 1073) << "Single-stage gain is distinct and preserves channel mute";
    audio.set_reset(false);
    audio.set_reset(true);
    sample();
    EXPECT_TRUE(pcm[0] == 0 && pcm[1] == 1073) << "CPU-line reset preserves attenuation and playing OTIS voices";
    audio.reset_board();
    sample();
    EXPECT_TRUE(pcm[0] == 2142 && pcm[1] == 2142) << "Board reset restores volume without resetting OTIS voices";
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
    m->write8(f3rt::SHARED_BASE + 0x10, 0x75);
    EXPECT_EQ(m->audio->read16(f3rt::SOUND_SHARED_BASE + 0x20), 0x75ff) << "DPRAM sound high-byte lane";
    m->audio->write16(f3rt::SOUND_SHARED_BASE + 0x20, 0xaabb);
    EXPECT_EQ(m->read8(f3rt::SHARED_BASE + 0x10), 0xaa) << "DPRAM reverse lane";
    m->audio->write16(f3rt::OTIS_PAGE, 0); // Voice 0, register page
    m->audio->write16(otis_reg(10), 0x0246);
    m->audio->write16(otis_reg(11), 0x8a00);
    m->audio->write16(f3rt::OTIS_PAGE, 0x20); // Stopped voice sample-ROM readback
    EXPECT_EQ(m->audio->read16(otis_reg(6)), 0x4567)
        << "OTIS stopped voice reads full 20-bit sample address";
    m->audio->write16(SND_WORK_BYTE, 0xa55a);
    m->audio->set_reset(false);
    m->audio->set_reset(true);
    m->audio->set_reset(false);
    EXPECT_EQ(m->audio->read16(SND_WORK_BYTE), 0xa55a) << "Sound CPU RESET preserves board work RAM";
}

TEST_F(AudioMachineTest, CpuLineResetPreservesDspAndDuart) {
    m->audio->set_reset(true);
    m->audio->write8(duart_reg(12), 0x66);
    m->audio->write8(f3rt::SOUND_ES5510_BASE + 1, 0x12);
    m->audio->write8(f3rt::SOUND_ES5510_BASE + 3, 0x34);
    m->audio->write8(f3rt::SOUND_ES5510_BASE + 5, 0x56);
    m->audio->write8(f3rt::SOUND_ES5510_BASE + 0x141, 0);
    m->audio->set_reset(false);
    m->audio->set_reset(true);
    m->audio->write8(f3rt::SOUND_ES5510_BASE + 0x101, 0);
    EXPECT_TRUE(m->audio->read8(f3rt::SOUND_ES5510_BASE + 1) == 0x12 && m->audio->read8(duart_reg(12)) == 0x66)
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
    const auto result = execute_sound_instruction(*mach, {0xc1c1}, uint16_t(op2), uint32_t(int32_t(op1)));

    const int32_t expected_product = int32_t(op1) * int32_t(op2);
    RC_ASSERT(result.d0 == uint32_t(expected_product));

    // 68000 condition codes: X preserved (bit 4, setup sets 0x271b so X=1), N if negative, Z if zero, V=0, C=0.
    const uint16_t expected_flags = 0x10 | (expected_product == 0 ? 4 : (expected_product < 0 ? 8 : 0));
    RC_ASSERT((result.sr & 0x1f) == expected_flags);
}

// Behaviour test: verify hardware OTIS->ESP PCB wiring in Effects mixer
// (docs/notes/raw/docs/otis-esp-dac-routing-2026-10-09.txt, 12's 5505-5510.txt note):
// - Pair 3 (buses 6/7) -> dry output with no echo/tail
// - Pair 1 (buses 2/3) -> reverb path (diffused tail after input stops)
// - Pair 2 (buses 4/5) -> ~274.8 ms cross-delay echo
// - Pair 0 (buses 0/1) -> silent (physically unconnected on board)
TEST(Audio, EnhancedEffectsRoutingMatchesHardwareWiring) {
    std::vector<uint8_t> sound_rom(0x20000, 0);
    for (int i = 0; i < 128; ++i) {
        const uint16_t val = uint16_t(std::round(i / 127.0 * 32767.0));
        sound_rom[f3rt::sound_5a7e9117::VOLUME_PAN_TABLE_OFFSET + i * 2] = val >> 8;
        sound_rom[f3rt::sound_5a7e9117::VOLUME_PAN_TABLE_OFFSET + i * 2 + 1] = val & 0xff;
    }
    const uint8_t params[10] = {30, 58, 73, 32, 37, 0, 40, 43, 18, 32};
    for (int n = 0; n < 10; ++n) {
        sound_rom[f3rt::sound_5a7e9117::PRESET_TABLE_OFFSET + 11 * 16 + 15 - n] = params[n];
    }

    const std::array<float, 2> in_gain{1.0f, 1.0f};
    const std::array<float, 2> out_gain{1.0f, 1.0f};

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
        fx.mix(buses.data(), stereo.data(), N, in_gain, out_gain);
        float peak = 0.0f;
        for (float s : stereo) peak = std::max(peak, std::abs(s));
        EXPECT_EQ(peak, 0.0f) << "OTIS pair 0 must produce silence (unconnected on PCB)";
    }

    // 2. OTIS Pair 3 (buses 6/7) reaches dry output with no echo or tail
    {
        f3rt::hle::Effects fx(sound_rom);
        fx.select(11);
        constexpr size_t N = 1000;
        std::vector<float> buses(8 * N, 0.0f);
        std::vector<float> stereo(2 * N, 0.0f);
        buses[0 * 8 + 6] = 1.0f;
        buses[0 * 8 + 7] = 0.5f;
        fx.mix(buses.data(), stereo.data(), N, in_gain, out_gain);
        EXPECT_GT(stereo[0], 0.0f) << "OTIS pair 3 must reach dry output";
        EXPECT_GT(stereo[1], 0.0f) << "OTIS pair 3 must reach dry output";
        float subsequent_peak = 0.0f;
        for (size_t f = 1; f < N; ++f) {
            subsequent_peak = std::max({subsequent_peak, std::abs(stereo[f * 2]), std::abs(stereo[f * 2 + 1])});
        }
        EXPECT_EQ(subsequent_peak, 0.0f) << "OTIS pair 3 must have no echo or reverb tail";
    }

    // 3. OTIS Pair 1 (buses 2/3) reaches the reverb path (diffused tail after input stops)
    {
        f3rt::hle::Effects fx(sound_rom);
        fx.select(11);
        constexpr size_t N = 2000;
        std::vector<float> buses(8 * N, 0.0f);
        std::vector<float> stereo(2 * N, 0.0f);
        buses[0 * 8 + 2] = 1.0f;
        fx.mix(buses.data(), stereo.data(), N, in_gain, out_gain);
        float tail_energy = 0.0f;
        for (size_t f = 100; f < N; ++f) {
            tail_energy += std::abs(stereo[f * 2]) + std::abs(stereo[f * 2 + 1]);
        }
        EXPECT_GT(tail_energy, 0.01f) << "OTIS pair 1 must produce a diffused reverb tail";
    }

    // 4. OTIS Pair 2 (buses 4/5) produces ~274.8 ms cross-delay echo
    {
        f3rt::hle::Effects fx(sound_rom);
        fx.select(11);
        constexpr size_t N = 20000; // ~416 ms at 48 kHz
        std::vector<float> buses(8 * N, 0.0f);
        std::vector<float> stereo(2 * N, 0.0f);
        buses[0 * 8 + 4] = 1.0f;
        fx.mix(buses.data(), stereo.data(), N, in_gain, out_gain);
        size_t echo_peak_frame = 0;
        float echo_peak = 0.0f;
        for (size_t f = 1000; f < N; ++f) {
            if (std::abs(stereo[f * 2]) > echo_peak) {
                echo_peak = std::abs(stereo[f * 2]);
                echo_peak_frame = f;
            }
        }
        EXPECT_GT(echo_peak, 0.05f) << "OTIS pair 2 must produce a delay echo";
        const double echo_ms = double(echo_peak_frame) / 48000.0 * 1000.0;
        EXPECT_NEAR(echo_ms, 274.8, 1.0) << "OTIS pair 2 echo must occur at ~274.8 ms";
    }
}


