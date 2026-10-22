#pragma once
#include "audio/hle/events.hpp"
#include "audio/hle/patch.hpp"
#include "audio/hle/sound_bank.hpp"
#include "audio/hle/sound_rom_5a7e9117.hpp"
#include "f3rt/bytes.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <utility>

namespace f3rt::hle {

using f3rt::s16;
using f3rt::s8;
using sound_5a7e9117::Split;

constexpr uint32_t NATIVE_RATE = (15238090u + 8u * 32u) / (16u * 32u);
constexpr double RATE_RATIO = double(NATIVE_RATE) / SAMPLE_RATE;
constexpr unsigned VOLUME_RAMP = SAMPLE_RATE * 4 / 1000;
constexpr unsigned FILTER_RAMP = SAMPLE_RATE * 3 / 1000;

constexpr int32_t wrap_product(int64_t x) noexcept { return int32_t(uint32_t(x)); }
constexpr uint16_t volume_target(int value, int threshold) noexcept {
    value = s16(value - threshold);
    return value <= 0 ? 0x0ff0 : uint16_t(std::min(0xfff0, value * 48 + 0xff0));
}

constexpr auto make_gain_table() noexcept {
    std::array<float, 256> table{};
    for (unsigned i = 0; i < 256; ++i) {
        table[i] = float((((i & 15) | 16) << 11) >> (16 - (i >> 4))) / 2048.0f;
    }
    return table;
}
inline constexpr auto GAIN_TABLE = make_gain_table();

constexpr float interpolated_gain(double encoded) noexcept {
    const double index = std::clamp(encoded / 256.0, 0.0, 255.0);
    const unsigned i = unsigned(index);
    return float(GAIN_TABLE[i] + (GAIN_TABLE[std::min(i + 1, 255u)] - GAIN_TABLE[i]) * (index - i));
}

struct GainSegment {
    double value = 0, slope = 0;
    unsigned frames = 0;
};

// gain[] is linear between adjacent entries and ramps move the encoded
// volume by a constant step, so a running sum tracks interpolated_gain (to
// double rounding) until the ramp crosses into the next table interval.
constexpr float ramp_gain(double encoded, double step, GainSegment &s) noexcept {
    if (s.frames) {
        --s.frames;
        s.value += s.slope;
        return float(s.value);
    }
    const float exact = interpolated_gain(encoded);
    const double index = encoded / 256.0;
    if (index > 0 && index < 255) {
        const unsigned i = unsigned(index);
        const double per_frame = step / 256.0, delta = double(GAIN_TABLE[i + 1]) - GAIN_TABLE[i];
        s.value = GAIN_TABLE[i] + delta * (index - i);
        s.slope = delta * per_frame;
        const double room = per_frame > 0   ? (double(i + 1) - index) / per_frame
                            : per_frame < 0 ? (index - double(i)) / -per_frame
                                            : 1e9;
        s.frames = room > 2 ? unsigned(std::min(room, 1e6)) - 1 : 0;
    }
    return exact;
}

constexpr float interpolate_sample(double a, double b, double frac) noexcept {
    return float(a + (b - a) * frac);
}

enum class SampleAdvance { Normal, Ended };

constexpr SampleAdvance advance_voice_position(double &pos, bool &reverse, double inc, double start,
                                               double end, double length, bool loop,
                                               bool bidi) noexcept {
    pos += reverse ? -inc : inc;
    if (!reverse && pos > end) {
        if (!loop || length <= 0.0) return SampleAdvance::Ended;
        if (bidi) {
            pos = end - (pos - end);
            reverse = true;
        } else {
            pos = start + std::fmod(pos - end, length);
        }
    } else if (reverse && pos < start) {
        if (!loop || length <= 0.0) return SampleAdvance::Ended;
        if (bidi) {
            pos = start + (start - pos);
            reverse = false;
        } else {
            pos = end - std::fmod(start - pos, length);
        }
    }
    return SampleAdvance::Normal;
}

struct Envelope {
    uint32_t position = 0, target = 0;
    int32_t slope = 0;
    std::array<uint16_t, 4> levels{};
    std::array<uint8_t, 4> rates{};
    uint8_t flags = 0, stage = 0;
    bool intermediate = false, done = false;
    constexpr int16_t level() const noexcept { return s16(position >> 16); }
    constexpr bool advance() noexcept {
        position += uint32_t(slope);
        return int32_t(position - target) >= 0;
    }
};

struct Filter {
    double a1 = 1, a2 = 1, target1 = 1, target2 = 1;
    double high_pole = 1, target_high_pole = 1;
    unsigned ramp = 0;
    uint8_t mode = 3;
    uint16_t last_k1 = 0, last_k2 = 0;
    void targets(uint16_t k1, uint16_t k2, bool initial) {
        if (!initial && k1 == last_k1 && k2 == last_k2) return;
        last_k1 = k1;
        last_k2 = k2;
        // Preserve native pole positions in continuous time, not device
        // register/clock execution. Quantization masks come from ROM's format.
        target1 = 1 - std::pow(1 - double(k1 & 0xfff0) / 65536, RATE_RATIO);
        target2 = 1 - std::pow(1 - double(k2 & 0xfff0) / 65536, RATE_RATIO);
        // Native highpass pole is (1+k/65536)/2. Converting its
        // pole rather than cutoff-Hz guesses keeps low-k differences.
        target_high_pole = std::pow((1 + double(k2 & 0xfff0) / 65536) * 0.5, RATE_RATIO);
        if (initial) {
            a1 = target1;
            a2 = target2;
            high_pole = target_high_pole;
            ramp = 0;
        } else ramp = FILTER_RAMP;
    }
    void advance() {
        if (ramp) {
            a1 += (target1 - a1) / ramp;
            a2 += (target2 - a2) / ramp;
            high_pole += (target_high_pole - high_pole) / ramp;
            --ramp;
        }
    }
};

struct Track;

// The driver's two shared noise words.
struct NoiseState {
    uint16_t value = 0, history = 0;
};

struct Voice {
    NoteOnEvent owner{};
    const Patch *patch = nullptr;
    Track *track = nullptr;
    uint32_t filter_epoch = 0;
    Split split{};
    // envelope[] slots, in EnvelopeRole order.
    static constexpr unsigned pitch_envelope = std::to_underlying(EnvelopeRole::Pitch);
    static constexpr unsigned filter_envelope = std::to_underlying(EnvelopeRole::Filter);
    static constexpr unsigned amplitude_envelope = std::to_underlying(EnvelopeRole::Amplitude);
    static_assert(pitch_envelope == 0 && filter_envelope == 1 && amplitude_envelope == 2);
    std::array<Envelope, 3> envelope{};
    Filter filter{};
    double position = 0, start = 0, end = 0, increment = 0;
    double left = 0, right = 0, target_left = 0, target_right = 0;
    float left_gain = 0, right_gain = 0;
    bool gain_dirty = true;
    uint64_t serial = 0;
    uint16_t frequency = 0, left_volume = 0, right_volume = 0;

    uint16_t filter_cutoff_low = 0;  //!< k1 register
    uint16_t filter_cutoff_high = 0; //!< k2 register
    int16_t lfo = 0, lfo_depth = 0, lfo_target = 0, lfo_step = 0;
    uint16_t lfo_phase = 0, random_countdown = 0, random_period = 0;
    int16_t random = 0, previous_random = 0, pitch = 0, glide = 0, glide_target = 0,
            glide_destination = 0;
    uint8_t selector = 14, pair = 0, priority = 0, allocation = 0;
    uint8_t effective_key = 0, weight = 127, pressure = 0, slot = 0;
    unsigned gain_frames = 0;
    double step_left = 0, step_right = 0;
    GainSegment left_segment{}, right_segment{};
    bool active = false, released = false, reverse = false;
    bool loop = false, bidirectional = false, boundary = false, sample_finished = false,
         hardware_valid = false;
    uint64_t release_serial = 0;

    // The patch owns the kind-specific behavior; the voice exposes one interface for it.
    void initialize(const NoteStart &start, NoiseState &noise, const PriorWindow &prior) {
        patch->initialize(*this, start, noise);
        patch->address(*this, prior);
    }
    void parameters(bool initial) { patch->parameters(*this, initial); }
    void retarget(int kernel, const Program &program) { patch->retarget(*this, kernel, program); }
    EnvelopeTick step(NoiseState &noise) { return patch->step(*this, noise); }
    OnsetStep onset(NoiseState &noise) { return patch->onset(*this, noise); }
    bool release() { return patch->release(*this); }
};

inline void segment(Envelope &e, unsigned target, int r, const SoundBank &bank) {
    int difference = s16(target - uint16_t(e.level()));
    if (difference) {
        e.slope = wrap_product(int64_t(difference) * s16(bank.rate(r)) * 2);
        e.position &= 0xffff0000;
    } else {
        e.slope = -s16(bank.rate(r));
        e.position = (e.position & 0xffff0000) | 0x7fff;
    }
    e.target = uint32_t(uint16_t(target ^ (e.slope <= 0 ? 0x8000 : 0))) << 16;
}

template <EnvelopeRole Role>
inline void initialize_envelope(Envelope &e, EnvelopePatch<Role> patch, const Voice &v,
                                const SoundBank &bank) {
    e = {};
    e.flags = patch.flags();
    unsigned index = velocity_curve_index(patch.velocity_curve(), v.owner.velocity);
    int gain_scale = velocity_level_scale(patch.velocity_level());
    int scale = s16(0x4000 - (128 - bank.velocity_curves[index]) * gain_scale) >> 6;
    for (unsigned i = 0; i < 4; ++i) e.levels[i] = uint16_t(patch.level(i) * scale);
    int key_scale = -(s16((int(v.effective_key) - 64) * patch.key_rate() * 18) >> 6);
    int vel_rate = patch.velocity_attack() * 9 * v.owner.velocity >> 7;
    e.rates[0] = uint8_t(std::clamp(int(patch.rate(0)) - vel_rate, 0, 99));
    e.rates[1] = uint8_t(std::clamp(int(patch.rate(1)) + key_scale, 0, 99));
    e.rates[2] = uint8_t(std::clamp(int(patch.rate(2)) + key_scale, 0, 99));
    e.rates[3] = patch.rate(3);
    e.position = uint32_t(e.levels[0]) << 16;
    if constexpr (Role == EnvelopeRole::Amplitude) {
        if (s16(e.levels[1] - e.levels[0]) > 0x3000 && e.rates[0] > 20) {
            e.intermediate = true;
            segment(e, uint16_t(e.levels[1] - 0x1400), e.rates[0] - 10, bank);
            return;
        }
    }
    segment(e, e.levels[1], e.rates[0], bank);
}

inline double accumulator_start(const Voice &v) noexcept {
    uint32_t address = (v.split.accumulator | ((v.selector & 1) << 28)) & 0x1fffff00;
    return double((v.selector >> 1) << 20) + double(address) / 512;
}

inline void update_random(Voice &v, NoiseState &noise) {
    if (--v.random_countdown) return;
    v.random_countdown = v.random_period;
    noise.history = uint16_t(noise.history + noise.value);
    noise.value = uint16_t(noise.value + noise.history);
    v.random = s16(noise.value); // previous_random retains the onset value.
}

inline void set_glide(Voice &v, int16_t destination, unsigned rate_index, const SoundBank &bank) {
    v.glide_destination = destination;
    if (!rate_index) {
        v.glide = 0;
        v.glide_target = destination;
        return;
    }
    int difference = s16(destination - v.glide_target);
    v.glide = s16((int64_t(difference) *
                   s16(bank.rom.read_u16(sound_5a7e9117::RATE_TABLE + 2 * rate_index)) * 2) >>
                  16);
    if (!v.glide) v.glide_target = destination;
}

inline void advance_glide(Voice &v) noexcept {
    if (!v.glide) return;
    int current = s16(v.glide_target + v.glide);
    if ((v.glide > 0 && current >= v.glide_destination) ||
        (v.glide < 0 && current <= v.glide_destination)) {
        current = v.glide_destination;
        v.glide = 0;
    }
    v.glide_target = s16(current);
}

} // namespace f3rt::hle
