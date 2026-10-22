#pragma once
#include <cstddef>
#include <cstdint>

namespace f3rt::hle {

// A Program is one 0x198-byte instrument. A nonzero enable mask holds three Layer
// records. A zero mask holds seventeen DirectRegion key regions. ProgramService
// turns each record into a LayerPatch or a DirectPatch, and the voice runs the
// behavior the record's kind selects. Every field sits on an even byte. The odd
// bytes are unread.

enum class EnvelopeRole : uint8_t { Pitch = 0, Filter = 1, Amplitude = 2 };

enum class ModSource : uint8_t {
    Lfo = 0,
    PitchEnvelope = 1,
    FilterEnvelope = 2,
    AmplitudeEnvelope = 3,
    Random = 4,
    PreviousRandom = 5,
    Velocity = 6,
    Key = 7,
    Mod1A = 8,
    Mod12 = 9,
    Bend = 10,
    Mod10 = 11,
    Mod0C = 12,
    Pressure = 13,
    Full = 14,
    Off = 15,
};

enum class AllocationMode : uint8_t { Poly = 0, Retrigger = 1, Legato = 2, LegatoHold = 3 };

// Filter stage order, matching voice_filter: bits 6..7 of the layer flags.
// Bit 7 also selects the k2 coefficient half (set = offset 0, clear = 1024),
// so modes 2 and 3 use the low half and modes 0 and 1 the high half.
enum class FilterMode : uint8_t { LpLpHpHp = 0, LpLpLpHp = 1, LpLpLp2Lp2 = 2, LpLpLpLp2 = 3 };

// What a sample id does to start, end, and phase. The four ranges partition 0..127.
struct SampleKind {
    uint8_t id = 0;
    constexpr bool ordinary() const noexcept { return id < 0x54; }
    constexpr bool phase_aligned() const noexcept { return id >= 0x54 && id < 0x6f; }
    constexpr bool probe() const noexcept { return id >= 0x6f && id < 0x78; }
    constexpr bool table() const noexcept { return id >= 0x78; }
};

// High nibble of an envelope's velocity byte, or of a direct region's sensitivity byte.
constexpr int velocity_level_scale(uint8_t nibble) noexcept {
    return int(nibble) * 8 + (int(nibble) >> 1);
}

constexpr unsigned velocity_curve_index(uint8_t curve, uint8_t velocity) noexcept {
    return unsigned(curve) * 128u + velocity;
}

// Twenty bytes. Four breakpoints, then flags and the velocity scale.
// level(0) is where the generator starts. Rates 0..2 ramp toward level(1)..level(3).
// Rate 3 releases to zero. On the pitch envelope, level(3) is also the pitch zero.
template <EnvelopeRole Role> struct EnvelopePatch {
    struct At {
        static constexpr size_t size = 20;
        static constexpr size_t flags = 16;
        static constexpr size_t velocity = 18;
        static constexpr size_t level(unsigned i) noexcept { return i * 4; }
        static constexpr size_t rate(unsigned i) noexcept { return i * 4 + 2; }
    };

    static constexpr size_t size = At::size;
    const uint8_t *bytes = nullptr;

    constexpr explicit EnvelopePatch(const uint8_t *bytes) noexcept : bytes(bytes) {}

    constexpr uint8_t level(unsigned i) const noexcept { return bytes[At::level(i)] & 127; }
    constexpr uint8_t rate(unsigned i) const noexcept { return bytes[At::rate(i)] & 127; }
    constexpr uint8_t flags() const noexcept { return bytes[At::flags]; }
    constexpr int8_t key_rate() const noexcept {
        return int8_t(int8_t((bytes[At::flags] & 15) << 4) >> 4);
    }
    constexpr uint8_t velocity_curve() const noexcept { return (bytes[At::flags] >> 4) & 3; }
    constexpr bool one_shot() const noexcept { return (bytes[At::flags] & 0x40) != 0; }
    constexpr bool loop() const noexcept { return (bytes[At::flags] & 0x80) != 0; }
    constexpr uint8_t velocity_level() const noexcept { return bytes[At::velocity] >> 4; }
    constexpr uint8_t velocity_attack() const noexcept { return bytes[At::velocity] & 15; }
    constexpr uint8_t pitch_zero() const noexcept
        requires(Role == EnvelopeRole::Pitch)
    {
        return level(3);
    }
};

struct Layer {
    struct At {
        static constexpr size_t size = 0x76;
        static constexpr size_t pitch_envelope = 0x00;
        static constexpr size_t filter_envelope = 0x14;
        static constexpr size_t amplitude_envelope = 0x28;
        static constexpr size_t coarse_pitch = 0x3c;
        static constexpr size_t fine_pitch = 0x3e;
        static constexpr size_t pitch_envelope_depth = 0x40;
        static constexpr size_t lfo_pitch_depth = 0x42;
        static constexpr size_t flags = 0x44;
        static constexpr size_t pitch_source_depth = 0x46;
        static constexpr size_t k1_base = 0x48;
        static constexpr size_t k1_key = 0x4a;
        static constexpr size_t k1_envelope = 0x4c;
        static constexpr size_t filter_source = 0x4e;
        static constexpr size_t filter_source_depth = 0x50;
        static constexpr size_t k2_base = 0x52;
        static constexpr size_t k2_key = 0x54;
        static constexpr size_t k2_envelope = 0x56;
        static constexpr size_t weight_amount = 0x58;
        static constexpr size_t weight_low = 0x5a;
        static constexpr size_t weight_high = 0x5c;
        static constexpr size_t level = 0x5e;
        static constexpr size_t amplitude_source = 0x60;
        static constexpr size_t amplitude_source_depth = 0x62;
        static constexpr size_t lfo_source = 0x64;
        static constexpr size_t lfo_depth = 0x66;
        static constexpr size_t lfo_rate = 0x68;
        static constexpr size_t lfo_wave = 0x6a;
        static constexpr size_t sample_id = 0x6c;
        static constexpr size_t gate = 0x6e;
        static constexpr size_t start_amount = 0x70;
        static constexpr size_t start_offset = 0x72;
        static constexpr size_t delay = 0x74;
    };

    static constexpr size_t size = At::size;
    const uint8_t *bytes = nullptr;

    constexpr Layer() noexcept = default;
    constexpr explicit Layer(const uint8_t *bytes) noexcept : bytes(bytes) {}

    constexpr EnvelopePatch<EnvelopeRole::Pitch> pitch() const noexcept {
        return EnvelopePatch<EnvelopeRole::Pitch>{bytes + At::pitch_envelope};
    }
    constexpr EnvelopePatch<EnvelopeRole::Filter> filter() const noexcept {
        return EnvelopePatch<EnvelopeRole::Filter>{bytes + At::filter_envelope};
    }
    constexpr EnvelopePatch<EnvelopeRole::Amplitude> amplitude() const noexcept {
        return EnvelopePatch<EnvelopeRole::Amplitude>{bytes + At::amplitude_envelope};
    }

    // Key * 256, plus coarse semitones and fine in steps of 2/256 semitone.
    // Bit 7 of the k1 base byte selects the played key; clear centers on key 60.
    constexpr int16_t pitch_word(int key) const noexcept {
        const int sum = (use_played_key() ? key : 60) * 256 + int(fine_pitch()) * 2 +
                        (int(coarse_pitch()) << 8);
        return int16_t(uint16_t(sum));
    }
    constexpr uint8_t coarse_pitch() const noexcept { return bytes[At::coarse_pitch]; }
    constexpr int8_t fine_pitch() const noexcept { return int8_t(bytes[At::fine_pitch]); }
    constexpr int8_t pitch_envelope_depth() const noexcept {
        return int8_t(bytes[At::pitch_envelope_depth]);
    }
    constexpr int8_t lfo_pitch_depth() const noexcept { return int8_t(bytes[At::lfo_pitch_depth]); }
    constexpr ModSource pitch_source() const noexcept { return ModSource(bytes[At::flags] & 15); }
    constexpr AllocationMode allocation() const noexcept {
        return AllocationMode((bytes[At::flags] >> 4) & 3);
    }
    constexpr FilterMode filter_mode() const noexcept { return FilterMode(bytes[At::flags] >> 6); }
    constexpr bool k2_low_half() const noexcept { return (bytes[At::flags] & 128) != 0; }
    constexpr unsigned k2_table() const noexcept { return k2_low_half() ? 0u : 1024u; }
    constexpr int8_t pitch_source_depth() const noexcept {
        return int8_t(bytes[At::pitch_source_depth]);
    }

    constexpr bool use_played_key() const noexcept { return (bytes[At::k1_base] & 128) != 0; }
    constexpr uint8_t k1_base() const noexcept { return bytes[At::k1_base] & 127; }
    constexpr int8_t k1_key() const noexcept { return int8_t(bytes[At::k1_key]); }
    constexpr int8_t k1_envelope() const noexcept { return int8_t(bytes[At::k1_envelope]); }
    constexpr ModSource filter_source() const noexcept {
        return ModSource(bytes[At::filter_source] & 15);
    }
    constexpr uint8_t default_pan_byte() const noexcept { return bytes[At::filter_source] & 0xf0; }
    constexpr int8_t default_pan() const noexcept { return int8_t(default_pan_byte()); }
    constexpr int8_t filter_source_depth() const noexcept {
        return int8_t(bytes[At::filter_source_depth]);
    }

    constexpr bool k2_apply_source() const noexcept { return (bytes[At::k2_base] & 128) != 0; }
    constexpr uint8_t k2_base() const noexcept { return bytes[At::k2_base] & 127; }
    constexpr int8_t k2_key() const noexcept { return int8_t(bytes[At::k2_key]); }
    constexpr int8_t k2_envelope() const noexcept { return int8_t(bytes[At::k2_envelope]); }

    constexpr int8_t weight_amount() const noexcept { return int8_t(bytes[At::weight_amount]); }
    constexpr uint8_t weight_low() const noexcept { return bytes[At::weight_low]; }
    constexpr uint8_t weight_high() const noexcept { return bytes[At::weight_high]; }

    constexpr uint8_t level() const noexcept { return bytes[At::level] & 127; }
    constexpr bool level_plus_43() const noexcept { return (bytes[At::level] & 128) != 0; }
    constexpr ModSource amplitude_source() const noexcept {
        return ModSource(bytes[At::amplitude_source] & 15);
    }
    constexpr uint8_t pair_choice() const noexcept {
        return (bytes[At::amplitude_source] >> 4) & 3;
    }
    constexpr uint8_t priority() const noexcept { return bytes[At::amplitude_source] & 0xc0; }
    constexpr int8_t amplitude_source_depth() const noexcept {
        return int8_t(bytes[At::amplitude_source_depth]);
    }

    constexpr ModSource lfo_source() const noexcept {
        return ModSource(bytes[At::lfo_source] & 15);
    }
    constexpr uint8_t random_period() const noexcept { return bytes[At::lfo_source] >> 4; }
    // High nibble expanded the way the voice stores its random countdown.
    constexpr uint16_t random_countdown() const noexcept {
        unsigned period = (bytes[At::lfo_source] & 0xf0) >> 1;
        period |= period >> 4;
        return uint16_t((-int(period)) & 127);
    }
    constexpr uint8_t lfo_depth() const noexcept { return bytes[At::lfo_depth] & 127; }
    constexpr bool lfo_phase_reset() const noexcept { return (bytes[At::lfo_depth] & 128) != 0; }
    constexpr uint8_t lfo_rate() const noexcept { return bytes[At::lfo_rate]; }
    constexpr uint8_t lfo_wave() const noexcept { return bytes[At::lfo_wave] & 15; }
    constexpr uint8_t lfo_fade() const noexcept { return bytes[At::lfo_wave] >> 4; }
    constexpr uint8_t lfo_fade_offset() const noexcept { return bytes[At::lfo_wave] & 0xf0; }

    constexpr uint8_t sample_id() const noexcept { return bytes[At::sample_id] & 127; }
    constexpr bool reverse() const noexcept { return (bytes[At::sample_id] & 128) != 0; }
    constexpr SampleKind sample() const noexcept { return SampleKind{sample_id()}; }
    // Whole signed byte. Bits 7..4 are the velocity gate; the low nibble is start_source.
    constexpr int8_t velocity_gate() const noexcept { return int8_t(bytes[At::gate]); }
    constexpr ModSource start_source() const noexcept { return ModSource(bytes[At::gate] & 15); }
    constexpr int8_t start_amount() const noexcept { return int8_t(bytes[At::start_amount]); }
    constexpr uint8_t start_amount_byte() const noexcept { return bytes[At::start_amount]; }
    constexpr uint8_t start_offset() const noexcept { return bytes[At::start_offset]; }
    constexpr uint8_t delay_ticks() const noexcept { return bytes[At::delay]; }
    constexpr bool starts_on_release() const noexcept { return bytes[At::delay] == 0xfb; }
};

struct DirectRegion {
    struct At {
        static constexpr size_t size = 20;
        static constexpr size_t decay = 0x00;
        static constexpr size_t release = 0x02;
        static constexpr size_t sample_id = 0x04;
        static constexpr size_t transpose = 0x06;
        static constexpr size_t sensitivity = 0x08;
        static constexpr size_t key_low = 0x0a;
        static constexpr size_t key_high = 0x0c;
        static constexpr size_t filter_index = 0x0e;
        static constexpr size_t level = 0x10;
        static constexpr size_t pan = 0x12;
    };

    static constexpr size_t size = At::size;
    const uint8_t *bytes = nullptr;

    constexpr DirectRegion() noexcept = default;
    constexpr explicit DirectRegion(const uint8_t *bytes) noexcept : bytes(bytes) {}

    constexpr uint8_t decay_rate() const noexcept { return bytes[At::decay] & 127; }
    constexpr bool ignore_note_off() const noexcept { return (bytes[At::decay] & 128) != 0; }
    constexpr uint8_t release_rate() const noexcept { return bytes[At::release] & 127; }
    constexpr bool root_key_relative() const noexcept { return (bytes[At::release] & 128) != 0; }
    constexpr uint8_t sample_id() const noexcept { return bytes[At::sample_id] & 127; }
    constexpr bool reverse() const noexcept { return (bytes[At::sample_id] & 128) != 0; }
    constexpr SampleKind sample() const noexcept { return SampleKind{sample_id()}; }
    constexpr int8_t transpose() const noexcept { return int8_t(bytes[At::transpose]); }
    constexpr uint8_t fine_pitch() const noexcept { return bytes[At::sensitivity] & 15; }
    constexpr uint8_t velocity_level() const noexcept { return bytes[At::sensitivity] >> 4; }
    constexpr int8_t key_low() const noexcept { return int8_t(bytes[At::key_low]); }
    constexpr int8_t key_high() const noexcept { return int8_t(bytes[At::key_high]); }
    constexpr bool contains(int key) const noexcept {
        return key >= key_low() && key <= key_high();
    }
    // Raw low-key byte. The root-key formula subtracts this, not the signed range.
    constexpr uint8_t key_low_byte() const noexcept { return bytes[At::key_low]; }
    constexpr uint8_t filter_index() const noexcept { return bytes[At::filter_index]; }
    constexpr uint8_t level() const noexcept { return bytes[At::level] & 127; }
    constexpr bool level_plus_168() const noexcept { return (bytes[At::level] & 128) != 0; }
    constexpr uint8_t pan() const noexcept { return bytes[At::pan] & 15; }
    // Nibble 0 is center: XOR 8 flips it around the middle, then scale into the pan table.
    constexpr int centered_pan() const noexcept { return (pan() ^ 8) << 3; }
    constexpr uint8_t pair_choice() const noexcept { return (bytes[At::pan] >> 4) & 3; }
    constexpr uint8_t velocity_curve() const noexcept { return bytes[At::pan] >> 6; }

    // channel_transpose is already sign-extended. The result is the 8-bit key used as key * 256.
    constexpr int8_t sounding_key(int kernel, int channel_transpose) const noexcept {
        const int key = root_key_relative() ? int(uint8_t(kernel - int(key_low_byte()) + 60)) : 60;
        return int8_t(uint8_t(key + channel_transpose + int(transpose())));
    }
};

// One instrument record. layer() and region() are the two layouts of these bytes.
struct Program {
    struct At {
        static constexpr size_t size = 0x198;
        static constexpr size_t glide = 0x17e;
        static constexpr size_t enabled = 0x196;
        static constexpr unsigned layer_count = 3;
        static constexpr unsigned region_count = 17;
    };

    static constexpr size_t size = At::size;
    static constexpr unsigned layer_count = At::layer_count;
    static constexpr unsigned region_count = At::region_count;
    const uint8_t *bytes = nullptr;

    constexpr Program() noexcept = default;
    constexpr explicit Program(const uint8_t *bytes) noexcept : bytes(bytes) {}
    constexpr explicit operator bool() const noexcept { return bytes != nullptr; }

    // Top three bits of byte 0x196. Zero selects the seventeen key regions.
    constexpr uint8_t enabled() const noexcept { return bytes[At::enabled] >> 5; }
    constexpr uint8_t glide() const noexcept { return bytes[At::glide]; }
    constexpr Layer layer(unsigned index) const noexcept {
        return Layer{bytes + index * Layer::size};
    }
    constexpr DirectRegion region(unsigned index) const noexcept {
        return DirectRegion{bytes + index * DirectRegion::size};
    }
};

class SoundBank;
struct Voice;
struct NoteOnEvent;
struct NoiseState;

// Where the previous note left this physical slot's sample window. A probe sample keeps it.
struct PriorWindow {
    double start = 0, end = 0;
    bool valid = false;
};

// A note being started, after the track's allocation decision.
struct NoteStart {
    const NoteOnEvent &event;
    int kernel = 0;
    int transpose = 0;
    AllocationMode mode = AllocationMode::Poly;
    int prior_pitch = 0;
    bool prior_found = false;
    const Program &program;
};

// One envelope advance: whether the voice ends, and whether a looping amplitude envelope restarted the sample.
struct EnvelopeTick {
    bool ended = false, restarted = false;
};

// The extra work a layered voice does on admission: a direct voice waits for the ring.
struct OnsetStep {
    bool ended = false, restarted = false, parameters = false;
};

// The behavior of one program record. A LayerPatch runs three envelopes, an LFO and glide; a DirectPatch runs one
// amplitude envelope with a fixed velocity response. ProgramService owns both kinds; VoiceService and Voice start,
// step and release a voice without asking which kind it holds. Every patch is constructed with the bank that owns
// its program bytes, so there is no empty patch state.
class Patch {
public:
    Patch(const SoundBank &bank, const uint8_t *program, uint8_t index, uint8_t delay_ticks,
          bool starts_on_release) noexcept
        : m_bank(bank), m_program(program), m_index(index), m_delay_ticks(delay_ticks),
          m_starts_on_release(starts_on_release) {}
    virtual ~Patch() = default;

    const SoundBank &bank() const noexcept { return m_bank; }
    constexpr Program program() const noexcept { return Program{m_program}; }
    constexpr uint8_t index() const noexcept { return m_index; } // layer 0..2, or region 0..16
    constexpr uint8_t delay_ticks() const noexcept { return m_delay_ticks; }
    constexpr bool starts_on_release() const noexcept { return m_starts_on_release; }

    virtual bool layered() const noexcept = 0;
    virtual AllocationMode allocation() const noexcept = 0;
    virtual unsigned priority() const noexcept = 0;
    // Key weight, or -1 when this note is gated out of the patch.
    virtual int weight(const NoteOnEvent &note, int kernel) const = 0;
    // Whether an active voice of the same layer slot may be taken over for legato or retrigger.
    virtual bool reuses(const Voice &voice) const noexcept = 0;

    // Pitch, envelopes and LFO for the new voice; then its split and sample window.
    virtual void initialize(Voice &voice, const NoteStart &start, NoiseState &noise) const = 0;
    virtual void address(Voice &voice, const PriorWindow &prior) const = 0;
    // Legato re-entry of a still-running voice: the new key's pitch becomes the glide destination.
    virtual void retarget(Voice &voice, int kernel, const Program &program) const = 0;

    virtual EnvelopeTick step(Voice &voice, NoiseState &noise) const = 0;
    virtual OnsetStep onset(Voice &voice, NoiseState &noise) const = 0;
    virtual void parameters(Voice &voice, bool initial) const = 0;
    // True when the release ends the voice immediately.
    virtual bool release(Voice &voice) const = 0;

private:
    const SoundBank &m_bank;
    const uint8_t *m_program = nullptr;
    uint8_t m_index = 0;
    uint8_t m_delay_ticks = 0;
    bool m_starts_on_release = false;
};

class LayerPatch final : public Patch {
public:
    LayerPatch(Layer view, const SoundBank &bank, const uint8_t *program, uint8_t index,
               uint8_t delay_ticks, bool starts_on_release) noexcept
        : Patch(bank, program, index, delay_ticks, starts_on_release), m_view(view) {}

    bool layered() const noexcept override { return true; }
    AllocationMode allocation() const noexcept override { return m_view.allocation(); }
    unsigned priority() const noexcept override { return m_view.priority(); }
    int weight(const NoteOnEvent &note, int kernel) const override;
    bool reuses(const Voice &voice) const noexcept override;

    void initialize(Voice &voice, const NoteStart &start, NoiseState &noise) const override;
    void address(Voice &voice, const PriorWindow &prior) const override;
    void retarget(Voice &voice, int kernel, const Program &program) const override;

    EnvelopeTick step(Voice &voice, NoiseState &noise) const override;
    OnsetStep onset(Voice &voice, NoiseState &noise) const override;
    void parameters(Voice &voice, bool initial) const override;
    bool release(Voice &voice) const override;

private:
    EnvelopeTick advance_envelopes(Voice &voice) const;
    EnvelopeTick advance(Voice &voice, unsigned which) const;

    Layer m_view{};
};

class DirectPatch final : public Patch {
public:
    DirectPatch(DirectRegion view, const SoundBank &bank, const uint8_t *program,
                uint8_t index) noexcept
        : Patch(bank, program, index, 0, false), m_view(view) {}

    constexpr bool contains_key(int key) const noexcept { return m_view.contains(key); }

    bool layered() const noexcept override { return false; }
    AllocationMode allocation() const noexcept override { return AllocationMode::Poly; }
    unsigned priority() const noexcept override { return 64; }
    int weight(const NoteOnEvent &note, int kernel) const override;
    bool reuses(const Voice &) const noexcept override { return false; }

    void initialize(Voice &voice, const NoteStart &start, NoiseState &noise) const override;
    void address(Voice &voice, const PriorWindow &prior) const override;
    void retarget(Voice &, int, const Program &) const override {}

    EnvelopeTick step(Voice &voice, NoiseState &noise) const override;
    OnsetStep onset(Voice &voice, NoiseState &noise) const override;
    void parameters(Voice &voice, bool initial) const override;
    bool release(Voice &voice) const override;

private:
    DirectRegion m_view{};
};

// Filter-table index of a direct (key-region) voice, as the sound driver computes it at c195a4-c195d0:
// d0 = (patch[0xe] & 15) * word8; d0 = (d0 << 7) >> 16; d0 += patch[0xe] * 4. word8 is voice +8, which the
// note-on routine (c17baa-c17bb2) fills with the note velocity (bit 7 cleared) in both bytes, i.e.
// velocity * 257. The driver then clamps the table byte offset 2 * index to 0x7fe.
inline int direct_filter_index(uint8_t patch_e, uint8_t velocity) {
    const int word8 = int(velocity & 127) * 257;
    return patch_e * 4 + ((patch_e & 15) * word8 >> 9);
}

} // namespace f3rt::hle
