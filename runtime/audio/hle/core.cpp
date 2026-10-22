#include "audio/hle/core.hpp"
#include "audio/hle/clock.hpp"
#include "audio/hle/effects.hpp"
#include "audio/hle/mailbox.hpp"
#include "audio/hle/program_service.hpp"
#include "audio/hle/sequencer.hpp"
#include "audio/hle/sound_bank.hpp"
#include "audio/hle/synthesis.hpp"
#include "audio/hle/track_service.hpp"
#include "audio/hle/voice_service.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <variant>
#if (defined(__x86_64__) || defined(_M_X64) || defined(__SSE__)) && !defined(__EMSCRIPTEN__)
#include <xmmintrin.h>
#include <pmmintrin.h>
#endif

namespace f3rt::hle {
using std::make_unique;

namespace {
template <class... F> struct Overloaded : F... {
    using F::operator()...;
};
template <class... F> Overloaded(F...) -> Overloaded<F...>;
} // namespace

// The composition root: it owns every service, wires the 1 kHz clock, dispatches the mailbox and renders. It is a
// clock listener itself so that a tick's events reach the services after the sequencer's pulses and the fade pulse
// and before the render reaches the tick; the voice ring is stepped from the render at the first frame that gets
// there (VoiceService::service), as it always has been.
struct Core::Impl : Clock::Listener {
    SoundBank bank;
    Effects effects;
    Clock clock;
    Mailbox mailbox;
    ProgramService programs{bank};
    TrackService tracks{mailbox};
    VoiceService voices{bank};
    Sequencer sequencer;
    VoiceRenderState render_state;
    uint64_t worker_tick = 0, worker_frames = 0;
    std::array<float, 2> gain{1.f, 1.f};
    PcmSink *sink = nullptr;

    Impl(std::span<const uint8_t> rom, std::span<const uint16_t> samples)
        : bank(rom, samples), effects(rom), sequencer(rom, mailbox, tracks) {
        // Tempo pulses, then the fade pulse, then the events they queued.
        for (size_t i = 0; i < SONG_SLOTS; ++i) {
            sequencer.beats()[i].add(tracks, uint8_t(i));
            sequencer.beats()[i].add(sequencer, uint8_t(i));
            clock.add(sequencer.beats()[i]);
        }
        clock.add(tracks.fade_timer());
        clock.add(*this);
        sequencer.reset(0);
        voices.reset(0);
        tracks.reset();
        reset_render_state(render_state, 0);
        clock.reset(0);
    }

    void clock_tick(uint64_t) override { dispatch_mailbox(); }

    // Applies one batch of events in arrival order: notes allocate, track events rewrite parameters, effects apply.
    void dispatch_mailbox() {
        mailbox.drain([this](const Event &event) {
            std::visit(
                Overloaded{
                    [this](const NoteOnEvent &n) {
                        Track &t = tracks.track(n.sequence, n.track);
                        tracks.apply(n);
                        NoteOnEvent note = n;
                        // A ProgramSelector earlier in this batch is already applied: the note resolves with the
                        // program in force at this point of the event order.
                        note.program = t.config.program;
                        const ProgramQuery query{
                            note.program, t.config.parameters.transposed_key(note.kernel_key),
                            t.config.parameters.layer_mask(),
                            t.config.parameters.notes_suppressed()};
                        const PatchList patches = programs.resolve(query);
                        if (patches.count) voices.note_on(note, patches, t);
                    },
                    [this](const NoteOffEvent &n) { voices.note_off(n.instance, n.tick); },
                    [this](const ReleaseKeyEvent &r) {
                        if (const uint64_t instance =
                                tracks.release_key(r.sequence, r.track, r.kernel_key, r.tick))
                            voices.note_off(instance, r.tick);
                    },
                    [this](const TrackEvent &e) {
                        tracks.apply(e);
                        switch (e.kind) {
                        case TrackEventKind::Controller:
                        case TrackEventKind::Program:
                        case TrackEventKind::ProgramSelector:
                        case TrackEventKind::Volume:
                        case TrackEventKind::Pan:
                        case TrackEventKind::Refresh:
                            voices.refresh_track(e.sequence, e.track, e.tick);
                            break;
                        case TrackEventKind::SequenceVolume:
                        case TrackEventKind::SequencePan:
                        case TrackEventKind::SequenceFade:
                        case TrackEventKind::SequenceFadeStep:
                            voices.refresh_sequence(e.sequence, e.tick);
                            break;
                        case TrackEventKind::PreviousKey:
                            break;
                        }
                    },
                    [this](const EffectEvent &e) {
                        if (e.kind == EffectEvent::Kind::Preset) effects.select(e.preset);
                        else effects.parameter(e.index, e.value);
                    },
                },
                event);
        });
    }

    void render_until(uint64_t tick) {
        const uint64_t end = frames_at(tick);
        BusBlock buses;
        StereoBlock stereo;
        std::array<float, 2> input, output;
        for (unsigned ch = 0; ch < 2; ++ch) {
            input[ch] = .18f;
            output[ch] = OUTPUT_BOOST * gain[ch];
        }
        auto &state = render_state;
        while (worker_frames < end) {
            const size_t n = size_t(std::min<uint64_t>(BLOCK_FRAMES, end - worker_frames));
            if (state.frame_ticks.size() < n) state.frame_ticks.resize(n);
            uint64_t frame_tick = state.tick;
            uint32_t remainder = state.tick_remainder;
            for (size_t f = 0; f < n; ++f) {
                state.frame_ticks[f] = frame_tick;
                remainder += MAIN_CLOCK;
                frame_tick += remainder / SAMPLE_RATE;
                remainder %= SAMPLE_RATE;
            }
            buses.resize(n);
            size_t frame_start = 0;
            while (frame_start < n) {
                const uint64_t now = state.frame_ticks[frame_start];
                // The voice ring and every queued driver start due by this frame run before it is rendered.
                for (;;) {
                    const uint64_t timer = voices.next_tick();
                    const bool timer_due = timer <= now;
                    const uint64_t queued = voices.next_queue_tick();
                    const bool queue_due = queued <= now;
                    if (!timer_due && !queue_due) break;
                    if (queue_due && (!timer_due || queued <= timer))
                        voices.service_queue(timer_due ? timer : now);
                    else
                        voices.service();
                }
                size_t frame_end = frame_start + 1;
                const uint64_t horizon = std::min(voices.next_tick(), voices.next_queue_tick());
                while (frame_end < n && state.frame_ticks[frame_end] < horizon) ++frame_end;
                std::fill_n(buses.data() + frame_start * 8, (frame_end - frame_start) * 8, 0.f);
                // Render-time voice events are delivered in (frame, voice) order once this span is done.
                voices.begin_render();
                render_voices(voices, bank, state, buses.span(), frame_start, frame_end,
                              state.frame_ticks);
                voices.finish_render();
                float *sub_out = buses.data() + frame_start * 8;
                const size_t sub_floats = (frame_end - frame_start) * 8;
                for (size_t i = 0; i < sub_floats; ++i)
                    sub_out[i] = std::clamp(sub_out[i], -1.f, 1.f - 1.f / 524288.f);
                frame_start = frame_end;
            }
            state.tick = frame_tick;
            state.tick_remainder = remainder;
            stereo.resize(n);
            effects.mix(buses, stereo, input, output);
            if (sink) sink->write(stereo.data(), n);
            worker_frames += n;
        }
    }

    void advance_worker(uint64_t tick) {
        while (worker_tick < tick) {
            const uint64_t next = std::min(tick, clock.next_tick());
            render_until(next);
            while (clock.next_tick() <= next) clock.advance_one();
            worker_tick = next;
        }
    }

    static uint64_t frames_at(uint64_t tick) {
        return tick / MAIN_CLOCK * SAMPLE_RATE + tick % MAIN_CLOCK * SAMPLE_RATE / MAIN_CLOCK;
    }
};

Core::Core(std::span<const uint8_t> sound_rom, std::span<const uint16_t> sample_words)
    : m_impl(make_unique<Impl>(sound_rom, sample_words)) {}
Core::~Core() = default;

void Core::set_pcm_sink(PcmSink *sink) { m_impl->sink = sink; }
#if F3RT_HLE_EVENTS
void Core::set_voice_observer(std::function<void(const VoiceEvent &)> observer) {
    m_impl->voices.set_observer(std::move(observer));
}
#endif

uint64_t Core::tick() const { return m_impl->worker_tick; }
uint64_t Core::frames() const { return m_impl->worker_frames; }
uint64_t Core::frames_at(uint64_t tick) { return Impl::frames_at(tick); }

void Core::advance(uint64_t tick) { m_impl->advance_worker(std::max(m_impl->worker_tick, tick)); }
void Core::command(const CommandPacket &packet, uint64_t instance, uint64_t tick) {
    advance(tick);
    m_impl->sequencer.command(packet, instance, m_impl->worker_tick);
    m_impl->dispatch_mailbox();
}
void Core::reset(uint64_t tick) {
    advance(tick);
    m_impl->sequencer.reset(m_impl->worker_tick);
    m_impl->mailbox.clear();
    // Voices first: their Stop events still see the tracks they point at.
    m_impl->voices.reset(m_impl->worker_tick);
    m_impl->tracks.reset();
    reset_render_state(m_impl->render_state, m_impl->worker_tick);
    m_impl->clock.reset(m_impl->worker_tick);
    m_impl->effects.reset();
    m_impl->gain.fill(1.f);
    m_impl->bank.set_bank(1); // the sound CPU's cold-boot bank, as in sequencer.reset
}
void Core::set_bank(uint8_t page, uint64_t tick) {
    advance(tick);
    m_impl->bank.set_bank(page);
    m_impl->sequencer.set_bank(page, m_impl->worker_tick);
}
void Core::set_gain(unsigned channel, uint8_t raw, uint64_t tick) {
    advance(tick);
    m_impl->gain[channel & 1] = std::pow(10.f, -.5f * float(63 - (raw & 63)) / 20.f);
}

bool Core::validate(std::span<const uint8_t> sound_rom,
                    std::span<const uint16_t> sample_words) noexcept {
    try {
        Core core(sound_rom, sample_words);
        return true;
    } catch (...) {
        return false;
    }
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
