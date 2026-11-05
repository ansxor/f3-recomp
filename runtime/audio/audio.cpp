#include "f3rt/audio.hpp"
#include "audio/hle/core.hpp"
#include "audio/pcm_ring.hpp"
#include "audio/shared_mailbox.hpp"
#include "audio/snapshot.hpp"
#include "audio/worker_queue.hpp"
#include "f3rt/memory_map.hpp"
#include <atomic>
#include <chrono>
#include <cstdint>
#include <stdexcept>
#include <utility>
#include <variant>
#include <vector>

namespace f3rt {
namespace {
constexpr size_t QUEUE_SIZE = 4096;

struct CoreInput {
    struct Advance {
        void apply(hle::Core &core, uint64_t tick) const { core.advance(tick); }
    };
    struct Command {
        hle::CommandPacket packet;
        uint64_t instance = 0;
        void apply(hle::Core &core, uint64_t tick) const { core.command(packet, instance, tick); }
    };
    struct Reset {
        void apply(hle::Core &core, uint64_t tick) const { core.reset(tick); }
    };
    struct Gain {
        uint8_t channel = 0, raw = 0;
        void apply(hle::Core &core, uint64_t tick) const { core.set_gain(channel, raw, tick); }
    };
    struct Bank {
        uint8_t page = 0;
        void apply(hle::Core &core, uint64_t tick) const { core.set_bank(page, tick); }
    };
    using Kind = std::variant<Advance, Command, Reset, Gain, Bank>;

    Kind kind;
    uint64_t tick = 0;

    void apply(hle::Core &core) const {
        std::visit([&](const auto &input) { input.apply(core, tick); }, kind);
    }
};

struct Synth {
    std::vector<uint8_t> rom;
    std::vector<uint16_t> samples;
    hle::Core core;
    std::atomic<uint64_t> generated{0}, render_ns{0};

    Synth(std::vector<uint8_t> sound_rom, std::vector<uint16_t> sample_words, hle::PcmSink &sink)
        : rom(std::move(sound_rom)), samples(std::move(sample_words)), core(rom, samples) {
        core.set_pcm_sink(&sink);
    }
    void apply(const CoreInput &input) {
        const auto started = std::chrono::steady_clock::now();
        input.apply(core);
        const auto elapsed = std::chrono::steady_clock::now() - started;
        render_ns.fetch_add(
            uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(elapsed).count()),
            std::memory_order_relaxed);
        generated.store(core.frames(), std::memory_order_relaxed);
    }
};
} // namespace

struct Audio::Impl {
    PcmRing pcm;
    Synth synth;
    SharedMailbox mailbox;
    uint64_t clock = 0, command_count = 0, serial = 0, submitted_tick = 0;
    uint64_t clock_epoch = 0, playback_epoch = 0;
    uint32_t bank = 1;
    bool held = true;
    WorkerQueue<CoreInput, Synth, QUEUE_SIZE> worker;

    Impl(std::vector<uint8_t> sound_rom, std::vector<uint16_t> sample_words, uint8_t *shared)
        : synth(std::move(sound_rom), std::move(sample_words), pcm), mailbox(shared),
          worker(synth) {}

    uint64_t playback_now() const { return playback_epoch + (clock - clock_epoch); }
    void post(CoreInput::Kind kind) { worker.post({kind, playback_now()}); }
    void post_gain(unsigned channel) {
        post(CoreInput::Gain{uint8_t(channel), mailbox.gain(channel)});
    }
    void record(hle::CommandPacket packet) {
        ++command_count;
        const uint64_t instance = ++serial;
        if (serial >> 63) throw std::overflow_error("HLE instance IDs exhausted");
        post(CoreInput::Command{packet, instance});
    }
    void submit_clock() {
        if (clock <= submitted_tick) return;
        post(CoreInput::Advance{});
        submitted_tick = clock;
    }
    void flush() {
        submit_clock();
        worker.flush();
    }
    [[nodiscard]] AudioSnapshot snapshot() const {
        return {clock, command_count, mailbox.consumer(), held};
    }
    void restore(const AudioSnapshot &state) {
        flush();
        clock = clock_epoch = submitted_tick = state.clock;
        playback_epoch = synth.core.tick();
        pcm.discard();
        command_count = state.commands;
        mailbox.restore(state.consumer);
        held = state.held;
    }
};

std::vector<uint16_t> decode_sample_words(std::span<const uint8_t> sample_rom) {
    std::vector<uint16_t> words(sample_rom.size() / 2);
    for (size_t i = 0; i < words.size(); ++i)
        words[i] = uint16_t(uint16_t(sample_rom[i * 2]) << 8 | sample_rom[i * 2 + 1]);
    return words;
}
bool Audio::supported(std::span<const uint8_t> sound_rom,
                      std::span<const uint16_t> sample_words) noexcept {
    return hle::Core::validate(sound_rom, sample_words);
}
Audio::Audio(std::vector<uint8_t> sound_rom, std::vector<uint16_t> sample_words,
             uint8_t *shared_ram)
    : m_impl(std::make_unique<Impl>(std::move(sound_rom), std::move(sample_words), shared_ram)) {}
Audio::~Audio() = default;
void Audio::advance(uint32_t cycles) { m_impl->clock += cycles; }
uint64_t Audio::clock_ticks() const { return m_impl->clock; }
bool Audio::is_reset() const { return m_impl->held; }

void Audio::shared_write(uint32_t offset) {
    auto &p = *m_impl;
    if (p.held) return;
    if (offset == MAILBOX_PRODUCER_L)
        p.mailbox.drain([&p](hle::CommandPacket packet) { p.record(packet); });
    else if (offset == MAILBOX_GAIN_CH0 || offset == MAILBOX_GAIN_CH1) p.post_gain(offset & 1);
}
void Audio::set_reset(bool asserted) {
    auto &p = *m_impl;
    if (p.held == asserted) return;
    p.held = asserted;
    if (asserted) {
        p.mailbox.reset();
        p.post(CoreInput::Reset{});
    } else {
        p.post_gain(0);
        p.post_gain(1);
    }
}
void Audio::reset_board() { set_reset(true); }
void Audio::set_sound_bank(uint32_t page) {
    m_impl->bank = page;
    m_impl->post(CoreInput::Bank{uint8_t(page)});
}
uint32_t Audio::sound_bank() const { return m_impl->bank; }
uint32_t Audio::sample_rate() const { return hle::SAMPLE_RATE; }

void Audio::finish_frame() {
    auto &p = *m_impl;
    p.submit_clock();
    p.pcm.close_frame(hle::Core::frames_at(p.playback_now()));
}
size_t Audio::available_frames() const {
    m_impl->flush();
    return m_impl->pcm.available();
}
size_t Audio::render(float *out, size_t max_frames) {
    m_impl->flush();
    return m_impl->pcm.drain(out, max_frames);
}
size_t Audio::render(int16_t *out, size_t max_frames) {
    m_impl->flush();
    return m_impl->pcm.drain(out, max_frames);
}
size_t Audio::render_ready(int16_t *out, size_t max_frames) {
    return m_impl->pcm.drain_released(out, max_frames);
}

uint64_t Audio::generated_frames() const {
    return m_impl->synth.generated.load(std::memory_order_relaxed);
}
AudioPerformanceStats Audio::performance_stats() const {
    const auto render = m_impl->synth.core.render_stats();
    return {m_impl->command_count, generated_frames(), render.voices_started, render.voices_active,
            m_impl->synth.render_ns.load(std::memory_order_relaxed)};
}
#if F3RT_HLE_EVENTS
void Audio::set_voice_observer(std::function<void(const hle::VoiceEvent &)> observer) {
    m_impl->flush();
    m_impl->synth.core.set_voice_observer(std::move(observer));
}
#endif

size_t Audio::state_size() const { return AudioSnapshot::BYTES; }
void Audio::save_state(std::span<uint8_t> dst) const { m_impl->snapshot().write(dst); }
void Audio::load_state(std::span<const uint8_t> src) { m_impl->restore(AudioSnapshot::parse(src)); }
}
