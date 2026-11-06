#include "f3rt/memory_map.hpp"
#include "f3rt/bytes.hpp"
#include "audio/reference/board.hpp"
#include "audio/hle/effects.hpp"
#include "audio/hle/clock.hpp"
#include "audio/hle/events.hpp"
#include "audio/hle/note_timing.hpp"
#include "audio/hle/mailbox.hpp"
#include "audio/hle/patch.hpp"
#include "audio/hle/pulse_timer.hpp"
#include "audio/hle/voice_filter.hpp"
#include "audio/hle/voice_render.hpp"
#include "audio/pcm_ring.hpp"
#include "audio/shared_mailbox.hpp"
#include "audio/snapshot.hpp"
#include "audio/worker_queue.hpp"
#include "audio/hle/sound_rom_5a7e9117.hpp"
#include "third_party/audio/mc68681.hpp"
#include "third_party/audio/es5510.hpp"
#include "state_io.hpp"
#include "frontend/audio_pacer.hpp"
#include "support.hpp"
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <future>
#include <initializer_list>
#include <latch>
#include <memory>
#include <random>
#include <span>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
constexpr uint32_t SND_WORK_BYTE = 0x600;
// DUART register n on the sound bus (odd byte lane).
constexpr uint32_t duart_reg(uint32_t reg) { return f3rt::SOUND_DUART_BASE + 2 * reg + 1; }
// ES5505 (OTIS) register n on the sound bus (16-bit, even byte lane).
constexpr uint32_t otis_reg(uint32_t reg) { return f3rt::SOUND_ES5505_BASE + 2 * reg; }
f3rt::reference::Board make_board(std::span<const uint8_t> sample_rom = {}, uint8_t *shared = nullptr) {
    return f3rt::reference::Board({}, sample_rom, shared);
}
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
    const std::array<uint8_t, 4> rom{0x40, 0, 0x40, 0};
    auto audio = make_board(rom);
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
    sample();
    EXPECT_TRUE(pcm[0] == 2136 && pcm[1] == 2136)
        << "Default gain matches post-DAC analog attenuation";
    audio.write8(f3rt::SOUND_VOLUME_BASE, 2);
    audio.write8(f3rt::SOUND_VOLUME_BASE + 2, 0);
    sample();
    EXPECT_TRUE(pcm[0] == 2136 && pcm[1] == 0)
        << "Channel-0 volume mute follows its data through the DAC swap and preserves channel 1";
    audio.write8(f3rt::SOUND_VOLUME_BASE, 7);
    audio.write8(f3rt::SOUND_VOLUME_BASE + 2, 0x33);
    sample();
    EXPECT_TRUE(pcm[0] == 1070 && pcm[1] == 0)
        << "Minus-six-dB control attenuates once and preserves channel mute";
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
    auto clock_audio = make_board();
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

TEST(Audio, DpramOtisAndSoundReset) {
    std::array<uint8_t, 0x800> shared{};
    std::vector<uint8_t> samples(0x2468c);
    samples[0x2468a] = 0x45;
    samples[0x2468b] = 0x67;
    auto audio = make_board(samples, shared.data());
    shared[0x10] = 0x75;
    EXPECT_EQ(audio.read16(f3rt::SOUND_SHARED_BASE + 0x20), 0x75ff) << "DPRAM sound high-byte lane";
    audio.write16(f3rt::SOUND_SHARED_BASE + 0x20, 0xaabb);
    EXPECT_EQ(shared[0x10], 0xaa) << "DPRAM reverse lane";
    audio.write16(f3rt::OTIS_PAGE, 0);
    audio.write16(otis_reg(10), 0x0246);
    audio.write16(otis_reg(11), 0x8a00);
    audio.write16(f3rt::OTIS_PAGE, 0x20);
    EXPECT_EQ(audio.read16(otis_reg(6)), 0x4567) << "OTIS stopped voice reads full 20-bit sample address";
    audio.write16(SND_WORK_BYTE, 0xa55a);
    audio.set_reset(false);
    audio.set_reset(true);
    audio.set_reset(false);
    EXPECT_EQ(audio.read16(SND_WORK_BYTE), 0xa55a) << "Sound CPU RESET preserves board work RAM";
}

TEST(Audio, CpuLineResetPreservesDspAndDuart) {
    auto audio = make_board();
    audio.set_reset(true);
    audio.write8(duart_reg(12), 0x66);
    audio.write8(f3rt::SOUND_ES5510_BASE + 1, 0x12);
    audio.write8(f3rt::SOUND_ES5510_BASE + 3, 0x34);
    audio.write8(f3rt::SOUND_ES5510_BASE + 5, 0x56);
    audio.write8(f3rt::SOUND_ES5510_BASE + 0x141, 0);
    audio.set_reset(false);
    audio.set_reset(true);
    audio.write8(f3rt::SOUND_ES5510_BASE + 0x101, 0);
    EXPECT_TRUE(audio.read8(f3rt::SOUND_ES5510_BASE + 1) == 0x12 && audio.read8(duart_reg(12)) == 0x66)
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
    fx.mix(f3rt::hle::BusSpan(buses), f3rt::hle::MutableStereoSpan(stereo), {1.0f, 1.0f}, {1.0f, 1.0f});
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
        fx.mix(f3rt::hle::BusSpan(buses), f3rt::hle::MutableStereoSpan(stereo), {1.0f, 1.0f}, {1.0f, 1.0f});
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
        fx.mix(f3rt::hle::BusSpan(buses), f3rt::hle::MutableStereoSpan(stereo), {1.0f, 1.0f}, {1.0f, 1.0f});
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
        fx.mix(f3rt::hle::BusSpan(buses), f3rt::hle::MutableStereoSpan(stereo), {1.0f, 1.0f}, {1.0f, 1.0f});
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
    fx.mix(f3rt::hle::BusSpan(buses), f3rt::hle::MutableStereoSpan(stereo), {1.0f, 1.0f}, {1.0f, 1.0f});
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

// The ES5510 executes its whole program on every sample frame whatever the input is, so the phase of its LFOs
// keeps advancing through silence. The auto-bypass (silent sends and nothing audible left in the program) must
// therefore be invisible: after the sends resume, a run that bypassed produces the same output as a run that
// never did (here kept awake by a sub-audible send). The chorus program has all three LFOs; with p0 = 0 and no
// echo feedback its tail (the nested all-pass blocks ring down at about -30 dB per half second) is below the
// silence threshold after about 5 s, so the quiet period is 8 s.
TEST(Audio, EffectsAutoBypassKeepsLfoPhaseRunning) {
    const uint8_t params[10] = {0, 0, 40, 26, 37, 0, 0x68, 64, 32, 39};
    const auto sound_rom = effects_test_rom(5, 5, params);
    f3rt::hle::Effects bypassing(sound_rom), awake(sound_rom);
    bypassing.select(5);
    awake.select(5);

    constexpr size_t BLOCK = 4000, SILENT_BLOCKS = 60, BURST = 3000, TAIL = 20000;
    const auto run = [&](f3rt::hle::Effects &fx, float whisper) {
        std::vector<float> out;
        const auto render = [&](const std::vector<float> &buses) {
            std::vector<float> stereo(buses.size() / 4);
            fx.mix(f3rt::hle::BusSpan(buses), f3rt::hle::MutableStereoSpan(stereo), {1.0f, 1.0f}, {1.0f, 1.0f});
            out.insert(out.end(), stereo.begin(), stereo.end());
        };
        std::vector<float> buses(8 * BLOCK, 0.0f);
        for (size_t bus = 2; bus < 8; ++bus) buses[bus] = 0.5f; // an impulse on every input
        render(buses);
        std::fill(buses.begin(), buses.end(), 0.0f);
        // The quiet period: a send of 1e-9 on every frame keeps the effect awake; zero lets it bypass.
        for (size_t f = 0; f < BLOCK; ++f) buses[f * 8 + 4] = buses[f * 8 + 6] = whisper;
        for (size_t block = 0; block < SILENT_BLOCKS; ++block) render(buses);
        const size_t resumed = out.size() / 2;
        const bool slept = fx.bypassed();
        // After the quiet period: a burst of noise on every input, then its tail.
        uint32_t seed = 12345;
        std::vector<float> burst(8 * (BURST + TAIL), 0.0f);
        for (size_t f = 0; f < BURST; ++f)
            for (size_t bus = 2; bus < 8; ++bus) {
                seed = seed * 1664525u + 1013904223u;
                burst[f * 8 + bus] = 0.4f * (float(seed >> 8) / float(1 << 24) - 0.5f);
            }
        render(burst);
        return std::tuple{out, resumed, slept};
    };
    const auto [bypassed_out, resumed, bypassed_slept] = run(bypassing, 0.0f);
    const auto [awake_out, resumed_awake, awake_slept] = run(awake, 1e-9f);
    ASSERT_EQ(resumed, resumed_awake);
    ASSERT_TRUE(bypassed_slept) << "the quiet period must have ended in an auto-bypass";
    ASSERT_FALSE(awake_slept);

    float peak = 0.0f, worst = 0.0f;
    for (size_t i = resumed * 2; i < awake_out.size(); ++i) {
        peak = std::max(peak, std::abs(awake_out[i]));
        worst = std::max(worst, std::abs(awake_out[i] - bypassed_out[i]));
    }
    ASSERT_GT(peak, 0.1f);
    EXPECT_LT(worst, peak * 1e-6f) << "output after an auto-bypass must match a run that never bypassed";
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

// The clock and the pulse timer are the only schedulers: one 1 kHz grid feeds the voice ring, the sequence tempo
// pulse and the sequence fade pulse.
namespace {
using f3rt::hle::Clock;
using f3rt::hle::PulseTimer;
using f3rt::hle::TIMER_TICKS;

struct TickRecorder final : Clock::Listener {
    std::vector<uint64_t> ticks;
    void clock_tick(uint64_t tick) override { ticks.push_back(tick); }
};

struct PulseRecorder final : PulseTimer::Listener {
    std::vector<uint64_t> ticks;
    std::vector<uint8_t> tags;
    void pulse(uint64_t tick, uint8_t tag) override {
        ticks.push_back(tick);
        tags.push_back(tag);
    }
};
} // namespace

TEST(HleServices, ClockTicksEveryMillisecond) {
    EXPECT_EQ(TIMER_TICKS, 16000u);
    TickRecorder recorder;
    Clock clock;
    clock.add(recorder);
    EXPECT_EQ(clock.next_tick(), uint64_t(TIMER_TICKS));
    for (unsigned i = 0; i < 3; ++i) clock.advance_one();
    EXPECT_EQ(recorder.ticks, (std::vector<uint64_t>{16000, 32000, 48000}));
    EXPECT_EQ(clock.next_tick(), uint64_t(64000));
    clock.reset(100000);
    clock.advance_one();
    EXPECT_EQ(recorder.ticks.back(), 112000u);
}

TEST(HleServices, PulseTimerListeners) {
    PulseTimer beat{625, 120};
    PulseRecorder first, second;
    beat.add(first, 3);
    beat.add(second, 7);
    for (unsigned i = 0; i < 12; ++i) beat.clock_tick(uint64_t(i + 1) * TIMER_TICKS);
    EXPECT_EQ(first.ticks, (std::vector<uint64_t>{96000, 176000}));
    EXPECT_EQ(first.tags, (std::vector<uint8_t>{3, 3}));
    EXPECT_EQ(second.ticks, first.ticks) << "every listener of a timer fires";
    EXPECT_EQ(second.tags, (std::vector<uint8_t>{7, 7}));

    PulseTimer fade{12, 1};
    PulseRecorder pulses;
    fade.add(pulses, 0xff);
    for (unsigned i = 0; i < 24; ++i) fade.clock_tick(uint64_t(i + 1) * TIMER_TICKS);
    EXPECT_EQ(pulses.ticks, (std::vector<uint64_t>{192000, 384000}));

    fade.set_step(0);
    for (unsigned i = 0; i < 24; ++i) fade.clock_tick(uint64_t(25 + i) * TIMER_TICKS);
    EXPECT_EQ(pulses.ticks.size(), 2u) << "a stopped timer does not pulse";
}

TEST(HleServices, MailboxPreservesEventOrderAndRejectsOverflow) {
    using f3rt::hle::Event;
    using f3rt::hle::Mailbox;
    using f3rt::hle::NoteOffEvent;
    using f3rt::hle::NoteOnEvent;
    using f3rt::hle::TrackEvent;
    using f3rt::hle::TrackEventKind;

    Mailbox mailbox;
    EXPECT_TRUE(mailbox.empty());
    NoteOnEvent on;
    on.instance = 11;
    on.tick = 22;
    on.program = 33;
    NoteOffEvent off;
    off.instance = 44;
    TrackEvent volume;
    volume.kind = TrackEventKind::Volume;
    volume.value = 55;
    mailbox.push(on);
    mailbox.push(off);
    mailbox.push(volume);

    std::vector<int> order;
    mailbox.drain([&](const Event &event) {
        if (const auto *n = std::get_if<NoteOnEvent>(&event)) {
            EXPECT_EQ(n->instance, 11u);
            EXPECT_EQ(n->tick, 22u);
            EXPECT_EQ(n->program, 33u);
            order.push_back(1);
        } else if (const auto *o = std::get_if<NoteOffEvent>(&event)) {
            EXPECT_EQ(o->instance, 44u);
            order.push_back(2);
        } else if (const auto *t = std::get_if<TrackEvent>(&event)) {
            EXPECT_EQ(t->kind, TrackEventKind::Volume);
            EXPECT_EQ(t->value, 55u);
            order.push_back(3);
        } else {
            ADD_FAILURE() << "unexpected event alternative";
            order.push_back(0);
        }
    });
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3})) << "drain is FIFO";
    EXPECT_TRUE(mailbox.empty());

    mailbox.clear();
    EXPECT_TRUE(mailbox.empty());
    for (size_t i = 0; i < Mailbox::CAPACITY; ++i) mailbox.push(off);
    EXPECT_EQ(Mailbox::CAPACITY, 1024u);
    try {
        mailbox.push(off);
        FAIL() << "a full mailbox must reject the push";
    } catch (const std::runtime_error &e) {
        EXPECT_STREQ(e.what(), "HLE mailbox full");
    }
    mailbox.clear();
    EXPECT_NO_THROW(mailbox.push(off));
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

namespace {

// Scalar reference for the voice filter: the four-stage recursion, one frame at a time, in float.
struct VoiceFilterRef {
    float s[4]{}, p[4]{};
    void frame(int mode, float x, float a1, float a2, float hp, float gl, float gr, float &L,
               float &R) {
        float y0 = a1 * (x - s[0]) + s[0];
        p[0] = x;
        s[0] = y0;
        x = y0;
        float y1 = a1 * (x - s[1]) + s[1];
        p[1] = x;
        s[1] = y1;
        x = y1;
        float as2 = mode == 2 ? a2 : a1;
        float lp2 = as2 * (x - s[2]) + s[2];
        float hp2 = hp * s[2] + (x - p[2]);
        float y2 = mode == 0 ? hp2 : lp2;
        p[2] = x;
        s[2] = y2;
        x = y2;
        float lp3 = a2 * (x - s[3]) + s[3];
        float hp3 = hp * s[3] + (x - p[3]);
        float y3 = mode < 2 ? hp3 : lp3;
        p[3] = x;
        s[3] = y3;
        L = y3 * gl;
        R = y3 * gr;
    }
};

float voice_filter_step(f3rt::hle::VoiceFilterState &state, int mode, float x, float a1, float a2, float hp) {
    using namespace f3rt::hle;
    return with_filter_mode(uint8_t(mode), [&](auto m) {
        return filter_step<decltype(m)::value>(state, x, a1, a2, hp);
    });
}

// mode_of(chunk) selecting each block's mode. Returns the largest output error relative to the
// largest output magnitude.
template <class ModeOf>
float voice_filter_error(unsigned seed, int chunks, ModeOf mode_of) {
    constexpr int CH = 64;
    std::mt19937 rng(seed);
    std::uniform_real_distribution<float> coef(0.01f, 0.99f), pole(0.f, 1.f), gain(0.f, 1.f),
        sample(-32768.f, 32767.f);
    f3rt::hle::VoiceFilterState state;
    VoiceFilterRef ref;
    float peak = 0.f, err = 0.f;
    for (int k = 0; k < chunks; ++k) {
        const int mode = mode_of(k);
        for (int r = 0; r < CH; ++r) {
            const float x = sample(rng), a1 = coef(rng), a2 = coef(rng), hp = pole(rng);
            const float gl = gain(rng), gr = gain(rng);
            const float y = voice_filter_step(state, mode, x, a1, a2, hp);
            float L = 0.f, R = 0.f;
            ref.frame(mode, x, a1, a2, hp, gl, gr, L, R);
            peak = std::max({peak, std::fabs(L), std::fabs(R)});
            err = std::max({err, std::fabs(y * gl - L), std::fabs(y * gr - R)});
        }
    }
    return peak > 0.f ? err / peak : err;
}

float voice_filter_dc(int mode) {
    f3rt::hle::VoiceFilterState state;
    float y = 0.f;
    for (int k = 0; k < 2000 * 64; ++k) y = voice_filter_step(state, mode, 1000.f, 0.2f, 0.7f, 0.5f);
    return y;
}

}  // namespace

TEST(Audio, VoiceFilterMatchesScalarRecursion) {
    for (int mode = 0; mode < 4; ++mode) {
        EXPECT_LT(voice_filter_error(1234u + unsigned(mode), 256, [mode](int) { return mode; }),
                  1e-5f)
            << "mode " << mode;
    }
    EXPECT_LT(voice_filter_error(99u, 256, [](int k) { return (k * 7 + k / 3) % 4; }), 1e-5f)
        << "mixed modes";
}

TEST(Audio, VoiceFilterDcGainPerMode) {
    EXPECT_NEAR(voice_filter_dc(0), 0.f, 1e-3f);
    EXPECT_NEAR(voice_filter_dc(1), 0.f, 1e-3f);
    EXPECT_NEAR(voice_filter_dc(2), 1000.f, 1e-2f);
    EXPECT_NEAR(voice_filter_dc(3), 1000.f, 1e-2f);
}

TEST(Audio, DirectVoiceFilterIndexMatchesDriverMath) {
    EXPECT_EQ(f3rt::hle::direct_filter_index(101, 115), 692);
    EXPECT_EQ(f3rt::hle::direct_filter_index(248, 127), 1501);
    EXPECT_EQ(f3rt::hle::direct_filter_index(0, 100), 0);
}

namespace {

uint32_t float_bits(float v) { return std::bit_cast<uint32_t>(v); }
uint64_t double_bits(double v) { return std::bit_cast<uint64_t>(v); }

constexpr size_t ROM_WORDS = 256;

std::vector<uint16_t> test_words() {
    std::vector<uint16_t> words(ROM_WORDS);
    for (size_t i = 0; i < words.size(); ++i) words[i] = uint16_t(i * 37 + 5);
    return words;
}

f3rt::hle::Voice forward_voice() {
    f3rt::hle::Voice voice;
    voice.position = 10;
    voice.start = 0;
    voice.end = 100;
    voice.increment = 1;
    return voice;
}

f3rt::hle::Voice ramping_voice() {
    f3rt::hle::Voice voice = forward_voice();
    voice.left = 0x4010;
    voice.right = 0x5080;
    voice.step_left = 1.0;
    voice.step_right = -1.5;
    voice.target_left = voice.left + 100 * voice.step_left;
    voice.target_right = voice.right + 100 * voice.step_right;
    voice.gain_frames = 100;
    voice.gain_dirty = false;
    f3rt::hle::step_gain(voice);
    return voice;
}

template <unsigned Mode> void expect_filter_matches_filter_step() {
    using namespace f3rt::hle;
    Filter filter;
    filter.a1 = 0.123456789012345;
    filter.a2 = 0.765432109876543;
    filter.high_pole = 0.456789012345678;
    const VoiceFilterState memory{100.25f, -200.5f, 300.75f, -400.125f};
    VoiceFilter<Mode> vf(memory, filter);
    VoiceFilterState reference = memory;
    for (float x : {1234.5f, -4321.25f, 0.f}) {
        const float y = vf.step(x);
        const float expected = filter_step<Mode>(reference, x, float(filter.a1), float(filter.a2),
                                                 float(filter.high_pole));
        EXPECT_EQ(float_bits(y), float_bits(expected)) << "mode " << Mode;
        EXPECT_EQ(float_bits(vf.state.stage1), float_bits(reference.stage1)) << "mode " << Mode;
        EXPECT_EQ(float_bits(vf.state.stage2), float_bits(reference.stage2)) << "mode " << Mode;
        EXPECT_EQ(float_bits(vf.state.stage3), float_bits(reference.stage3)) << "mode " << Mode;
        EXPECT_EQ(float_bits(vf.state.stage4), float_bits(reference.stage4)) << "mode " << Mode;
    }
}

}

TEST(HleVoiceRender, WithFilterModeClampsAboveThree) {
    using namespace f3rt::hle;
    for (unsigned mode = 0; mode < 4; ++mode) {
        const unsigned seen =
            with_filter_mode(uint8_t(mode), [](auto m) -> unsigned { return decltype(m)::value; });
        EXPECT_EQ(seen, mode);
    }
    for (unsigned mode : {4u, 200u, 255u}) {
        const unsigned seen =
            with_filter_mode(uint8_t(mode), [](auto m) -> unsigned { return decltype(m)::value; });
        EXPECT_EQ(seen, 3u) << "mode " << mode;
    }
}

TEST(HleVoiceRender, SampleWordsReadSignedWordsAndMask) {
    using namespace f3rt::hle;
    std::vector<uint16_t> words = test_words();
    words[2] = 100;
    words[3] = 300;
    words[4] = 0x8000;
    words[5] = 0;
    words[ROM_WORDS - 1] = 1000;
    words[0] = 2000;
    words[9] = 777;
    words[19] = 555;
    words[20] = 333;
    const SampleWords samples(words);

    EXPECT_EQ(samples.interpolate(2.25), interpolate_sample(100, 300, 0.25));
    EXPECT_EQ(samples.interpolate(4.0), interpolate_sample(-32768.0, 0.0, 0.0));
    EXPECT_EQ(samples.interpolate(4.5), interpolate_sample(-32768.0, 0.0, 0.5));
    EXPECT_LT(samples.interpolate(4.0), 0.f);

    Voice wrap = forward_voice();
    wrap.loop = true;
    wrap.end = double(ROM_WORDS);
    wrap.position = double(ROM_WORDS - 1) + 0.5;
    EXPECT_EQ(samples.interpolate(wrap), interpolate_sample(1000, 2000, 0.5));

    Voice forward = forward_voice();
    forward.end = 10;
    forward.position = 9.5;
    EXPECT_EQ(samples.interpolate(forward), interpolate_sample(777, 0.0, 0.5));

    Voice reverse = forward_voice();
    reverse.reverse = true;
    reverse.start = 20;
    reverse.position = 19.5;
    EXPECT_EQ(samples.interpolate(reverse), interpolate_sample(0.0, 333, 0.5));

    EXPECT_TRUE(samples.outside(-0.5));
    EXPECT_TRUE(samples.outside(double(ROM_WORDS)));
    EXPECT_FALSE(samples.outside(0.0));
    EXPECT_FALSE(samples.outside(double(ROM_WORDS) - 0.5));
    EXPECT_FALSE(samples.outside(std::nan("")));
}

TEST(HleVoiceRender, SampleWordsRoomStopsBeforeTheWindowEdge) {
    using namespace f3rt::hle;
    const std::vector<uint16_t> words = test_words();
    const SampleWords samples(words);

    Voice forward = forward_voice();
    EXPECT_EQ(samples.room(forward, 1000), 89u);
    EXPECT_EQ(samples.room(forward, 20), 20u);
    forward.increment = 0.5;
    EXPECT_EQ(samples.room(forward, 1000), 179u);
    forward.position = 98;
    forward.increment = 1;
    EXPECT_EQ(samples.room(forward, 1000), 1u);
    forward.position = 99.5;
    EXPECT_EQ(samples.room(forward, 1000), 0u);
    forward.position = 10;
    forward.increment = 0;
    EXPECT_EQ(samples.room(forward, 1000), 0u);
    forward.increment = 1;
    forward.position = double(ROM_WORDS - 1);
    EXPECT_EQ(samples.room(forward, 1000), 0u);
    forward.position = 10;
    forward.end = double(ROM_WORDS - 1);
    EXPECT_EQ(samples.room(forward, 1000), 0u);

    Voice reverse = forward_voice();
    reverse.reverse = true;
    reverse.start = 10;
    reverse.position = 100;
    reverse.increment = 1.5;
    EXPECT_EQ(samples.room(reverse, 1000), 59u);
    EXPECT_EQ(samples.room(reverse, 7), 7u);
    reverse.position = 11;
    EXPECT_EQ(samples.room(reverse, 1000), 0u);
    reverse.position = 100;
    reverse.increment = 0;
    EXPECT_EQ(samples.room(reverse, 1000), 0u);
    reverse.increment = 1.5;
    reverse.position = double(ROM_WORDS - 1);
    EXPECT_EQ(samples.room(reverse, 1000), 0u);
}

TEST(HleVoiceRender, PlanRunPicksSingleConstantOrRamping) {
    using namespace f3rt::hle;
    using Kind = VoiceRun::Kind;
    const std::vector<uint16_t> words = test_words();
    const SampleWords samples(words);

    auto expect_single = [&](const Voice &voice, const char *why) {
        const VoiceRun run = plan_run(voice, 64, samples);
        EXPECT_EQ(run.kind, Kind::Single) << why;
        EXPECT_EQ(run.frames, 1u) << why;
    };

    Voice finished = forward_voice();
    finished.sample_finished = true;
    expect_single(finished, "sample finished");

    Voice filter_ramp = forward_voice();
    filter_ramp.filter.ramp = 5;
    expect_single(filter_ramp, "filter ramp");

    Voice empty_segment = forward_voice();
    empty_segment.gain_frames = 50;
    empty_segment.left_segment.frames = 30;
    empty_segment.right_segment.frames = 0;
    expect_single(empty_segment, "empty right segment");
    empty_segment.left_segment.frames = 0;
    empty_segment.right_segment.frames = 30;
    expect_single(empty_segment, "empty left segment");

    Voice last_step = forward_voice();
    last_step.gain_frames = 1;
    last_step.left_segment.frames = 30;
    last_step.right_segment.frames = 30;
    expect_single(last_step, "last ramp step");

    Voice no_room = forward_voice();
    no_room.increment = 0;
    expect_single(no_room, "no room");

    Voice constant = forward_voice();
    const VoiceRun all = plan_run(constant, 1000, samples);
    EXPECT_EQ(all.kind, Kind::Constant);
    EXPECT_EQ(all.frames, samples.room(constant, 1000));
    const VoiceRun capped = plan_run(constant, 20, samples);
    EXPECT_EQ(capped.kind, Kind::Constant);
    EXPECT_EQ(capped.frames, 20u);

    Voice ramping = forward_voice();
    ramping.gain_frames = 50;
    ramping.left_segment.frames = 30;
    ramping.right_segment.frames = 20;
    VoiceRun run = plan_run(ramping, 40, samples);
    EXPECT_EQ(run.kind, Kind::Ramping);
    EXPECT_EQ(run.frames, 20u);
    ramping.right_segment.frames = 45;
    run = plan_run(ramping, 40, samples);
    EXPECT_EQ(run.kind, Kind::Ramping);
    EXPECT_EQ(run.frames, 30u);
    ramping.left_segment.frames = 60;
    run = plan_run(ramping, 70, samples);
    EXPECT_EQ(run.kind, Kind::Ramping);
    EXPECT_EQ(run.frames, 45u);
    ramping.right_segment.frames = 60;
    run = plan_run(ramping, 70, samples);
    EXPECT_EQ(run.kind, Kind::Ramping);
    EXPECT_EQ(run.frames, 49u);
    run = plan_run(ramping, 10, samples);
    EXPECT_EQ(run.kind, Kind::Ramping);
    EXPECT_EQ(run.frames, 10u);
}

TEST(HleVoiceRender, CurrentGainRefreshesOnlyWhenDirty) {
    using namespace f3rt::hle;
    Voice voice;
    voice.left = 0x4010;
    voice.right = 0x5080;
    voice.left_gain = 1234.f;
    voice.right_gain = 5678.f;
    voice.gain_dirty = false;

    StereoGain clean = current_gain(voice);
    EXPECT_EQ(float_bits(voice.left_gain), float_bits(1234.f));
    EXPECT_EQ(float_bits(voice.right_gain), float_bits(5678.f));
    EXPECT_FALSE(voice.gain_dirty);
    EXPECT_EQ(float_bits(clean.left), float_bits(1234.f * GAIN_SCALE));
    EXPECT_EQ(float_bits(clean.right), float_bits(5678.f * GAIN_SCALE));

    voice.gain_dirty = true;
    StereoGain dirty = current_gain(voice);
    EXPECT_FALSE(voice.gain_dirty);
    EXPECT_EQ(float_bits(voice.left_gain), float_bits(interpolated_gain(0x4010)));
    EXPECT_EQ(float_bits(voice.right_gain), float_bits(interpolated_gain(0x5080)));
    EXPECT_EQ(float_bits(dirty.left), float_bits(voice.left_gain * GAIN_SCALE));
    EXPECT_EQ(float_bits(dirty.right), float_bits(voice.right_gain * GAIN_SCALE));
}

TEST(HleVoiceRender, StepGainLandsOnTheTargets) {
    using namespace f3rt::hle;
    Voice idle;
    idle.left = 100;
    idle.right = 200;
    idle.step_left = 10;
    idle.step_right = -5;
    idle.gain_dirty = false;
    step_gain(idle);
    EXPECT_EQ(idle.left, 100.0);
    EXPECT_EQ(idle.right, 200.0);
    EXPECT_EQ(idle.gain_frames, 0u);
    EXPECT_FALSE(idle.gain_dirty);

    Voice voice;
    voice.left = 0x4010;
    voice.right = 0x5080;
    voice.step_left = 1.0;
    voice.step_right = -1.5;
    voice.target_left = voice.left + 3.25;
    voice.target_right = voice.right - 4.75;
    voice.gain_frames = 3;
    voice.gain_dirty = false;

    step_gain(voice);
    EXPECT_EQ(voice.left, 0x4011);
    EXPECT_EQ(voice.right, 0x5080 - 1.5);
    EXPECT_EQ(voice.gain_frames, 2u);
    EXPECT_FALSE(voice.gain_dirty);
    EXPECT_EQ(float_bits(voice.left_gain), float_bits(interpolated_gain(0x4011)));

    step_gain(voice);
    EXPECT_EQ(voice.gain_frames, 1u);
    EXPECT_FALSE(voice.gain_dirty);

    step_gain(voice);
    EXPECT_EQ(voice.gain_frames, 0u);
    EXPECT_EQ(voice.left, voice.target_left);
    EXPECT_EQ(voice.right, voice.target_right);
    EXPECT_TRUE(voice.gain_dirty);
}

TEST(HleVoiceRender, RampingGainMatchesFrameByFrameSteps) {
    using namespace f3rt::hle;
    constexpr size_t N = 16;
    Voice a = ramping_voice();
    Voice b = a;
    ASSERT_GE(a.left_segment.frames, N);
    ASSERT_GE(a.right_segment.frames, N);
    ASSERT_GT(a.gain_frames, N);
    ASSERT_GE(N, 8u);

    RampingGain ramp(a);
    for (size_t k = 0; k < N; ++k) {
        const StereoGain from_ramp = ramp.next();
        step_gain(b);
        const StereoGain from_steps = current_gain(b);
        EXPECT_EQ(float_bits(from_ramp.left), float_bits(from_steps.left)) << "frame " << k;
        EXPECT_EQ(float_bits(from_ramp.right), float_bits(from_steps.right)) << "frame " << k;
    }
    ramp.store(a, N);

    EXPECT_EQ(double_bits(a.left), double_bits(b.left));
    EXPECT_EQ(double_bits(a.right), double_bits(b.right));
    EXPECT_EQ(double_bits(a.left_segment.value), double_bits(b.left_segment.value));
    EXPECT_EQ(double_bits(a.right_segment.value), double_bits(b.right_segment.value));
    EXPECT_EQ(double_bits(a.left_segment.slope), double_bits(b.left_segment.slope));
    EXPECT_EQ(double_bits(a.right_segment.slope), double_bits(b.right_segment.slope));
    EXPECT_EQ(a.left_segment.frames, b.left_segment.frames);
    EXPECT_EQ(a.right_segment.frames, b.right_segment.frames);
    EXPECT_EQ(a.gain_frames, b.gain_frames);
    EXPECT_EQ(float_bits(a.left_gain), float_bits(b.left_gain));
    EXPECT_EQ(float_bits(a.right_gain), float_bits(b.right_gain));
    EXPECT_EQ(a.gain_dirty, b.gain_dirty);
}

TEST(HleVoiceRender, VoiceFilterRoundsCoefficientsOnce) {
    expect_filter_matches_filter_step<0>();
    expect_filter_matches_filter_step<1>();
    expect_filter_matches_filter_step<2>();
    expect_filter_matches_filter_step<3>();
}

TEST(HleVoiceRender, VoiceBusRoutesThePairOrDiscards) {
    using namespace f3rt::hle;
    std::vector<float> buses(VOICE_BLOCK_FRAMES * 3 * 8, 9.f);
    MutableAudioBlockSpan<8> span(buses);
    std::array<float, VOICE_BLOCK_FRAMES * 8> discard;
    discard.fill(7.f);
    constexpr size_t FIRST = VOICE_BLOCK_FRAMES, FRAMES = 5;

    const VoiceBus dry(span, 1, discard);
    const Chunk routed = dry.chunk(FIRST, FRAMES);
    EXPECT_EQ(routed.rows, span.data() + FIRST * 8 + size_t(OtisBus::DryL));
    EXPECT_EQ(routed.first_frame, FIRST);
    EXPECT_EQ(routed.frames, FRAMES);
    EXPECT_EQ(routed.row(2), routed.rows + 16);
    EXPECT_EQ(routed.frame(2), FIRST + 2);
    for (float v : discard) EXPECT_EQ(v, 7.f);

    const VoiceBus delay(span, 2, discard);
    EXPECT_EQ(delay.chunk(0, 1).rows, span.data() + size_t(OtisBus::DelayL));

    const VoiceBus unrouted(span, 0, discard);
    const Chunk dropped = unrouted.chunk(FIRST, FRAMES);
    EXPECT_EQ(dropped.rows, discard.data());
    EXPECT_EQ(dropped.first_frame, FIRST);
    EXPECT_EQ(dropped.frames, FRAMES);
    for (size_t i = 0; i < discard.size(); ++i)
        EXPECT_EQ(discard[i], i < FRAMES * 8 ? 0.f : 7.f) << "float " << i;
    for (float v : buses) EXPECT_EQ(v, 9.f);
}

TEST(Audio, AudioBlockAlignmentAndMultiSubscript) {
    using namespace f3rt::hle;
    static_assert(alignof(BusBlock) == 64, "BusBlock must be 64-byte aligned for SIMD");
    static_assert(alignof(StereoBlock) == 64, "StereoBlock must be 64-byte aligned for SIMD");
    static_assert(BusBlock::K_CHANNELS == 8);
    static_assert(StereoBlock::K_CHANNELS == 2);
    static_assert(BusBlock::K_MAX_FRAMES == 256);

    BusBlock buses;
    buses.resize(100);
    EXPECT_EQ(buses.size(), 100);
    EXPECT_EQ(buses.capacity(), 256);

    // C++23 multi-subscript with enum
    buses[10, OtisBus::DryL] = 0.75f;
    buses[10, OtisBus::ReverbR] = -0.5f;
    EXPECT_EQ((buses[10, OtisBus::DryL]), 0.75f);
    EXPECT_EQ((buses[10, OtisBus::ReverbR]), -0.5f);

    // Numeric multi-subscript matches enum index
    EXPECT_EQ((buses[10, static_cast<size_t>(OtisBus::DryL)]), 0.75f);
    EXPECT_EQ((buses[10, static_cast<size_t>(OtisBus::ReverbR)]), -0.5f);

    // Single-subscript frame span
    auto frame10 = buses[10];
    EXPECT_EQ(frame10.size(), 8);
    EXPECT_EQ(frame10[static_cast<size_t>(OtisBus::DryL)], 0.75f);
    EXPECT_EQ(frame10[static_cast<size_t>(OtisBus::ReverbR)], -0.5f);

    // Non-owning BusSpan and MutableStereoSpan
    BusSpan bus_span = buses.span();
    EXPECT_EQ(bus_span.size(), 100);
    EXPECT_EQ((bus_span[10, OtisBus::DryL]), 0.75f);

    StereoBlock stereo;
    stereo.resize(100);
    stereo[10, StereoChannel::Left] = 1.0f;
    stereo[10, StereoChannel::Right] = 2.0f;
    EXPECT_EQ((stereo[10, StereoChannel::Left]), 1.0f);
    EXPECT_EQ((stereo[10, StereoChannel::Right]), 2.0f);
}

TEST(Audio, EffectsMixAudioBlockOverload) {
    using namespace f3rt::hle;
    const uint8_t params[10] = {30, 58, 73, 32, 37, 0, 40, 43, 18, 32};
    const auto sound_rom = effects_test_rom(11, 6, params);

    Effects fx(sound_rom);
    fx.select(11);

    BusBlock buses;
    buses.resize(BLOCK_FRAMES);
    buses[0, OtisBus::DryL] = 1.0f;
    buses[0, OtisBus::DryR] = 0.5f;

    StereoBlock stereo;
    fx.mix(buses, stereo, {1.0f, 1.0f}, {1.0f, 1.0f});

    EXPECT_EQ(stereo.size(), BLOCK_FRAMES);
    EXPECT_GT((stereo[0, StereoChannel::Left]), 0.0f);
    EXPECT_GT((stereo[0, StereoChannel::Right]), 0.0f);
}

TEST(Audio, PlanarAudioBlockAndScaledBusSubspan) {
    using namespace f3rt::hle;

    BusBlock buses;
    buses.resize(32);
    for (size_t f = 0; f < 32; ++f) {
        buses[f, OtisBus::DryL] = 1.0f + float(f);
        buses[f, OtisBus::DryR] = 2.0f + float(f);
        buses[f, OtisBus::DelayL] = 3.0f + float(f);
        buses[f, OtisBus::DelayR] = 4.0f + float(f);
        buses[f, OtisBus::ReverbL] = 5.0f + float(f);
        buses[f, OtisBus::ReverbR] = 6.0f + float(f);
    }

    const std::array<float, 2> gain = {0.5f, 2.0f};
    PlanarAudioBlock<6, 32> in;

    // Test full active bus assignment
    in = buses.span().subspan(4, 16) * gain;
    EXPECT_FLOAT_EQ(in[0][0], (1.0f + 4.0f) * 0.5f);
    EXPECT_FLOAT_EQ(in[1][0], (2.0f + 4.0f) * 2.0f);
    EXPECT_FLOAT_EQ(in[2][0], (3.0f + 4.0f) * 0.5f);
    EXPECT_FLOAT_EQ(in[3][0], (4.0f + 4.0f) * 2.0f);
    EXPECT_FLOAT_EQ(in[4][0], (5.0f + 4.0f) * 0.5f);
    EXPECT_FLOAT_EQ(in[5][0], (6.0f + 4.0f) * 2.0f);

    // Clear and test dry-only slice assignment
    in.storage[2][0] = 99.0f;
    in.storage[4][0] = 99.0f;
    in = buses.span().subspan(4, 16).dry() * gain;
    EXPECT_FLOAT_EQ(in[0][0], (1.0f + 4.0f) * 0.5f);
    EXPECT_FLOAT_EQ(in[1][0], (2.0f + 4.0f) * 2.0f);
    EXPECT_FLOAT_EQ(in[2][0], 99.0f); // untouched
    EXPECT_FLOAT_EQ(in[4][0], 99.0f); // untouched
}

TEST(Audio, MutableBusSliceAccumulateAndOtisTargetBus) {
    using namespace f3rt::hle;

    EXPECT_EQ(otis_target_bus(0), std::nullopt);
    EXPECT_EQ(otis_target_bus(1), BusSelect::Dry);
    EXPECT_EQ(otis_target_bus(2), BusSelect::Delay);
    EXPECT_EQ(otis_target_bus(3), BusSelect::Reverb);
    EXPECT_EQ(otis_target_bus(4), std::nullopt);

    BusBlock buses;
    buses.resize(16);
    buses.fill(0.0f);

    PlanarAudioBlock<2, 16> planar;
    for (size_t f = 0; f < 16; ++f) {
        planar[0][f] = 10.0f + float(f);
        planar[1][f] = 20.0f + float(f);
    }

    // Accumulate planar stereo into Dry
    buses.subspan(0, 16).dry() += planar;
    EXPECT_FLOAT_EQ((buses[0, OtisBus::DryL]), 10.0f);
    EXPECT_FLOAT_EQ((buses[0, OtisBus::DryR]), 20.0f);
    EXPECT_FLOAT_EQ((buses[5, OtisBus::DryL]), 15.0f);
    EXPECT_FLOAT_EQ((buses[5, OtisBus::DryR]), 25.0f);
    EXPECT_FLOAT_EQ((buses[0, OtisBus::DelayL]), 0.0f);

    // Accumulate again with slice(BusSelect::Delay)
    buses.subspan(0, 16).slice(BusSelect::Delay) += planar;
    EXPECT_FLOAT_EQ((buses[0, OtisBus::DelayL]), 10.0f);
    EXPECT_FLOAT_EQ((buses[0, OtisBus::DelayR]), 20.0f);

    // Test interleaved stereo accumulation
    StereoBlock stereo;
    stereo.resize(16);
    for (size_t f = 0; f < 16; ++f) {
        stereo[f, StereoChannel::Left] = 1.0f;
        stereo[f, StereoChannel::Right] = 2.0f;
    }
    buses.subspan(0, 16).reverb() += stereo.span();
    EXPECT_FLOAT_EQ((buses[0, OtisBus::ReverbL]), 1.0f);
    EXPECT_FLOAT_EQ((buses[0, OtisBus::ReverbR]), 2.0f);
}

RC_GTEST_PROP(AudioPacer, PolicyInvariants, ()) {
    const uint32_t rate = *rc::gen::element<uint32_t>(8000, 22050, 44100, 48000);
    const size_t target = *rc::gen::inRange<size_t>(1, size_t(rate) / 4 + 1);
    const unsigned max_queue_ms = *rc::gen::inRange<unsigned>(0, 200);
    const bool throttle = *rc::gen::arbitrary<bool>();
    f3rt::AudioPacer pacer({.rate = rate,
                            .target_frames = target,
                            .max_queue_ms = max_queue_ms,
                            .throttle = throttle});
    const size_t cap = target + size_t(rate) * max_queue_ms / 1000;
    const size_t steps = *rc::gen::inRange<size_t>(1, 64);
    uint64_t deepest = 0, sum = 0, drops = 0;
    for (size_t step = 0; step < steps; ++step) {
        const size_t queued = *rc::gen::inRange<size_t>(0, cap + target + 1);
        const size_t chunk = *rc::gen::inRange<size_t>(0, target * 2 + 1);
        const auto action = pacer.before_push(queued, chunk);
        const bool dropped = throttle && queued > cap;
        RC_ASSERT(action.drop_backlog == dropped);
        drops += dropped;
        const size_t effective = dropped ? 0 : queued;
        RC_ASSERT(action.lead_silence_frames ==
                  (throttle && effective == 0 && chunk < target ? target - chunk : 0));
        const size_t depth = effective + action.lead_silence_frames + chunk;
        const double ratio = pacer.after_push(depth);
        if (throttle) RC_ASSERT(ratio >= 0.995 - 1e-9 && ratio <= 1.005 + 1e-9);
        else RC_ASSERT(ratio == 1.0);
        deepest = std::max(deepest, uint64_t(depth));
        sum += depth;
        RC_ASSERT(pacer.stats().queue_max >= depth);
    }
    RC_ASSERT(pacer.stats().queue_max == deepest);
    RC_ASSERT(pacer.stats().queue_samples == steps);
    RC_ASSERT(pacer.stats().queue_sum == sum);
    RC_ASSERT(pacer.stats().drops == drops);
}

namespace {
struct Ring {
    std::array<uint8_t, f3rt::SHARED_RAM_SIZE> shared{};
    size_t written = 0;

    void put(std::initializer_list<uint8_t> bytes) {
        for (uint8_t byte : bytes) shared[written++ & f3rt::MAILBOX_RING_MASK] = byte;
    }
    void publish() {
        f3rt::write_be<uint16_t>(shared.data() + f3rt::MAILBOX_PRODUCER_H,
                                 uint16_t((written * 2) & f3rt::SharedMailbox::INDEX_MASK));
    }
    uint16_t consumer() const { return f3rt::read_be16(shared.data() + f3rt::MAILBOX_CONSUMER_H); }
};

std::vector<f3rt::hle::CommandPacket> drain_packets(f3rt::SharedMailbox &mailbox) {
    std::vector<f3rt::hle::CommandPacket> packets;
    mailbox.drain([&](const f3rt::hle::CommandPacket &packet) { packets.push_back(packet); });
    return packets;
}

struct Recorder {
    static constexpr int GATED = 1, FAIL = -1;
    std::vector<int> applied;
    std::thread::id thread_id;
    std::latch *gate = nullptr;

    void apply(const int &value) {
        thread_id = std::this_thread::get_id();
        if (gate && value == GATED) gate->wait();
        if (value == FAIL) throw std::runtime_error("apply failed");
        applied.push_back(value);
    }
};
}

TEST(AudioMailbox, DeliversPacketsInRingOrderAndWritesTheConsumerBack) {
    Ring ram;
    ram.put({3, 0x82, 0, 4, 0x21, 0, 0x28});
    ram.publish();
    f3rt::SharedMailbox mailbox(ram.shared.data());
    const auto packets = drain_packets(mailbox);
    ASSERT_EQ(packets.size(), 2u);
    EXPECT_EQ(packets[0].opcode(), 0x82);
    EXPECT_EQ(packets[1].opcode(), 0x21);
    EXPECT_EQ(mailbox.consumer(), 14);
    EXPECT_EQ(ram.consumer(), 14);
}

TEST(AudioMailbox, PacketsWrapAroundTheRingEnd) {
    Ring ram;
    ram.written = 0x3fe;
    ram.put({4, 0x21, 1, 2});
    ram.publish();
    f3rt::SharedMailbox mailbox(ram.shared.data());
    mailbox.restore(0x7fc);
    const auto packets = drain_packets(mailbox);
    ASSERT_EQ(packets.size(), 1u);
    const auto &param = std::get<f3rt::hle::EffectParameter>(packets[0].command());
    EXPECT_EQ(param.index, 1);
    EXPECT_EQ(param.value, 2);
    EXPECT_EQ(ram.consumer(), 4);
}

TEST(AudioMailbox, StopsAtAnIncompleteOrMalformedPacket) {
    Ring ram;
    ram.put({5, 0x8f, 0, 0});
    ram.publish();
    f3rt::SharedMailbox mailbox(ram.shared.data());
    EXPECT_TRUE(drain_packets(mailbox).empty());
    EXPECT_EQ(ram.consumer(), 0);
    ram.put({0x3c});
    ram.publish();
    const auto packets = drain_packets(mailbox);
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].opcode(), 0x8f);
    EXPECT_EQ(ram.consumer(), 10);
    for (uint8_t length : {uint8_t(1), uint8_t(0)}) {
        Ring bad;
        bad.put({length, 0x82, 0});
        bad.publish();
        f3rt::SharedMailbox stopped(bad.shared.data());
        EXPECT_TRUE(drain_packets(stopped).empty());
        EXPECT_EQ(bad.consumer(), 0);
    }
}

TEST(AudioMailbox, ConsumesOversizePacketsUnseen) {
    Ring ram;
    ram.put({9, 0x21, 0, 0, 0, 0, 0, 0, 0});
    ram.put({3, 0x82, 0});
    ram.publish();
    f3rt::SharedMailbox mailbox(ram.shared.data());
    const auto packets = drain_packets(mailbox);
    ASSERT_EQ(packets.size(), 1u);
    EXPECT_EQ(packets[0].opcode(), 0x82);
    EXPECT_EQ(ram.consumer(), 24);
}

TEST(AudioMailbox, IgnoresTheProducerLowBit) {
    Ring ram;
    ram.put({3, 0x82, 0});
    ram.publish();
    f3rt::write_be<uint16_t>(ram.shared.data() + f3rt::MAILBOX_PRODUCER_H, 7);
    f3rt::SharedMailbox mailbox(ram.shared.data());
    EXPECT_EQ(drain_packets(mailbox).size(), 1u);
    EXPECT_EQ(ram.consumer(), 6);
}

TEST(AudioMailbox, FullRingDrainsInOneCall) {
    Ring ram;
    for (int i = 0; i < 341; ++i) ram.put({3, 0x82, 0});
    ram.publish();
    f3rt::SharedMailbox mailbox(ram.shared.data());
    EXPECT_EQ(drain_packets(mailbox).size(), 341u);
    EXPECT_EQ(mailbox.consumer(), 0x7fe);
    EXPECT_EQ(ram.consumer(), 0x7fe);
}

TEST(AudioMailbox, GainResetAndRestore) {
    Ring ram;
    ram.shared[f3rt::MAILBOX_GAIN_CH0] = 0x11;
    ram.shared[f3rt::MAILBOX_GAIN_CH1] = 0x22;
    f3rt::SharedMailbox mailbox(ram.shared.data());
    EXPECT_EQ(mailbox.gain(0), 0x11);
    EXPECT_EQ(mailbox.gain(1), 0x22);
    EXPECT_EQ(mailbox.gain(2), 0x11);
    ram.put({3, 0x82, 0});
    ram.publish();
    EXPECT_EQ(drain_packets(mailbox).size(), 1u);
    EXPECT_EQ(mailbox.consumer(), 6);
    mailbox.reset();
    EXPECT_EQ(mailbox.consumer(), 0);
    EXPECT_EQ(ram.consumer(), 6);
    mailbox.restore(0x7fc);
    EXPECT_EQ(mailbox.consumer(), 0x7fc);
}

TEST(AudioMailbox, RequiresSharedRam) {
    EXPECT_THROW(f3rt::SharedMailbox{nullptr}, std::invalid_argument);
}

TEST(AudioPcmRing, DrainExchangesTheDacChannels) {
    auto ring = std::make_unique<f3rt::PcmRing>();
    const std::array<float, 4> frames{0.5f, -1.f, 2.f, -2.f};
    ring->write(frames.data(), 2);
    std::array<float, 4> floats{};
    EXPECT_EQ(ring->drain(floats.data(), 2), 2u);
    EXPECT_EQ(floats, (std::array<float, 4>{-1.f, 0.5f, -2.f, 2.f}));
    ring->write(frames.data(), 2);
    std::array<int16_t, 4> samples{};
    EXPECT_EQ(ring->drain(samples.data(), 2), 2u);
    EXPECT_EQ(samples, (std::array<int16_t, 4>{-32768, 16384, -32768, 32767}));
}

TEST(AudioPcmRing, KeepsTheNewestCapacityFrames) {
    auto ring = std::make_unique<f3rt::PcmRing>();
    const size_t frames = f3rt::PcmRing::CAPACITY + 3;
    std::vector<float> stereo(frames * 2);
    for (size_t i = 0; i < frames; ++i) stereo[i * 2] = stereo[i * 2 + 1] = float(i);
    ring->write(stereo.data(), frames);
    EXPECT_EQ(ring->available(), f3rt::PcmRing::CAPACITY);
    std::vector<float> out(f3rt::PcmRing::CAPACITY * 2);
    EXPECT_EQ(ring->drain(out.data(), frames), f3rt::PcmRing::CAPACITY);
    EXPECT_EQ(out.front(), 3.f);
    EXPECT_EQ(out.back(), float(frames - 1));
}

TEST(AudioPcmRing, DrainRespectsMaxFrames) {
    auto ring = std::make_unique<f3rt::PcmRing>();
    const std::vector<float> stereo(10 * 2, 0.25f);
    ring->write(stereo.data(), 10);
    std::array<float, 200> out{};
    EXPECT_EQ(ring->drain(out.data(), 4), 4u);
    EXPECT_EQ(ring->available(), 6u);
    EXPECT_EQ(ring->drain(out.data(), 100), 6u);
    EXPECT_EQ(ring->available(), 0u);
}

TEST(AudioPcmRing, DrainReleasedHoldsBackTheFrameJustClosed) {
    auto ring = std::make_unique<f3rt::PcmRing>();
    const std::vector<float> first(100 * 2, 0.25f), second(50 * 2, 0.25f);
    std::array<int16_t, 200> out{};
    ring->write(first.data(), 100);
    ring->close_frame(100);
    EXPECT_EQ(ring->drain_released(out.data(), 100), 0u);
    ring->write(second.data(), 50);
    ring->close_frame(150);
    EXPECT_EQ(ring->drain_released(out.data(), 100), 100u);
    EXPECT_EQ(ring->drain_released(out.data(), 100), 0u);
    EXPECT_EQ(ring->drain(out.data(), 100), 50u);
}

TEST(AudioPcmRing, DiscardDropsBufferedFramesAndReleasesThem) {
    auto ring = std::make_unique<f3rt::PcmRing>();
    const std::vector<float> stale(10 * 2, 0.25f), fresh(5 * 2, 0.5f);
    std::array<int16_t, 20> out{};
    ring->write(stale.data(), 10);
    ring->close_frame(10);
    ring->close_frame(10);
    ring->discard();
    EXPECT_EQ(ring->available(), 0u);
    ring->write(fresh.data(), 5);
    EXPECT_EQ(ring->drain_released(out.data(), 10), 0u);
    ring->close_frame(15);
    EXPECT_EQ(ring->drain_released(out.data(), 10), 0u);
    ring->close_frame(15);
    EXPECT_EQ(ring->drain_released(out.data(), 10), 5u);
    EXPECT_EQ(out[0], 16384);
}

TEST(AudioWorkerQueue, AppliesInPostOrderOnItsOwnThread) {
    Recorder recorder;
    f3rt::WorkerQueue<int, Recorder, 8> queue(recorder);
    std::vector<int> expected;
    for (int i = 1; i <= 100; ++i) {
        queue.post(i);
        expected.push_back(i);
    }
    queue.flush();
    EXPECT_EQ(recorder.applied, expected);
    EXPECT_NE(recorder.thread_id, std::this_thread::get_id());
}

TEST(AudioWorkerQueue, FullQueueWaitsInsteadOfDropping) {
    std::latch gate(1);
    Recorder recorder;
    recorder.gate = &gate;
    f3rt::WorkerQueue<int, Recorder, 2> queue(recorder);
    std::promise<void> returned;
    auto posted = returned.get_future();
    queue.post(Recorder::GATED);
    queue.post(2);
    queue.post(3);
    std::thread producer([&] {
        queue.post(4);
        returned.set_value();
    });
    EXPECT_EQ(posted.wait_for(std::chrono::milliseconds{50}), std::future_status::timeout);
    gate.count_down();
    ASSERT_EQ(posted.wait_for(std::chrono::seconds{10}), std::future_status::ready);
    producer.join();
    queue.flush();
    EXPECT_EQ(recorder.applied, (std::vector<int>{1, 2, 3, 4}));
}

TEST(AudioWorkerQueue, FailureStopsTheWorkerAndIsRethrown) {
    Recorder recorder;
    f3rt::WorkerQueue<int, Recorder, 8> queue(recorder);
    queue.post(1);
    queue.post(Recorder::FAIL);
    EXPECT_THROW(queue.flush(), std::runtime_error);
    EXPECT_THROW(queue.post(2), std::runtime_error);
    EXPECT_EQ(recorder.applied, (std::vector<int>{1}));
}

TEST(AudioWorkerQueue, DestructionAppliesEveryPostedMessage) {
    Recorder recorder;
    {
        f3rt::WorkerQueue<int, Recorder, 8> queue(recorder);
        for (int i = 0; i < 50; ++i) queue.post(i);
    }
    EXPECT_EQ(recorder.applied.size(), 50u);
}

TEST(AudioSnapshot, WritesTheBigEndianLayout) {
    const f3rt::AudioSnapshot state{0x0102030405060708, 0x1112131415161718, 0x07fe, true};
    std::array<uint8_t, f3rt::AudioSnapshot::BYTES> bytes{};
    state.write(bytes);
    const std::array<uint8_t, f3rt::AudioSnapshot::BYTES> expected{
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x11, 0x12,
        0x13, 0x14, 0x15, 0x16, 0x17, 0x18, 0x07, 0xfe, 0x01};
    EXPECT_EQ(bytes, expected);
}

TEST(AudioSnapshot, ParseRoundTripsWrite) {
    const f3rt::AudioSnapshot state{0x0102030405060708, 0x1112131415161718, 0x07fe, true};
    std::array<uint8_t, f3rt::AudioSnapshot::BYTES> bytes{};
    state.write(bytes);
    const auto parsed = f3rt::AudioSnapshot::parse(bytes);
    EXPECT_EQ(parsed.clock, state.clock);
    EXPECT_EQ(parsed.commands, state.commands);
    EXPECT_EQ(parsed.consumer, state.consumer);
    EXPECT_EQ(parsed.held, state.held);
}

TEST(AudioSnapshot, RejectsAWrongSize) {
    const std::array<uint8_t, 31> short_bytes{};
    const std::array<uint8_t, 33> long_bytes{};
    EXPECT_THROW((void)f3rt::AudioSnapshot::parse(short_bytes), std::invalid_argument);
    EXPECT_THROW((void)f3rt::AudioSnapshot::parse(long_bytes), std::invalid_argument);
    const f3rt::AudioSnapshot state{};
    std::array<uint8_t, 33> out{};
    EXPECT_THROW(state.write(std::span(out).first(31)), std::invalid_argument);
    EXPECT_THROW(state.write(out), std::invalid_argument);
}

TEST(AudioSnapshot, RejectsAMailboxStateTheHleCannotResume) {
    const std::array<uint8_t, f3rt::AudioSnapshot::BYTES> valid{};
    EXPECT_NO_THROW((void)f3rt::AudioSnapshot::parse(valid));
    for (uint16_t consumer : {uint16_t(0x0001), uint16_t(0x0800), uint16_t(0xfffe)}) {
        auto bad = valid;
        f3rt::write_be<uint16_t>(bad.data() + 16, consumer);
        EXPECT_THROW((void)f3rt::AudioSnapshot::parse(bad), std::invalid_argument);
    }
    auto held = valid;
    held[18] = 2;
    EXPECT_THROW((void)f3rt::AudioSnapshot::parse(held), std::invalid_argument);
    for (size_t reserved : {size_t(19), size_t(31)}) {
        auto bad = valid;
        bad[reserved] = 1;
        EXPECT_THROW((void)f3rt::AudioSnapshot::parse(bad), std::invalid_argument);
    }
}
