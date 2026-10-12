#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/rom.hpp"
#include "audio/hle/events.hpp"
#include <gtest/gtest.h>
#include <algorithm>
#include <array>
#include <filesystem>
#include <initializer_list>
#include <memory>
#include <thread>
#include <vector>

TEST(HleAudio, StateAndSequencing) {
    const char *rom_env = std::getenv("F3_ROM_DIR");
#ifdef F3RT_DEFAULT_ROM_DIR
    if (!rom_env) rom_env = F3RT_DEFAULT_ROM_DIR;
#endif
    if (!rom_env || !*rom_env) {
        GTEST_SKIP() << "F3_ROM_DIR not set; skipping ROM-backed HLE audio checks";
    }
    const std::filesystem::path rom_dir(rom_env);
    const std::string set = F3RT_DEFAULT_SET;
    f3rt::RomSet rom;
    try {
        rom = f3rt::RomSet::load(rom_dir, set);
    } catch (const std::exception &e) {
        GTEST_SKIP() << "ROM set unavailable: " << e.what();
    }
    f3rt::Audio probe_audio;
    probe_audio.load_sound_rom(rom.sound);
    probe_audio.load_sample_rom(rom.samples);
    ASSERT_TRUE(probe_audio.backend_available(f3rt::Audio::Backend::Enhanced))
        << "Enhanced audio backend must be available for Land Maker sound ROM";
    using Kind = f3rt::hle::VoiceEvent::Kind;

    struct Fixture {
        std::array<uint8_t, 0x800> shared{};
        std::vector<f3rt::hle::VoiceEvent> events;
        std::thread::id main_thread = std::this_thread::get_id();
        bool worker_only = true;
        f3rt::Audio audio;
        explicit Fixture(const f3rt::RomSet &r) {
            audio.load_sound_rom(r.sound); audio.load_sample_rom(r.samples);
            audio.set_shared_ram(shared.data()); audio.set_backend(f3rt::Audio::Backend::Enhanced);
            audio.set_cpu_runner([](int) -> int { throw std::runtime_error("HLE executed sound CPU"); });
            audio.set_hle_observer([this](const auto &event) {
                worker_only &= std::this_thread::get_id() != main_thread; events.push_back(event);
            });
            audio.set_reset(false);
            shared[f3rt::MAILBOX_GAIN_CH0] = shared[f3rt::MAILBOX_GAIN_CH1] = 0x30;
            audio.shared_write(f3rt::MAILBOX_GAIN_CH0); audio.shared_write(f3rt::MAILBOX_GAIN_CH1);
            packet({3, 0x81, 1});
            packet({6, 0x8d, 1, 1, 0x40, 2});
            step();
            events.clear();
        }
        void packet(std::initializer_list<uint8_t> bytes) {
            unsigned producer = (unsigned(shared[f3rt::MAILBOX_PRODUCER_H]) << 8 | shared[f3rt::MAILBOX_PRODUCER_L]) >> 1;
            for (auto byte : bytes) shared[producer++ & 0x3ff] = byte;
            producer = (producer & 0x3ff) * 2;
            shared[f3rt::MAILBOX_PRODUCER_H] = uint8_t(producer >> 8); shared[f3rt::MAILBOX_PRODUCER_L] = uint8_t(producer);
            audio.shared_write(f3rt::MAILBOX_PRODUCER_L);
            EXPECT_EQ(shared[f3rt::MAILBOX_PRODUCER_H], shared[f3rt::MAILBOX_CONSUMER_H]);
            EXPECT_EQ(shared[f3rt::MAILBOX_PRODUCER_L], shared[f3rt::MAILBOX_CONSUMER_L]);
        }
        std::vector<int16_t> step() {
            audio.advance(266667); audio.finish_frame();
            std::vector<int16_t> out(audio.available_frames() * 2);
            EXPECT_EQ(audio.render(out.data(), out.size() / 2) * 2, out.size());
            return out;
        }
        std::vector<uint8_t> save() {
            std::vector<uint8_t> out(audio.state_size() + shared.size());
            audio.save_state(std::span(out.data(), audio.state_size()));
            std::copy(shared.begin(), shared.end(), out.begin() + audio.state_size()); return out;
        }
        void load(const std::vector<uint8_t> &state) {
            audio.load_state(std::span(state.data(), audio.state_size()));
            std::copy(state.begin() + audio.state_size(), state.end(), shared.begin());
        }
        size_t count(f3rt::hle::VoiceEvent::Kind kind) const {
            return size_t(std::count_if(events.begin(), events.end(), [=](const auto &e) { return e.kind == kind; }));
        }
    };

    // 1. Clock adoption and state restore
    for (bool host_ahead : {false, true}) {
        Fixture host(rom), guest(rom);
        for (int frame = 2; frame <= 22; ++frame)
            (host_ahead ? host : guest).step();
        guest.load(host.save());
        guest.packet({6, 0x8e, 1, 1, 39, 104});
        const auto samples = guest.step();
        EXPECT_EQ(samples.size(), 1600u) << "adopted clock stalled or burst HLE playback";
        EXPECT_TRUE(std::any_of(samples.begin(), samples.end(), [](int16_t v) { return v != 0; }))
            << "adopted state lost future HLE commands";
    }

    // 2. Unsafe mailbox rejection
    {
        Fixture f(rom);
        const auto before = f.save();
        for (const auto &change : std::array<std::array<uint8_t, 2>, 4>{{{24, 255}, {25, 1}, {26, 2}, {27, 1}}}) {
            auto invalid = before;
            invalid[change[0]] = change[1];
            bool rejected = false;
            try { f.load(invalid); } catch (const std::invalid_argument &) { rejected = true; }
            EXPECT_TRUE(rejected) << "unsafe HLE mailbox state was accepted";
            EXPECT_EQ(f.save(), before) << "corrupted state mutated machine";
        }
    }

    // 3. Direct note after sequence completion
    {
        Fixture f(rom);
        f.packet({3, 0x80, 2});
        for (int frame = 2; frame <= 182; ++frame) f.step();
        f.packet({6, 0x8d, 2, 1, 0x40, 2});
        f.packet({6, 0x8e, 2, 1, 39, 104});
        const auto samples = f.step();
        EXPECT_EQ(f.count(Kind::Start), 1u) << "natural sequence end discarded direct-note channels";
        EXPECT_TRUE(std::any_of(samples.begin(), samples.end(), [](int16_t v) { return v != 0; }))
            << "direct note after completed non-looping sequence is silent";
    }

    // 4. Arrangement selection & stop
    {
        Fixture f(rom);
        f.packet({3, 0x81, 7});
        for (int frame = 2; frame <= 121; ++frame) f.step();
        EXPECT_EQ(f.count(Kind::Start), 0u) << "ordinary start incorrectly played an arrangement selection list";
        f.packet({3, 0x81, 0x87});
        for (int frame = 122; frame <= 241; ++frame) f.step();
        const auto first = std::find_if(f.events.begin(), f.events.end(),
                                       [](const auto &e) { return e.kind == Kind::Start; });
        ASSERT_NE(first, f.events.end());
        EXPECT_EQ(first->sequence, 4);
        EXPECT_EQ(first->track, 6);
        EXPECT_EQ(first->key, 32);
        f.packet({3, 0x82, 0x87}); f.step();
        const auto starts = f.count(Kind::Start);
        for (int frame = 243; frame <= 362; ++frame) f.step();
        EXPECT_EQ(f.count(Kind::Start), starts) << "selected stop left the arrangement child sequencing";
    }

    // 5. ROM sample/pitch/volume & thread isolation
    {
        Fixture f(rom);
        f.packet({6, 0x8e, 1, 1, 39, 104}); f.step();
        EXPECT_EQ(f.count(Kind::Start), 1u) << "direct SFX did not allocate one key-region voice";
        const auto note_it = std::find_if(f.events.begin(), f.events.end(), [](const auto &e) { return e.kind == Kind::Start; });
        ASSERT_NE(note_it, f.events.end());
        EXPECT_EQ(note_it->sample_start, 2157724u);
        EXPECT_EQ(note_it->sample_end, 2200571u);
        EXPECT_EQ(note_it->frequency, 796u);
        EXPECT_EQ(note_it->left_volume, 0xcba0u);
        EXPECT_EQ(note_it->right_volume, 0xcba0u);
        EXPECT_TRUE(f.worker_only) << "synthesis event ran on main thread";
        const auto before_drain = f.save();
        std::array<float, 8> unused{}; f.audio.render(unused.data(), 4);
        EXPECT_EQ(before_drain, f.save()) << "host PCM drain changed main HLE state";
    }
}
