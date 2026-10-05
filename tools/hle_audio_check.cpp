#include "f3rt/audio.hpp"
#include "f3rt/rom.hpp"
#include "hle_events.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <thread>
#include <vector>

namespace {
void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}
struct Fixture {
    std::array<uint8_t, 0x800> shared{};
    std::vector<f3rt::hle::VoiceEvent> events;
    std::thread::id main_thread = std::this_thread::get_id();
    bool worker_only = true;
    f3rt::Audio audio;
    explicit Fixture(const f3rt::RomSet &rom) {
        audio.load_sound_rom(rom.sound); audio.load_sample_rom(rom.samples);
        audio.set_shared_ram(shared.data()); audio.set_backend(f3rt::Audio::Backend::Hle);
        audio.set_cpu_runner([](int) -> int { throw std::runtime_error("HLE executed sound CPU"); });
        audio.set_hle_observer([this](const auto &event) {
            worker_only &= std::this_thread::get_id() != main_thread; events.push_back(event);
        });
        audio.set_reset(false);
        shared[0x7fa] = shared[0x7fb] = 0x30;
        audio.shared_write(0x7fa, 0); audio.shared_write(0x7fb, 0);
        packet({3, 0x81, 1}, 0);
        packet({6, 0x8d, 1, 1, 0x40, 2}, 0);
        step(1);
        events.clear();
    }
    void packet(std::initializer_list<uint8_t> bytes, uint64_t frame) {
        unsigned producer = (unsigned(shared[0x480]) << 8 | shared[0x481]) >> 1;
        for (auto byte : bytes) shared[producer++ & 0x3ff] = byte;
        producer = (producer & 0x3ff) * 2;
        shared[0x480] = uint8_t(producer >> 8); shared[0x481] = uint8_t(producer);
        audio.shared_write(0x481, frame);
        require(shared[0x480] == shared[0x482] && shared[0x481] == shared[0x483], "mailbox did not acknowledge complete packet");
    }
    std::vector<int16_t> step(uint64_t frontier) {
        audio.advance(266667); audio.finish_frame(frontier);
        std::vector<int16_t> out(audio.available_frames() * 2);
        require(audio.render(out.data(), out.size() / 2) * 2 == out.size(), "PCM frame count changed during drain");
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
}
int main(int argc, char **argv) try {
    if (argc != 2) throw std::runtime_error("usage: f3rt-hle-check ROM_DIRECTORY");
    const auto rom = f3rt::RomSet::load(argv[1], "landmakrj");
    using Kind = f3rt::hle::VoiceEvent::Kind;
    {
        Fixture f(rom);
        f.packet({3, 0x80, 2}, 1);
        for (uint64_t frame = 2; frame <= 182; ++frame) f.step(frame);
        f.packet({6, 0x8d, 2, 1, 0x40, 2}, 182);
        f.packet({6, 0x8e, 2, 1, 39, 104}, 182);
        const auto samples = f.step(183);
        require(f.count(Kind::Start) == 1, "natural sequence end discarded direct-note channels");
        require(std::any_of(samples.begin(), samples.end(), [](int16_t v) { return v != 0; }),
                "direct note after completed non-looping sequence is silent");
    }
    {
        Fixture f(rom);
        f.packet({3, 0x81, 7}, 1);
        for (uint64_t frame = 2; frame <= 121; ++frame) f.step(frame);
        require(f.count(Kind::Start) == 0, "ordinary start incorrectly played an arrangement selection list");
        f.packet({3, 0x81, 0x87}, 121);
        for (uint64_t frame = 122; frame <= 241; ++frame) f.step(frame);
        const auto first = std::find_if(f.events.begin(), f.events.end(),
                                       [](const auto &e) { return e.kind == Kind::Start; });
        require(first != f.events.end() && first->sequence == 4 && first->track == 6 && first->key == 32,
                "selected arrangement did not start its intro's actual sequence/track/key");
        f.packet({3, 0x82, 0x87}, 241); f.step(242);
        const auto starts = f.count(Kind::Start);
        for (uint64_t frame = 243; frame <= 362; ++frame) f.step(frame);
        require(f.count(Kind::Start) == starts, "selected stop left the arrangement child sequencing");
    }
    {
        Fixture corrected(rom), reference(rom);
        const auto before = corrected.save();
        for (auto *f : {&corrected, &reference}) {
            f->packet({6, 0x8e, 1, 1, 39, 104}, 1);
            f->step(2); f->step(3); f->step(4);
        }
        corrected.audio.begin_rollback(1, 4); corrected.load(before);
        corrected.step(2); corrected.step(3); corrected.step(4); corrected.audio.end_rollback();
        const auto faded = corrected.step(5), original = reference.step(5);
        require(faded.size() == original.size() && faded.size() >= 480, "fade comparison interval missing");
        int peak = 0;
        for (size_t sample = 0; sample < 240; ++sample) for (unsigned ch = 0; ch < 2; ++ch) {
            const size_t index = sample * 2 + ch;
            peak = std::max(peak, std::abs(int(original[index])));
            const double expected = original[index] * double(239 - sample) / 240;
            require(std::abs(faded[index] - expected) <= 2, "cancel introduced a discontinuity instead of a linear fade");
        }
        require(peak > 20, "fade scenario did not contain an audible reference voice");
        require(std::all_of(faded.begin() + 480, faded.end(), [](int16_t v) { return v == 0; }),
                "cancelled dry voice emitted audio after its fade");
    }
    {
        Fixture f(rom);
        const auto before = f.save();
        f.packet({6, 0x8e, 1, 1, 39, 104}, 1); f.step(2);
        require(f.count(Kind::Start) == 1, "direct SFX did not allocate one key-region voice");
        const auto note = *std::find_if(f.events.begin(), f.events.end(), [](const auto &e) { return e.kind == Kind::Start; });
        require(note.sample_start == 2157724 && note.sample_end == 2200571 && note.frequency == 796,
                "ROM direct-note sample or pitch differs from the oracle");
        require(note.left_volume == 0xcba0 && note.right_volume == 0xcba0,
                "ROM direct-note encoded volume differs from the oracle");
        require(f.worker_only, "synthesis event ran on main thread");
        const auto canonical = f.save();
        std::array<float, 8> unused{}; f.audio.render(unused.data(), 4);
        require(canonical == f.save(), "host PCM drain changed deterministic main state");
        const auto generated = f.audio.generated_frames();
        f.audio.begin_rollback(1, 2); f.load(before);
        f.packet({6, 0x8e, 1, 1, 39, 104}, 1); f.step(2);
        f.audio.end_rollback(); f.audio.available_frames();
        require(f.audio.generated_frames() == generated, "rollback rewound or advanced rendered audio");
        require(f.count(Kind::Start) == 1 && f.audio.hle_stats().reused == 1,
                "identical replay duplicated a sound");
        require(f.save() == canonical, "identical replay changed deterministic protocol state");
    }
    {
        Fixture f(rom);
        const auto before = f.save();
        f.packet({6, 0x8e, 1, 1, 39, 104}, 1); f.step(2); f.step(3); f.step(4);
        f.audio.begin_rollback(1, 4); f.load(before); f.step(2); f.step(3); f.step(4);
        f.audio.end_rollback(); f.audio.available_frames();
        require(f.audio.hle_stats().cancelled == 1 && f.count(Kind::Cancel) == 1,
                "missing speculative SFX was not cancelled by instance");
        auto cancel = *std::find_if(f.events.begin(), f.events.end(), [](const auto &e) { return e.kind == Kind::Cancel; });
        f.step(5);
        auto stop = std::find_if(f.events.begin(), f.events.end(), [&](const auto &e) { return e.kind == Kind::Stop && e.instance == cancel.instance; });
        require(stop != f.events.end(), "cancelled voice survived the fade");
        const uint64_t elapsed = stop->tick - cancel.tick;
        require(elapsed >= 78000 && elapsed <= 80334, "cancellation did not finish its 240-sample fast fade");
    }
    for (unsigned shift : {1u, 2u}) {
        Fixture f(rom);
        const auto before = f.save();
        f.packet({6, 0x8e, 1, 1, 39, 104}, 1); f.step(2);
        f.audio.begin_rollback(1, 2); f.load(before); f.step(2); f.audio.end_rollback();
        for (unsigned frame = 2; frame <= 1 + shift; ++frame) {
            if (frame == 1 + shift) f.packet({6, 0x8e, 1, 1, 39, 104}, frame);
            f.step(frame + 1);
        }
        require(f.count(Kind::Start) == 1 && f.audio.hle_stats().cancelled == 0 && f.audio.hle_stats().reused == 1,
                "one/two-frame command leeway restarted or cancelled the retained note");
    }
    {
        Fixture f(rom);
        const auto before = f.save();
        f.packet({6, 0x8e, 1, 1, 39, 104}, 1); f.step(2); f.step(3); f.step(4);
        f.audio.begin_rollback(1, 4); f.load(before);
        f.packet({6, 0x8d, 1, 1, 0x40, 3}, 1);
        f.packet({6, 0x8e, 1, 1, 39, 104}, 1); f.step(2); f.step(3); f.step(4);
        f.audio.end_rollback(); f.audio.available_frames();
        require(f.audio.hle_stats().reused == 0 && f.audio.hle_stats().cancelled == 1,
                "equal-key packet incorrectly reused a different instrument");
    }
    std::cout << "HLE checks passed: ROM sample/pitch/volume, worker isolation, canonical state, replay identity, cancellation, 1/2-frame leeway, instrument identity\n";
    return 0;
} catch (const std::exception &e) { std::cerr << "HLE check: " << e.what() << '\n'; return 1; }
