#pragma once
#include <array>
#include <cstddef>
#include <cstdint>
#include <variant>

namespace f3rt::hle {
// The voice and effects stages run on the ES5505 output grid (clock rate 15238090 Hz, 32 voices:
// 15238090 / (16 * 32) = 29761 + 229/256 = 29761.89453125 Hz, rounded to 29762 Hz per 12's
// PCB observation over MAME's integer-truncation bug) and the engine outputs at that rate.
inline constexpr uint32_t SAMPLE_RATE = 29762;
inline constexpr uint32_t MAIN_CLOCK = 16000000;
inline constexpr uint32_t TIMER_TICKS = 16000; // ROM sequencer's observed 1 kHz service.
// Sound CPU cycles (16 MHz) the real driver spends per started voice while a sequencer pulse starts several
// notes; see DriverClock in note_timing.hpp and docs/notes/raw/tests/2026-10-16-kirameki-187-clap-dynamics.md.
inline constexpr uint32_t DRIVER_KEYON_CYCLES = 20408;

// Sequence slots the ROM can start, and the logical tracks each sequence holds.
inline constexpr size_t SONG_SLOTS = 100;
inline constexpr size_t TRACK_COUNT = 8;
inline constexpr uint16_t NO_PROGRAM = 0xffff;

struct NoteOnEvent {
    uint64_t instance = 0, tick = 0;
    uint16_t duration = 0, program = 0;
    uint8_t sequence = 0, track = 0; // track is 0-based
    uint8_t key = 0, kernel_key = 0, velocity = 0;
    uint8_t previous_key = 0; // TrackParameters.previous_key snapshot before this note
};

struct NoteOffEvent {
    uint64_t instance = 0, tick = 0;
};

struct ReleaseKeyEvent {
    uint64_t tick = 0;
    uint8_t sequence = 0, track = 0, kernel_key = 0;
};

enum class TrackEventKind : uint8_t {
    Controller,
    Program,
    ProgramSelector,
    Volume,
    Pan,
    PreviousKey,
    SequenceVolume,
    SequencePan,
    SequenceFade,
    SequenceFadeStep,
    Refresh // the track record was rewritten as a whole: re-read it for live voices, no field changes
};

// One parameter write on a track, or one mix change on a sequence. Applied by TrackService in arrival order.
struct TrackEvent {
    TrackEventKind kind = TrackEventKind::Controller;
    uint64_t tick = 0;
    uint8_t sequence = 0, track = 0;
    uint8_t index = 0, value = 0; // Controller / Volume / Pan / PreviousKey
    uint16_t program = 0;         // Program
    uint8_t start = 0, target = 0; // SequenceFade
    int16_t rate = 0;              // SequenceFade rate multiplier (RATE_TABLE)
};

struct EffectEvent {
    enum class Kind : uint8_t { Preset, Parameter } kind = Kind::Preset;
    uint64_t tick = 0;
    uint8_t preset = 0, index = 0, value = 0;
};

using Event = std::variant<NoteOnEvent, NoteOffEvent, ReleaseKeyEvent, TrackEvent, EffectEvent>;

struct VoiceEvent {
    enum class Kind : uint8_t { Start, Release, Stop, Parameters };
    Kind kind = Kind::Start;
    uint64_t instance = 0, tick = 0;
    uint8_t sequence = 0, track = 0, key = 0, layer = 0, output_pair = 0;
    uint32_t sample_start = 0, sample_end = 0;
    uint16_t frequency = 0, left_volume = 0, right_volume = 0, k1 = 0, k2 = 0;
    bool loop = false, reverse = false;

    // Populated by VoiceService::emit:
    uint8_t slot = 0;             // ES5505 voice slot index 0..31
    uint8_t sounding_key = 0;     // Voice.effective_key 21..108 (after key transpose)
    uint8_t velocity = 0;         // Note velocity
    uint8_t filter_mode = 0;      // Voice.filter.mode 0..3
    bool direct = false;          // Direct/key-region voice vs layered voice
    uint16_t program = 0;         // Track program id (bits 14..15 bank, low byte program)
    int16_t pitch = 0;            // Current voice pitch (1/256 semitone)
    int16_t glide = 0;            // Glide target pitch
    int16_t lfo = 0;              // LFO output
    std::array<int16_t, 3> env{}; // Envelope generator levels 0..2 (pitch, filter, amplitude)
};
} // namespace f3rt::hle
