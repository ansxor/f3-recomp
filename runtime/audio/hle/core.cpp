#include "audio/hle/core.hpp"
#include "audio/hle/effects.hpp"
#include "audio/hle/sequencer.hpp"
#include "audio/hle/synth.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#if (defined(__x86_64__) || defined(_M_X64) || defined(__SSE__)) && !defined(__EMSCRIPTEN__)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

namespace f3rt::hle {

struct Core::Impl : VoiceSink {
    Synth synth;
    Effects effects;
    Sequencer sequencer;
    uint64_t worker_tick = 0, worker_frames = 0;
    std::array<float, 2> gain{1.f, 1.f};
    GainModel gain_model = GainModel::SingleStage;
    PcmSink *sink = nullptr;
    TraceTap *trace = nullptr;
    std::function<void(const VoiceEvent &)> voice_observer;

    void update_synth_observer() {
        if (!voice_observer && !trace) {
            synth.set_observer(nullptr);
        } else {
            synth.set_observer([this](const VoiceEvent &e) {
                if (voice_observer) voice_observer(e);
                if (trace) trace->voice(e);
            });
        }
    }

    Impl(std::span<const uint8_t> rom, std::span<const uint16_t> samples)
        : synth(rom, samples), effects(rom), sequencer(rom, *this) {}

    void note_on(const Note &n) override {
        synth.note_on(n);
        if (trace) trace->note_on(n);
    }
    void note_off(uint64_t id, uint64_t tick) override {
        synth.note_off(id, tick);
        if (trace) trace->note_off(id, tick);
    }
    void channel_update(uint8_t s, uint8_t t, const Channel &c, uint64_t tick) override {
        synth.channel_update(s, t, c, tick);
        if (trace) trace->channel_update(s, t, c, tick);
    }
    void effect(uint8_t preset, uint64_t tick) override {
        effects.select(preset);
        if (trace) trace->effect(preset, tick);
    }
    void effect_parameter(uint8_t index, uint8_t value, uint64_t tick) override {
        effects.parameter(index, value);
        if (trace) trace->effect_parameter(index, value, tick);
    }
    void reset(uint64_t tick) override {
        synth.reset(tick);
        effects.reset();
        gain.fill(1.f);
        if (trace) trace->reset(tick);
    }

    void render_until(uint64_t tick) {
        const uint64_t end = frames_at(tick);
        std::array<float, size_t(BLOCK_FRAMES) * 8> buses;
        std::array<float, size_t(BLOCK_FRAMES) * 2> stereo;
        std::array<float, 2> input, output;
        for (unsigned ch = 0; ch < 2; ++ch) {
            const float route = float(std::lround(gain[ch] * 100.f)) / 32.f;
            input[ch] = .18f * (gain_model == GainModel::MameRouting ? route : 1.f);
            output[ch] = OUTPUT_BOOST * (gain_model == GainModel::MameRouting ? route : gain[ch]);
        }
        while (worker_frames < end) {
            const size_t n = size_t(std::min<uint64_t>(BLOCK_FRAMES, end - worker_frames));
            synth.render(buses.data(), n);
            effects.mix(buses.data(), stereo.data(), n, input, output);
            if (trace) trace->mix_block(worker_frames, buses.data(), stereo.data(), n, input, output);
            if (sink) sink->write(stereo.data(), n);
            worker_frames += n;
        }
    }
    void advance_worker(uint64_t tick) {
        while (worker_tick < tick) {
            const uint64_t next = std::min(tick, sequencer.next_tick());
            render_until(next);
            sequencer.advance_to(next);
            worker_tick = next;
        }
    }
    static uint64_t frames_at(uint64_t tick) {
        return tick / MAIN_CLOCK * SAMPLE_RATE + tick % MAIN_CLOCK * SAMPLE_RATE / MAIN_CLOCK;
    }
};

Core::Core(std::span<const uint8_t> sound_rom, std::span<const uint16_t> sample_words)
    : m_impl(std::make_unique<Impl>(sound_rom, sample_words)) {}
Core::~Core() = default;

void Core::set_pcm_sink(PcmSink *sink) { m_impl->sink = sink; }
void Core::set_gain_model(GainModel model) { m_impl->gain_model = model; }
void Core::set_voice_observer(std::function<void(const VoiceEvent &)> observer) {
    m_impl->voice_observer = std::move(observer);
    m_impl->update_synth_observer();
}
void Core::set_trace(TraceTap *tap) {
    m_impl->trace = tap;
    m_impl->sequencer.set_observer(tap);
    m_impl->update_synth_observer();
}

EffectsInfo Core::effects_info() const { return m_impl->effects.info(); }
std::optional<ProgramInfo> Core::describe_program(uint16_t program) const {
    return m_impl->synth.describe_program(program);
}
std::optional<SequenceInfo> Core::describe_sequence(uint8_t id) const {
    return m_impl->sequencer.describe(id);
}
bool Core::sequences_running() const { return m_impl->sequencer.sequences_running(); }
uint8_t Core::bend_range() const { return m_impl->synth.bend_range(); }

uint64_t Core::tick() const { return m_impl->worker_tick; }
uint64_t Core::frames() const { return m_impl->worker_frames; }
uint64_t Core::next_boundary() const { return m_impl->sequencer.next_tick(); }
uint64_t Core::frames_at(uint64_t tick) { return Impl::frames_at(tick); }

void Core::advance(uint64_t tick) { m_impl->advance_worker(std::max(m_impl->worker_tick, tick)); }
void Core::command(std::span<const uint8_t> packet, uint64_t instance, uint64_t tick) {
    advance(tick);
    m_impl->sequencer.command(packet, instance, m_impl->worker_tick);
    if (m_impl->trace) m_impl->trace->command(packet, instance, m_impl->worker_tick);
}
void Core::reset(uint64_t tick) {
    advance(tick);
    m_impl->sequencer.reset(m_impl->worker_tick);
    m_impl->synth.set_bank(1); // the sound CPU's cold-boot bank, as in sequencer.reset
}
void Core::set_bank(uint8_t page, uint64_t tick) {
    advance(tick);
    m_impl->sequencer.set_bank(page, m_impl->worker_tick);
    m_impl->synth.set_bank(page);
}
void Core::set_gain(unsigned channel, uint8_t raw, uint64_t tick) {
    advance(tick);
    m_impl->gain[channel & 1] = std::pow(10.f, -.5f * float(63 - (raw & 63)) / 20.f);
    if (m_impl->trace) m_impl->trace->gain(channel, raw, m_impl->gain[channel & 1], m_impl->worker_tick);
}

// Flush-to-zero and denormals-are-zero prevent decaying reverb/filter tails from becoming subnormal.
void Core::enable_flush_denormals() {
#if (defined(__x86_64__) || defined(_M_X64) || defined(__SSE__)) && !defined(__EMSCRIPTEN__)
    _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON);
    _MM_SET_DENORMALS_ZERO_MODE(_MM_DENORMALS_ZERO_ON);
#elif defined(__aarch64__)
    uint64_t fpcr;
    __asm__ __volatile__("mrs %0, fpcr" : "=r"(fpcr));
    fpcr |= (1ULL << 24);
    __asm__ __volatile__("msr fpcr, %0" : : "r"(fpcr));
#endif
}

} // namespace f3rt::hle
