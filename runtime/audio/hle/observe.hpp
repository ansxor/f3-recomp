#pragma once
// Read-only observation API of the HLE audio engine for the native web-player exporter (tools/webplayer/README.md,
// section 4.6 and 4.7). Everything here is a data type or an interface; the engine only calls a tap through a
// null-checked pointer, never per sample, and nothing a tap or a query does changes the rendered audio.
//
// Milestone status: these declarations are the final API. Core::set_trace(), Core::effects_info(),
// Core::describe_program(), Core::describe_sequence(), Core::sequences_running() and Core::bend_range() are declared
// in core.hpp and are *not defined yet* (calling them is a link error, not a silent no-op); their implementation, the
// extra VoiceEvent fields and the sequencer/synth/effects hooks they need arrive in milestone 2.
#include "audio/hle/events.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

namespace f3rt::hle {

// ---- Sequencer observation ---------------------------------------------------------------------------------------

// One play of one sequence slot: an ordinary start, an arrangement child, a bar-boundary repeat or a 0x85 seek.
struct SectionInfo {
    uint8_t owner = 0;           // arrangement owner slot; == sequence when the sequence is not an arrangement child
    uint8_t sequence = 0;        // the slot whose tracks play
    uint8_t phase = 0;           // owner's 1-based arrangement entry (track 10 list), 0 when not arranged
    uint16_t repeat_index = 0;   // 0 for a fresh start, +1 for each bar-boundary repeat of the same section
    uint16_t bars = 0, ticks_per_bar = 0;
    uint8_t numerator = 0, exponent = 0; // time signature numerator and log2(denominator)
    uint16_t rate = 0;           // pulse rate: pulses per 625 timer ticks per rate unit (quarter-note BPM, hypothesis)
    int16_t transpose = 0;       // arrangement transpose applied to the tracks selected by transpose_mask
    uint16_t transpose_mask = 0;
};
enum class SectionEnd : uint8_t { Boundary, Repeat, Stopped, Finished };

class SequenceObserver {
public:
    virtual ~SequenceObserver() = default;
    virtual void section_start(const SectionInfo & /*section*/, uint64_t /*tick*/) {}
    virtual void section_end(uint8_t /*sequence*/, SectionEnd /*how*/, uint64_t /*tick*/) {}
    // position % ticks_per_bar == 0, including bar 0 of every section.
    virtual void bar(uint8_t /*sequence*/, uint32_t /*bar_index*/, uint64_t /*tick*/) {}
    // Mailbox 0x88 (new rate), 0x83 (rate 0, paused) and 0x84 (saved rate restored).
    virtual void rate_change(uint8_t /*sequence*/, uint16_t /*rate*/, uint64_t /*tick*/) {}
    // The arrangement terminator was reached. resume_phase = entry following the last loop marker (1-based), or 0 when
    // the list has no marker and the song ends.
    virtual void arrangement_wrap(uint8_t /*owner*/, uint8_t /*resume_phase*/, uint64_t /*tick*/) {}
};

// ---- Core tap ------------------------------------------------------------------------------------------------------

// Installed with Core::set_trace(). Every method defaults to a no-op. Sink callbacks arrive after the engine handled
// them (so Core::effects_info() inside effect() already shows the new preset). Ticks are worker ticks.
class TraceTap : public SequenceObserver {
public:
    virtual void note_on(const Note & /*note*/) {}
    virtual void note_off(uint64_t /*instance*/, uint64_t /*tick*/) {}
    virtual void channel_update(uint8_t /*sequence*/, uint8_t /*track*/, const Channel & /*channel*/,
                                uint64_t /*tick*/) {}
    virtual void effect(uint8_t /*preset*/, uint64_t /*tick*/) {}
    virtual void effect_parameter(uint8_t /*index*/, uint8_t /*value*/, uint64_t /*tick*/) {}
    virtual void reset(uint64_t /*tick*/) {}
    virtual void voice(const VoiceEvent & /*event*/) {}
    virtual void command(std::span<const uint8_t> /*packet*/, uint64_t /*instance*/, uint64_t /*tick*/) {}
    virtual void gain(unsigned /*channel*/, uint8_t /*raw*/, float /*linear*/, uint64_t /*tick*/) {}
    // One <= 256-frame block after the effects stage: `buses8` = 8 floats per frame (four stereo pairs), `stereo` = the
    // mixed output (OTIS/ESP order, before the final left/right exchange). Pointers are valid during the call only.
    virtual void mix_block(uint64_t /*first_frame*/, const float * /*buses8*/, const float * /*stereo*/,
                           size_t /*frames*/, const std::array<float, 2> & /*input_gain*/,
                           const std::array<float, 2> & /*output_gain*/) {}
};

// ---- Const queries -------------------------------------------------------------------------------------------------

struct SequenceTrackInfo {
    uint8_t track = 0;             // logical track 1..8
    uint32_t events = 0;           // decoded events in its stream
    uint16_t initial_program = 0xffff;
};
struct SequenceInfo {
    uint8_t id = 0;
    bool arranged = false;         // track 10 starts with an 0xe7 arrangement entry
    uint32_t arrangement_entries = 0;
    uint8_t flags = 0, rate = 0, linked = 0, numerator = 0, exponent = 0;
    uint16_t bars = 0, ticks_per_bar = 0;
    uint32_t header_loop_start = 0, header_loop_end = 0; // parsed from the header, never used by the HLE
    std::vector<SequenceTrackInfo> tracks;
};

enum class EffectsProgram : uint8_t { None, Reverb, Hall, Chorus, Echo, Mute };
struct EffectsInfo {
    EffectsProgram program = EffectsProgram::None;
    uint8_t preset = 0;
    std::array<uint8_t, 10> p{};
    float decay = 0, damp = 0, feedback = 0, send = 0, dry_send = 0, chorus_to_reverb = 0, chorus_out = 0;
    double lfo_a = 0, lfo_b = 0, lfo_c = 0; // rad per frame
    float lfo_depth_frames = 0, echo_frames = 0, mod_center = 0, enhance = 0;
    float hall_a = 0, hall_g = 0, hall_g2 = 0, hall_g108 = 0;
    bool bypassed = true;
};

// One envelope generator (patch +20 * n), fully decoded so no consumer has to read raw bytes.
struct EnvelopeInfo {
    std::array<uint8_t, 4> levels{}, rates{}; // +0,+4,+8,+12 and +2,+6,+10,+14, each & 127
    int8_t key_rate = 0;                      // +16 low nibble, signed: rate 1 and 2 shift per key above/below 64
    uint8_t velocity_curve = 0;               // +16 bits 4..5
    bool one_shot = false;                    // +16 bit 6: ignores note-off, runs the release rate after stage 2
    bool loop = false;                        // +16 bit 7: restarts after stage 2
    uint8_t vel_level = 0;                    // +18 high nibble: velocity -> level sensitivity
    uint8_t vel_attack = 0;                   // +18 low nibble: velocity -> attack rate sensitivity
};
struct SplitInfo {
    uint8_t key_low = 0, key_high = 0;        // inclusive; key_high = the record's cutoff key
    int16_t tuning = 0;
    uint8_t loop_mode = 0;                    // low byte of the start longword (index into the split-mode table)
    uint32_t accumulator = 0, start = 0, end = 0;     // packed longwords as stored
    uint32_t start_word = 0, end_word = 0, phase_word = 0; // sample word indices, as begin() resolves them
    bool loop = false, bidirectional = false, reverse = false;
};
// A layer of a layered program: the 0x76-byte patch at program base + layer * 0x76. Field comments give the patch offset.
struct LayerInfo {
    uint8_t index = 0;
    bool enabled = false;
    std::array<uint8_t, 0x76> raw{};
    std::array<EnvelopeInfo, 3> envelope{};   // +0, +20, +40: pitch, filter, amplitude generators
    uint8_t base_pitch = 0;                   // +0x3c
    int8_t base_fine = 0;                     // +0x3e
    bool use_played_key = false;              // +0x48 bit 7
    int8_t pitch_envelope_depth = 0;          // +0x40
    uint8_t pitch_envelope_offset = 0;        // +0x0c & 127
    int8_t lfo_pitch_depth = 0;               // +0x42
    uint8_t pitch_source = 0;                 // +0x44 & 15
    uint8_t allocation_mode = 0;              // +0x44 >> 4 & 3
    uint8_t filter_mode = 0;                  // +0x44 >> 6
    bool k2_bank = false;                     // +0x44 bit 7
    int8_t pitch_source_depth = 0;            // +0x46
    uint8_t k1_base = 0;                      // +0x48 & 127
    int8_t k1_key = 0, k1_envelope = 0;       // +0x4a, +0x4c
    uint8_t filter_source = 0;                // +0x4e & 15
    uint8_t default_pan = 0;                  // +0x4e & 0xf0
    int8_t filter_source_depth = 0;           // +0x50
    uint8_t k2_base = 0;                      // +0x52 & 127
    bool k2_apply_source = false;             // +0x52 bit 7
    int8_t k2_key = 0, k2_envelope = 0;       // +0x54, +0x56
    int8_t weight_amount = 0;                 // +0x58
    uint8_t weight_low = 0, weight_high = 0;  // +0x5a, +0x5c
    uint8_t level = 0;                        // +0x5e & 127
    bool level_plus_43 = false;               // +0x5e bit 7
    uint8_t amplitude_source = 0;             // +0x60 & 15
    uint8_t pair_choice = 0;                  // +0x60 >> 4 & 3
    uint8_t priority = 0;                     // +0x60 & 0xc0
    int8_t amplitude_source_depth = 0;        // +0x62
    uint8_t lfo_source = 0;                   // +0x64 & 15
    uint8_t random_period = 0;                // +0x64 >> 4
    uint8_t lfo_depth = 0;                    // +0x66 & 127
    bool lfo_phase_reset = false;             // +0x66 bit 7
    uint8_t lfo_rate_index = 0;               // +0x68
    uint8_t lfo_wave = 0;                     // +0x6a & 15
    uint8_t lfo_fade_rate = 0;                // +0x6a >> 4
    uint8_t sample_id = 0;                    // +0x6c & 127
    bool reverse = false;                     // +0x6c bit 7
    int8_t velocity_gate = 0;                 // +0x6e (high nibble threshold, sign selects below/above)
    uint8_t start_source = 0;                 // +0x6e & 15
    int8_t start_amount = 0;                  // +0x70
    uint8_t start_offset = 0;                 // +0x72
    uint8_t delay_ticks = 0;                  // +0x74, 0xfb = start on release
    std::vector<SplitInfo> splits;            // key regions of the layer's sample, ascending
};
// One of the 17 key regions of a direct program: the 20-byte record at program base + index * 20.
struct DirectRegionInfo {
    uint8_t index = 0;
    std::array<uint8_t, 20> raw{};
    int8_t key_low = 0, key_high = 0;         // +10, +12
    uint8_t sample_id = 0;                    // +4 & 127
    bool reverse = false;                     // +4 bit 7
    int8_t transposition = 0;                 // +6
    uint8_t fine_pitch = 0;                   // +8 & 15
    uint8_t sensitivity = 0;                  // +8 >> 4
    uint8_t decay_rate = 0;                   // +0 & 127
    bool ignore_note_off = false;             // +0 bit 7
    uint8_t release_rate = 0;                 // +2 & 127
    bool root_key_relative = false;           // +2 bit 7
    uint8_t filter_index = 0;                 // +0x0e
    uint8_t level = 0;                        // +0x10 & 127
    bool level_plus_168 = false;              // +0x10 bit 7
    uint8_t pan = 0;                          // +0x12 & 15
    uint8_t pair_choice = 0;                  // +0x12 >> 4 & 3
    uint8_t velocity_curve = 0;               // +0x12 >> 6
    std::vector<SplitInfo> splits;
};
struct ProgramInfo {
    enum class Kind : uint8_t { Absent, Layered, Direct };
    uint16_t id = 0;                          // bits 14..15 bank, low byte program
    Kind kind = Kind::Absent;
    uint8_t enabled_mask = 0;                 // +0x196 >> 5
    uint8_t glide_rate = 0;                   // +0x17e
    std::array<LayerInfo, 3> layers{};        // Layered
    std::vector<DirectRegionInfo> regions;    // Direct
};

} // namespace f3rt::hle
