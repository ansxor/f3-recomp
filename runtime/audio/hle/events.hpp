#pragma once
#include <array>
#include <cstdint>

namespace f3rt::hle {
// The voice and effects stages run on the ES5505 output grid (15238090 / (16 * 32) = 29761 Hz) and the
// engine outputs at that rate.
inline constexpr uint32_t SAMPLE_RATE = 29761;
inline constexpr uint32_t MAIN_CLOCK = 16000000;
inline constexpr uint32_t TIMER_TICKS = 16000; // ROM sequencer's observed 1 kHz service.
// Sound CPU cycles (16 MHz) the real driver spends per started voice while a sequencer pulse starts several
// notes; see DriverClock in note_timing.hpp and docs/notes/raw/tests/2026-10-16-kirameki-187-clap-dynamics.md.
inline constexpr uint32_t DRIVER_KEYON_CYCLES = 20408;

// Parameter state, not an emulated channel RAM object. Offsets retain the
// documented firmware format while unknown musical names remain unasserted.
struct Channel {
    std::array<uint8_t, 0x38> parameters{};
    uint16_t program = 0;
    uint8_t bank = 0;
    uint8_t sequence_volume = 127;
};

struct Note {
    uint64_t instance = 0;
    uint64_t tick = 0;
    uint8_t sequence = 0;
    uint8_t track = 0;
    uint8_t key = 0;        // Original mailbox key / sequence event class.
    uint8_t kernel_key = 0; // Factory +21 and arrangement transpose, exactly once.
    uint8_t velocity = 0;
    Channel channel;
};

// Sequencer events have no dependency on a sound CPU, chip registers or SDL.
// Instance IDs distinguish equal-key retriggers.
class VoiceSink {
public:
    virtual ~VoiceSink() = default;
    virtual void note_on(const Note &note) = 0;
    virtual void note_off(uint64_t instance, uint64_t tick) = 0;
    virtual void channel_update(uint8_t sequence, uint8_t track, const Channel &channel,
                                uint64_t tick) = 0;
    virtual void effect(uint8_t preset, uint64_t tick) = 0;
    virtual void effect_parameter(uint8_t index, uint8_t value, uint64_t tick) = 0;
    virtual void reset(uint64_t tick) = 0;
};

struct VoiceEvent {
    enum class Kind : uint8_t { Start, Release, Stop, Parameters };
    Kind kind = Kind::Start;
    uint64_t instance = 0, tick = 0;
    uint8_t sequence = 0, track = 0, key = 0, layer = 0, output_pair = 0;
    uint32_t sample_start = 0, sample_end = 0;
    uint16_t frequency = 0, left_volume = 0, right_volume = 0, k1 = 0, k2 = 0;
    bool loop = false, reverse = false;
};
} // namespace f3rt::hle
