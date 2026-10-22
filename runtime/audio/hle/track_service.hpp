#pragma once
#include "audio/hle/track_parameters.hpp"
#include "audio/hle/events.hpp"
#include "audio/hle/mailbox.hpp"
#include "audio/hle/note_pool.hpp"
#include "audio/hle/pulse_timer.hpp"
#include "audio/hle/voice_pool.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace f3rt::hle {

// The parameters a sequence header supplies for one track: the 0x38-byte record the firmware keeps per track, the
// selected program id and the program-selector bank.
struct TrackConfig {
    TrackParameters parameters{};
    uint16_t program = NO_PROGRAM;
    uint8_t bank = 0;
};

enum class TrackStatus : uint8_t {
    Inactive,  // unallocated, or no stream / no program to play
    Streaming, // reading events from the ROM track stream
    Ended,     // reached the end of the stream; the track stays live for direct commands
};

// The live sequencing state of one track of one sequence.
struct Track {
    TrackConfig config{};
    uint8_t sequence_volume = 127;
    size_t cursor = 0;
    uint32_t countdown = 1;
    uint16_t head = NO_NOTE;
    uint8_t physical = 0;
    TrackStatus status = TrackStatus::Inactive;

    bool is_live() const noexcept { return status != TrackStatus::Inactive; }
    bool is_streaming() const noexcept { return status == TrackStatus::Streaming; }
};

// Pushes the note-off into the mailbox; the dispatcher releases the voice.
struct MailboxNoteSink {
    Mailbox &mailbox;
    void note_off(uint64_t instance, uint64_t tick) { mailbox.push(NoteOffEvent{instance, tick}); }
};

// Owns everything that is sequencing state rather than sequencer position: the per-track parameter records, the note
// durations and the per-sequence volume / fade mix. Parameters are only written through apply(TrackEvent); start,
// stop and park are lifecycle calls from the Sequencer.
class TrackService : public PulseTimer::Listener {
public:
    explicit TrackService(Mailbox &mailbox);

    void reset() noexcept;

    Track &track(uint8_t sequence, uint8_t track) noexcept { return m_tracks[sequence][track]; }
    std::array<Track, TRACK_COUNT> &tracks(uint8_t sequence) noexcept { return m_tracks[sequence]; }

    // ---- lifecycle, called synchronously by the Sequencer before it dispatches a pulse's events.
    void start_sequence(uint8_t sequence, const std::array<TrackConfig, TRACK_COUNT> &initial,
                        uint64_t tick);
    void stop_sequence(uint8_t sequence, uint64_t tick);
    void park_sequence(uint8_t sequence, uint64_t tick);
    // Note-offs only: the physical channels and the track records stay.
    void release_notes(uint8_t sequence, uint64_t tick);
    void copy_mix(uint8_t from, uint8_t to) noexcept;
    uint8_t previous_key(uint8_t physical) const noexcept { return m_physical.previous_key(physical); }

    // ---- event application, called only by the mailbox dispatcher.
    void apply(const NoteOnEvent &event);
    void apply(const TrackEvent &event);
    // Unlinks the track's note of `kernel_key`; returns the instance to release (0 when none).
    uint64_t release_key(uint8_t sequence, uint8_t track, uint8_t kernel_key, uint64_t tick);

    // PulseTimer::Listener: tag < SONG_SLOTS is that sequence's BPM note pulse, FADE_TAG the sequence fade pulse.
    void pulse(uint64_t tick, uint8_t tag) override;
    PulseTimer &fade_timer() noexcept { return m_fade; }

private:
    static constexpr uint8_t FADE_TAG = 0xff;

    struct Mix {
        uint16_t volume = 0x7fff;
        int16_t fade_step = 0;
        uint8_t fade_target = 127;
    };

    void reset_mix(uint8_t sequence) noexcept;
    void note_pulse(uint8_t sequence, uint64_t tick);
    void fade_pulse(uint64_t tick);
    void set_sequence_volume(uint8_t sequence, uint8_t volume) noexcept;

    Mailbox &m_mailbox;
    MailboxNoteSink m_sink{m_mailbox};
    std::array<std::array<Track, TRACK_COUNT>, SONG_SLOTS> m_tracks{};
    std::array<bool, SONG_SLOTS> m_active{};
    std::array<Mix, SONG_SLOTS> m_mix{};
    NotePool m_notes{};
    PhysicalVoicePool m_physical{};
    PulseTimer m_fade{12, 1};
};

} // namespace f3rt::hle
