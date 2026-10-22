#include "audio/hle/track_service.hpp"
#include <stdexcept>

namespace f3rt::hle {
namespace {

// Program-selector value ranges; the extended-selector bit lives on TrackParameters.
constexpr uint8_t SELECTOR_INTERNAL_MIN = 0x64;
constexpr uint8_t SELECTOR_INTERNAL_MAX = 0x7b;
constexpr uint8_t SELECTOR_SPECIAL_7B = 0x7b;
constexpr uint8_t SELECTOR_BANK_BASE = 0x7c;

} // namespace

TrackService::TrackService(Mailbox &mailbox) : m_mailbox(mailbox) {
    m_fade.add(*this, FADE_TAG);
}

void TrackService::reset() noexcept {
    m_tracks = {};
    m_active = {};
    m_mix = {};
    m_notes.reset();
    m_channel_used.fill(false);
    m_channel_previous_key.fill(0);
    m_fade.reset();
}

uint8_t TrackService::allocate_channel() noexcept {
    for (uint8_t channel = MAX_TRACK_CHANNEL; channel > 0; --channel) {
        if (!m_channel_used[channel]) {
            m_channel_used[channel] = true;
            return channel;
        }
    }
    return 0;
}

void TrackService::release_channel(uint8_t channel) noexcept {
    if (channel > 0 && channel <= MAX_TRACK_CHANNEL) m_channel_used[channel] = false;
}

void TrackService::start_sequence(uint8_t sequence,
                                  const std::array<TrackConfig, TRACK_COUNT> &initial,
                                  uint64_t tick) {
    auto &list = m_tracks[sequence];
    for (Track &t : list) m_notes.release_all(t.head, m_sink, tick);
    for (Track &t : list) {
        if (t.physical) release_channel(t.physical);
        t.physical = 0;
    }
    reset_mix(sequence);
    const uint8_t volume = uint8_t(m_mix[sequence].volume >> 8);
    for (size_t i = 0; i < TRACK_COUNT; ++i) {
        Track &t = list[i];
        t.config = initial[i];
        if (t.config.program != NO_PROGRAM) t.physical = allocate_channel();
        t.cursor = 0;
        t.countdown = 1;
        t.status = TrackStatus::Inactive;
        t.sequence_volume = volume;
    }
    m_active[sequence] = true;
}

void TrackService::stop_sequence(uint8_t sequence, uint64_t tick) {
    for (Track &t : m_tracks[sequence]) {
        m_notes.release_all(t.head, m_sink, tick);
        if (t.physical) release_channel(t.physical);
        t.physical = 0;
    }
    m_active[sequence] = false;
}

void TrackService::park_sequence(uint8_t sequence, uint64_t tick) {
    release_notes(sequence, tick);
}

void TrackService::release_notes(uint8_t sequence, uint64_t tick) {
    for (Track &t : m_tracks[sequence]) m_notes.release_all(t.head, m_sink, tick);
}

void TrackService::reset_mix(uint8_t sequence) noexcept { m_mix[sequence] = Mix{}; }

void TrackService::copy_mix(uint8_t from, uint8_t to) noexcept {
    m_mix[to] = m_mix[from];
    set_sequence_volume(to, uint8_t(m_mix[to].volume >> 8));
}

uint64_t TrackService::release_key(uint8_t sequence, uint8_t track, uint8_t kernel_key,
                                  [[maybe_unused]] uint64_t tick) {
    return m_notes.release_matching_key(m_tracks[sequence][track].head, kernel_key);
}

void TrackService::apply(const NoteOnEvent &event) {
    Track &t = m_tracks[event.sequence][event.track];
    m_notes.allocate_note(t.head, event.instance, event.key, event.kernel_key, event.duration);
}

void TrackService::apply(const TrackEvent &event) {
    Track &t = m_tracks[event.sequence][event.track];
    switch (event.kind) {
    case TrackEventKind::Controller:
        if (!t.config.parameters.set_controller(event.index, event.value))
            throw std::runtime_error("HLE controller outside track parameter record");
        break;

    case TrackEventKind::Program:
        t.config.program = event.program;
        t.config.parameters.set_program_image(event.program);
        break;

    case TrackEventKind::ProgramSelector: {
        TrackParameters &parameters = t.config.parameters;
        parameters.set_selector(event.value);
        if (event.value >= SELECTOR_INTERNAL_MIN && event.value < SELECTOR_INTERNAL_MAX) break;
        if (event.value == SELECTOR_SPECIAL_7B) {
            parameters.set_extended_selector(true);
            break;
        }
        if (event.value >= SELECTOR_BANK_BASE) {
            t.config.bank = uint8_t(event.value - SELECTOR_BANK_BASE);
            break;
        }
        parameters.set_extended_selector(false);
        t.config.program = uint16_t((uint16_t(t.config.bank) << 14) | event.value);
        parameters.set_program_image(t.config.program);
        // c144e2 passes the track with bit7 set: c0edda clears the bank after selection.
        t.config.bank = 0;
        break;
    }

    case TrackEventKind::Volume:
        t.config.parameters.set_volume(event.value);
        break;

    case TrackEventKind::Pan:
        t.config.parameters.set_pan(event.value);
        break;

    case TrackEventKind::PreviousKey:
        t.config.parameters.set_previous_key(event.value);
        m_channel_previous_key[t.physical] = event.value;
        break;

    case TrackEventKind::SequenceVolume: {
        Mix &mix = m_mix[event.sequence];
        mix.volume = uint16_t((uint16_t(event.value) << 8) | uint8_t(mix.volume));
        mix.fade_step = 0;
        set_sequence_volume(event.sequence, uint8_t(mix.volume >> 8));
        break;
    }

    case TrackEventKind::SequencePan:
        for (Track &live : m_tracks[event.sequence])
            if (live.is_live()) live.config.parameters.set_pan(event.value);
        break;

    case TrackEventKind::SequenceFade: {
        Mix &mix = m_mix[event.sequence];
        mix.volume = uint16_t((uint16_t(event.start) << 8) | uint8_t(mix.volume));
        mix.fade_target = event.target;
        const auto difference = int16_t(uint16_t(uint8_t(event.target - event.start)) << 8);
        const int32_t product = int32_t(difference) * event.rate;
        mix.fade_step = int16_t((uint32_t(product) * 2) >> 16);
        set_sequence_volume(event.sequence, uint8_t(mix.volume >> 8));
        break;
    }

    case TrackEventKind::Refresh:
        break;

    case TrackEventKind::SequenceFadeStep: {
        Mix &mix = m_mix[event.sequence];
        if (!mix.fade_step) break;
        mix.volume = uint16_t(mix.volume + mix.fade_step);
        const auto current = int8_t(mix.volume >> 8);
        const auto target = int8_t(mix.fade_target);
        if ((mix.fade_step > 0 && current >= target) || (mix.fade_step < 0 && current < target)) {
            mix.volume = uint16_t((uint16_t(mix.fade_target) << 8) | uint8_t(mix.volume));
            mix.fade_step = 0;
        }
        set_sequence_volume(event.sequence, uint8_t(mix.volume >> 8));
        break;
    }
    }
}

void TrackService::pulse(uint64_t tick, uint8_t tag) {
    if (tag < SONG_SLOTS) {
        note_pulse(uint8_t(tag), tick);
        return;
    }
    if (tag == FADE_TAG) fade_pulse(tick);
}

void TrackService::note_pulse(uint8_t sequence, uint64_t tick) {
    for (Track &t : m_tracks[sequence]) m_notes.step_durations(t.head, m_sink, tick);
}

void TrackService::fade_pulse(uint64_t tick) {
    for (size_t sequence = 0; sequence < SONG_SLOTS; ++sequence) {
        if (!m_active[sequence] || !m_mix[sequence].fade_step) continue;
        m_mailbox.push(TrackEvent{TrackEventKind::SequenceFadeStep, tick, uint8_t(sequence)});
    }
}

void TrackService::set_sequence_volume(uint8_t sequence, uint8_t volume) noexcept {
    for (Track &t : m_tracks[sequence])
        if (t.is_live()) t.sequence_volume = volume;
}

} // namespace f3rt::hle
