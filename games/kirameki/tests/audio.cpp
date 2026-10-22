#include "audio/hle/events.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/rom.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <initializer_list>
#include <thread>
#include <vector>

// Sound test BANK 01 / SOUND CODE 0x187 (song $2c). Its cymbal (key 20, layer 6) is a "direct" voice whose patch
// ignores note-off. The driver's envelope timer (~150 ms) expires into the release ramp (c1983c) and the voice ends
// at its decayed level about 290 ms after the note on. Stopping at the timer expiry at full level cut the cymbal.
TEST(HleAudio, KiramekiDirectVoiceTimerStartsRelease) {
    const char *rom_env = std::getenv("F3_ROM_DIR");
#ifdef F3RT_DEFAULT_ROM_DIR
    if (!rom_env) rom_env = F3RT_DEFAULT_ROM_DIR;
#endif
    if (!rom_env || !*rom_env) GTEST_SKIP() << "F3_ROM_DIR not set; skipping ROM-backed HLE audio checks";
    f3rt::RomSet rom;
    try {
        rom = f3rt::RomSet::load(std::filesystem::path(rom_env), F3RT_DEFAULT_SET);
    } catch (const std::exception &e) {
        GTEST_SKIP() << "ROM set unavailable: " << e.what();
    }
    using Kind = f3rt::hle::VoiceEvent::Kind;
    std::array<uint8_t, 0x800> shared{};
    std::vector<f3rt::hle::VoiceEvent> events;
    f3rt::Audio audio;
    audio.load_sound_rom(rom.sound);
    audio.load_sample_rom(rom.samples);
    audio.set_shared_ram(shared.data());
    audio.set_backend(f3rt::Audio::Backend::Enhanced);
    ASSERT_TRUE(audio.is_enhanced_active());
    audio.set_cpu_runner([](int) -> int { throw std::runtime_error("HLE executed sound CPU"); });
    audio.set_hle_observer([&](const auto &e) { events.push_back(e); });
    audio.set_sound_bank(1);
    audio.set_reset(false);
    shared[f3rt::MAILBOX_GAIN_CH0] = shared[f3rt::MAILBOX_GAIN_CH1] = 0x30;
    audio.shared_write(f3rt::MAILBOX_GAIN_CH0);
    audio.shared_write(f3rt::MAILBOX_GAIN_CH1);
    auto packet = [&](std::initializer_list<uint8_t> bytes) {
        unsigned producer =
            (unsigned(shared[f3rt::MAILBOX_PRODUCER_H]) << 8 | shared[f3rt::MAILBOX_PRODUCER_L]) >> 1;
        for (auto byte : bytes) shared[producer++ & 0x3ff] = byte;
        producer = (producer & 0x3ff) * 2;
        shared[f3rt::MAILBOX_PRODUCER_H] = uint8_t(producer >> 8);
        shared[f3rt::MAILBOX_PRODUCER_L] = uint8_t(producer);
        audio.shared_write(f3rt::MAILBOX_PRODUCER_L);
    };
    auto step = [&] {
        audio.advance(266667);
        audio.finish_frame();
        std::vector<int16_t> out(audio.available_frames() * 2);
        audio.render(out.data(), out.size() / 2);
    };
    packet({0x03, 0x20, 0x05});
    packet({0x04, 0x21, 0x00, 0x28, 0x04, 0x21, 0x01, 0x00, 0x04, 0x21, 0x02, 0x14, 0x04, 0x21, 0x03, 0x1a, 0x04,
            0x21, 0x04, 0x25, 0x04, 0x21, 0x05, 0x00, 0x04, 0x21, 0x06, 0x68, 0x04, 0x21, 0x07, 0x40, 0x04, 0x21,
            0x08, 0x20, 0x04, 0x21, 0x09, 0x27});
    packet({0x03, 0x81, 0x2c, 0x04, 0x86, 0x2c, 0x7f});
    for (int frame = 0; frame < 90; ++frame) step();

    const auto start = std::find_if(events.begin(), events.end(), [](const auto &e) {
        return e.kind == Kind::Start && e.key == 20 && e.layer == 6;
    });
    ASSERT_NE(start, events.end()) << "cymbal voice never started";
    const auto stop = std::find_if(events.begin(), events.end(), [&](const auto &e) {
        return e.kind == Kind::Stop && e.instance == start->instance;
    });
    ASSERT_NE(stop, events.end()) << "cymbal voice never ended";
    const double ms = double(stop->tick - start->tick) / 16000.0;
    EXPECT_GT(ms, 250.0) << "cymbal cut when its envelope timer expired instead of ramping down";
    EXPECT_LT(ms, 340.0);
    EXPECT_LT(stop->left_volume + 0x1000, start->left_volume) << "release ramp did not lower the volume";
}

// Sound test BANK 01 / SOUND CODE 0x187: the first bars start 17-19 voices in one sequencer pulse. The driver
// starts them one after another (docs/notes/raw/tests/2026-10-16-kirameki-187-clap-dynamics.md section 8):
// no voice may start sooner than the previous start group's voices could have been programmed, every started
// voice must also end, and the note-on count must not depend on the queue (nothing dropped).
TEST(HleAudio, KiramekiPulseNotesStartOneDriverSlotApart) {
    const char *rom_env = std::getenv("F3_ROM_DIR");
#ifdef F3RT_DEFAULT_ROM_DIR
    if (!rom_env) rom_env = F3RT_DEFAULT_ROM_DIR;
#endif
    if (!rom_env || !*rom_env) GTEST_SKIP() << "F3_ROM_DIR not set; skipping ROM-backed HLE audio checks";
    f3rt::RomSet rom;
    try {
        rom = f3rt::RomSet::load(std::filesystem::path(rom_env), F3RT_DEFAULT_SET);
    } catch (const std::exception &e) {
        GTEST_SKIP() << "ROM set unavailable: " << e.what();
    }
    using Kind = f3rt::hle::VoiceEvent::Kind;
    std::array<uint8_t, 0x800> shared{};
    std::vector<f3rt::hle::VoiceEvent> events;
    f3rt::Audio audio;
    audio.load_sound_rom(rom.sound);
    audio.load_sample_rom(rom.samples);
    audio.set_shared_ram(shared.data());
    audio.set_backend(f3rt::Audio::Backend::Enhanced);
    ASSERT_TRUE(audio.is_enhanced_active());
    audio.set_cpu_runner([](int) -> int { throw std::runtime_error("HLE executed sound CPU"); });
    audio.set_hle_observer([&](const auto &e) { events.push_back(e); });
    audio.set_sound_bank(1);
    audio.set_reset(false);
    shared[f3rt::MAILBOX_GAIN_CH0] = shared[f3rt::MAILBOX_GAIN_CH1] = 0x30;
    audio.shared_write(f3rt::MAILBOX_GAIN_CH0);
    audio.shared_write(f3rt::MAILBOX_GAIN_CH1);
    auto packet = [&](std::initializer_list<uint8_t> bytes) {
        unsigned producer =
            (unsigned(shared[f3rt::MAILBOX_PRODUCER_H]) << 8 | shared[f3rt::MAILBOX_PRODUCER_L]) >> 1;
        for (auto byte : bytes) shared[producer++ & 0x3ff] = byte;
        producer = (producer & 0x3ff) * 2;
        shared[f3rt::MAILBOX_PRODUCER_H] = uint8_t(producer >> 8);
        shared[f3rt::MAILBOX_PRODUCER_L] = uint8_t(producer);
        audio.shared_write(f3rt::MAILBOX_PRODUCER_L);
    };
    auto step = [&] {
        audio.advance(266667);
        audio.finish_frame();
        std::vector<int16_t> out(audio.available_frames() * 2);
        audio.render(out.data(), out.size() / 2);
    };
    packet({0x03, 0x20, 0x05});
    packet({0x04, 0x21, 0x00, 0x28, 0x04, 0x21, 0x01, 0x00, 0x04, 0x21, 0x02, 0x14, 0x04, 0x21, 0x03, 0x1a, 0x04,
            0x21, 0x04, 0x25, 0x04, 0x21, 0x05, 0x00, 0x04, 0x21, 0x06, 0x68, 0x04, 0x21, 0x07, 0x40, 0x04, 0x21,
            0x08, 0x20, 0x04, 0x21, 0x09, 0x27});
    packet({0x03, 0x81, 0x2c, 0x04, 0x86, 0x2c, 0x7f});
    for (int frame = 0; frame < 150; ++frame) step();

    std::vector<uint64_t> ticks;
    for (const auto &e : events)
        if (e.kind == Kind::Start) ticks.push_back(e.tick);
    ASSERT_GT(ticks.size(), 40u);
    std::sort(ticks.begin(), ticks.end());
    // Group the voices that share a start tick (layers of one note) and check the next group is at least one
    // driver slot per voice of the group before it away.
    size_t big_pulse = 0;
    for (size_t i = 0; i < ticks.size();) {
        size_t j = i;
        while (j < ticks.size() && ticks[j] == ticks[i]) ++j;
        if (j < ticks.size())
            EXPECT_GE(ticks[j], ticks[i] + uint64_t(j - i) * f3rt::hle::DRIVER_KEYON_CYCLES)
                << "voices started faster than the driver can program them at tick " << ticks[i];
        i = j;
    }
    // The opening downbeat spreads over more than 10 ms; without the driver clock it is a single tick.
    for (size_t i = 0; i < ticks.size(); ++i)
        for (size_t j = i; j < ticks.size() && ticks[j] - ticks[i] < 16000ull * 40; ++j)
            big_pulse = std::max(big_pulse, j - i + 1);
    EXPECT_GE(big_pulse, 12u);
    const uint64_t first = ticks.front();
    size_t within_10ms = 0;
    for (uint64_t t : ticks)
        if (t - first < 16000ull * 10) ++within_10ms;
    EXPECT_LT(within_10ms, big_pulse) << "the downbeat was not staggered";

    // Nothing started is left without an end (stop or release), checked at the end of the capture window.
    size_t starts = 0, ended = 0;
    for (const auto &s : events) {
        if (s.kind != Kind::Start) continue;
        ++starts;
        const bool released = std::any_of(events.begin(), events.end(), [&](const auto &e) {
            return e.instance == s.instance && (e.kind == Kind::Release || e.kind == Kind::Stop);
        });
        ended += released;
    }
    EXPECT_GT(ended, starts / 2);
}

// A plain 0x81 start (no selection bit) of sequence 0x2c, the SOUND CODE 0x187 body, plays its 18 bars once and
// parks at the bar boundary. The driver's bar-end path only re-enters a sequence through an arrangement link, so
// the reference trace has no voice start after its last note at ~20.2 s and no replay at ~21.1 s (see
// docs/notes/raw/tests/2026-10-16-kirameki-187-clap-dynamics.md, section 13).
TEST(HleAudio, KiramekiPlainStartStopsAtBarBoundary) {
    const char *rom_env = std::getenv("F3_ROM_DIR");
#ifdef F3RT_DEFAULT_ROM_DIR
    if (!rom_env) rom_env = F3RT_DEFAULT_ROM_DIR;
#endif
    if (!rom_env || !*rom_env) GTEST_SKIP() << "F3_ROM_DIR not set; skipping ROM-backed HLE audio checks";
    f3rt::RomSet rom;
    try {
        rom = f3rt::RomSet::load(std::filesystem::path(rom_env), F3RT_DEFAULT_SET);
    } catch (const std::exception &e) {
        GTEST_SKIP() << "ROM set unavailable: " << e.what();
    }
    using Kind = f3rt::hle::VoiceEvent::Kind;
    std::array<uint8_t, 0x800> shared{};
    std::vector<f3rt::hle::VoiceEvent> events;
    f3rt::Audio audio;
    audio.load_sound_rom(rom.sound);
    audio.load_sample_rom(rom.samples);
    audio.set_shared_ram(shared.data());
    audio.set_backend(f3rt::Audio::Backend::Enhanced);
    ASSERT_TRUE(audio.is_enhanced_active());
    audio.set_cpu_runner([](int) -> int { throw std::runtime_error("HLE executed sound CPU"); });
    audio.set_hle_observer([&](const auto &e) { events.push_back(e); });
    audio.set_sound_bank(1);
    audio.set_reset(false);
    shared[f3rt::MAILBOX_GAIN_CH0] = shared[f3rt::MAILBOX_GAIN_CH1] = 0x30;
    audio.shared_write(f3rt::MAILBOX_GAIN_CH0);
    audio.shared_write(f3rt::MAILBOX_GAIN_CH1);
    auto packet = [&](std::initializer_list<uint8_t> bytes) {
        unsigned producer =
            (unsigned(shared[f3rt::MAILBOX_PRODUCER_H]) << 8 | shared[f3rt::MAILBOX_PRODUCER_L]) >> 1;
        for (auto byte : bytes) shared[producer++ & 0x3ff] = byte;
        producer = (producer & 0x3ff) * 2;
        shared[f3rt::MAILBOX_PRODUCER_H] = uint8_t(producer >> 8);
        shared[f3rt::MAILBOX_PRODUCER_L] = uint8_t(producer);
        audio.shared_write(f3rt::MAILBOX_PRODUCER_L);
    };
    auto step = [&] {
        audio.advance(266667);
        audio.finish_frame();
        std::vector<int16_t> out(audio.available_frames() * 2);
        audio.render(out.data(), out.size() / 2);
    };
    packet({0x03, 0x20, 0x05});
    packet({0x04, 0x21, 0x00, 0x28, 0x04, 0x21, 0x01, 0x00, 0x04, 0x21, 0x02, 0x14, 0x04, 0x21, 0x03, 0x1a, 0x04,
            0x21, 0x04, 0x25, 0x04, 0x21, 0x05, 0x00, 0x04, 0x21, 0x06, 0x68, 0x04, 0x21, 0x07, 0x40, 0x04, 0x21,
            0x08, 0x20, 0x04, 0x21, 0x09, 0x27});
    packet({0x03, 0x81, 0x2c, 0x04, 0x86, 0x2c, 0x7f});
    for (int frame = 0; frame < 60 * 40; ++frame) step();

    std::vector<uint64_t> ticks;
    for (const auto &e : events)
        if (e.kind == Kind::Start) ticks.push_back(e.tick);
    ASSERT_FALSE(ticks.empty());
    const auto [first, last] = std::minmax_element(ticks.begin(), ticks.end());
    // Reference: last start 20.214 s after the event origin; replay (the bug) started at 21.1 s. 16 MHz clock.
    const uint64_t window = 16'000'000ull * 20'500 / 1000;
    EXPECT_LT(*last - *first, window) << "sequence 0x2c replayed after its bar boundary";
}
