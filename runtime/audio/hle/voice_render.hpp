#pragma once
#include "audio/hle/block.hpp"
#include "audio/hle/synthesis.hpp"
#include "audio/hle/voice.hpp"
#include "audio/hle/voice_filter.hpp"
#include "audio/hle/voice_service.hpp"

#include <algorithm>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <span>

namespace f3rt::hle {

inline constexpr float GAIN_SCALE = 1.f / 524288.f;

class SampleWords {
public:
    explicit SampleWords(std::span<const uint16_t> words) noexcept
        : m_words(reinterpret_cast<const int16_t *>(words.data())), m_mask(words.size() - 1),
          m_limit(double(words.size())) {}
    [[gnu::always_inline]] float interpolate(double position) const noexcept {
        const size_t i = size_t(position);
        const int16_t *word = m_words + i;
        return interpolate_sample(word[0], word[1], position - double(i));
    }
    [[gnu::always_inline]] float interpolate(const Voice &voice) const noexcept {
        const size_t index = size_t(voice.position);
        const double fraction = voice.position - double(index);
        const double first =
            (!voice.loop && voice.reverse && index < size_t(voice.start)) ? 0.0 : word(index);
        const double second = (!voice.loop && !voice.reverse && index + 1 >= size_t(voice.end))
                                  ? 0.0
                                  : word(index + 1);
        return interpolate_sample(first, second, fraction);
    }
    [[gnu::always_inline]] bool outside(double position) const noexcept {
        return position < 0 || position >= m_limit;
    }
    [[gnu::always_inline]] size_t room(const Voice &voice, size_t cap) const noexcept {
        double left = -1;
        if (!voice.reverse) {
            if (voice.position >= 0 && voice.position < m_limit - 1 && voice.end < m_limit - 1 &&
                voice.increment > 0)
                left = (voice.end - voice.position) / voice.increment;
        } else {
            if (voice.position >= 0 && voice.start >= 0 && voice.position < m_limit - 1 &&
                voice.increment > 0)
                left = (voice.position - voice.start) / voice.increment;
        }
        if (left < 2.0) return 0;
        return left < double(cap) + 1.0 ? size_t(left) - 1 : cap;
    }

private:
    double word(size_t at) const noexcept { return m_words[at & m_mask]; }
    const int16_t *m_words;
    size_t m_mask;
    double m_limit;
};

struct VoiceRun {
    enum class Kind : uint8_t { Single, Constant, Ramping };
    Kind kind;
    size_t frames;
};
[[nodiscard, gnu::always_inline]] inline VoiceRun plan_run(const Voice &voice, size_t rows,
                                                           const SampleWords &samples) noexcept {
    const bool ramping = voice.gain_frames > 0;
    size_t frames = 0;
    if (!voice.sample_finished && voice.filter.ramp == 0 &&
        (!ramping || (voice.left_segment.frames > 0 && voice.right_segment.frames > 0 &&
                      voice.gain_frames > 1))) {
        size_t cap = rows;
        if (ramping) {
            cap = std::min(cap, size_t(voice.left_segment.frames));
            cap = std::min(cap, size_t(voice.right_segment.frames));
            cap = std::min(cap, size_t(voice.gain_frames - 1));
        }
        frames = samples.room(voice, cap);
    }
    if (frames < 1) return {VoiceRun::Kind::Single, 1};
    return {ramping ? VoiceRun::Kind::Ramping : VoiceRun::Kind::Constant, frames};
}

struct StereoGain {
    float left = 0.f, right = 0.f;
    [[gnu::always_inline]] void mix(float *row, float y) const noexcept {
        row[0] += left * y;
        row[1] += right * y;
    }
};
[[nodiscard, gnu::always_inline]] inline StereoGain current_gain(Voice &voice) noexcept {
    if (voice.gain_dirty) {
        voice.left_gain = interpolated_gain(voice.left);
        voice.right_gain = interpolated_gain(voice.right);
        voice.gain_dirty = false;
    }
    return {voice.left_gain * GAIN_SCALE, voice.right_gain * GAIN_SCALE};
}
[[gnu::always_inline]] inline void step_gain(Voice &voice) noexcept {
    if (!voice.gain_frames) return;
    voice.left += voice.step_left;
    voice.right += voice.step_right;
    if (!--voice.gain_frames) {
        voice.left = voice.target_left;
        voice.right = voice.target_right;
        voice.gain_dirty = true;
    } else {
        voice.left_gain = ramp_gain(voice.left, voice.step_left, voice.left_segment);
        voice.right_gain = ramp_gain(voice.right, voice.step_right, voice.right_segment);
        voice.gain_dirty = false;
    }
}

template <class G>
concept RunGain = requires(G gain, Voice &voice, size_t frames) {
    { gain.next() } -> std::same_as<StereoGain>;
    gain.store(voice, frames);
};
struct ConstantGain {
    StereoGain gain;
    [[gnu::always_inline]] explicit ConstantGain(Voice &voice) noexcept
        : gain(current_gain(voice)) {}
    [[gnu::always_inline]] StereoGain next() const noexcept { return gain; }
    [[gnu::always_inline]] void store(Voice &, size_t) const noexcept {}
};
class RampingGain {
public:
    [[gnu::always_inline]] explicit RampingGain(const Voice &voice) noexcept
        : m_left(voice.left), m_right(voice.right), m_left_value(voice.left_segment.value),
          m_right_value(voice.right_segment.value), m_left_slope(voice.left_segment.slope),
          m_right_slope(voice.right_segment.slope), m_step_left(voice.step_left),
          m_step_right(voice.step_right) {}
    [[gnu::always_inline]] StereoGain next() noexcept {
        m_left += m_step_left;
        m_right += m_step_right;
        m_left_value += m_left_slope;
        m_right_value += m_right_slope;
        return {float(m_left_value) * GAIN_SCALE, float(m_right_value) * GAIN_SCALE};
    }
    [[gnu::always_inline]] void store(Voice &voice, size_t frames) const noexcept {
        voice.left = m_left;
        voice.right = m_right;
        voice.left_segment.frames -= unsigned(frames);
        voice.left_segment.value = m_left_value;
        voice.right_segment.frames -= unsigned(frames);
        voice.right_segment.value = m_right_value;
        voice.gain_frames -= unsigned(frames);
        voice.left_gain = float(m_left_value);
        voice.right_gain = float(m_right_value);
        voice.gain_dirty = false;
    }

private:
    double m_left, m_right, m_left_value, m_right_value;
    double m_left_slope, m_right_slope, m_step_left, m_step_right;
};

template <unsigned Mode> struct VoiceFilter {
    VoiceFilterState state;
    float coef1, coef2, pole;
    [[gnu::always_inline]] VoiceFilter(const VoiceFilterState &memory,
                                       const Filter &filter) noexcept
        : state(memory), coef1(float(filter.a1)), coef2(float(filter.a2)),
          pole(float(filter.high_pole)) {}
    [[gnu::always_inline]] float step(float x) noexcept {
        return filter_step<Mode>(state, x, coef1, coef2, pole);
    }
};

struct Chunk {
    float *rows;
    size_t first_frame, frames;
    [[gnu::always_inline]] float *row(size_t r) const noexcept { return rows + r * 8; }
    [[gnu::always_inline]] size_t frame(size_t r) const noexcept { return first_frame + r; }
};
class VoiceBus {
public:
    VoiceBus(MutableAudioBlockSpan<8> buses, uint8_t pair,
             std::span<float, VOICE_BLOCK_FRAMES * 8> discard) noexcept
        : m_discard(discard) {
        if (const auto select = otis_target_bus(pair))
            m_bus = buses.data() + buses.slice(*select).channel_left();
    }
    [[nodiscard, gnu::always_inline]] Chunk chunk(size_t first_frame,
                                                  size_t frames) const noexcept {
        if (m_bus) return {m_bus + first_frame * 8, first_frame, frames};
        std::fill_n(m_discard.data(), frames * 8, 0.f);
        return {m_discard.data(), first_frame, frames};
    }

private:
    float *m_bus = nullptr;
    std::span<float, VOICE_BLOCK_FRAMES * 8> m_discard;
};

template <unsigned Mode> class VoiceSpan {
public:
    VoiceSpan(Voice &voice, SampleWords samples, const VoiceFilterState &memory) noexcept
        : m_voice(voice), m_samples(samples), m_filter(memory) {}

    [[nodiscard, gnu::noinline]] VoiceFilterState render(const VoiceBus &bus, VoiceService &voices,
                                                         std::span<const uint64_t> frame_ticks,
                                                         size_t frame_start, size_t frame_end) && {
        for (size_t first = frame_start; first < frame_end; first += VOICE_BLOCK_FRAMES) {
            const Chunk chunk = bus.chunk(first, std::min(VOICE_BLOCK_FRAMES, frame_end - first));
            for (size_t row = 0; row < chunk.frames;) {
                const VoiceRun run = plan_run(m_voice, chunk.frames - row, m_samples);
                switch (run.kind) {
                case VoiceRun::Kind::Single:
                    row += render_frame(chunk, row, voices, frame_ticks);
                    break;
                case VoiceRun::Kind::Constant:
                    row += render_run(chunk, row, run.frames, ConstantGain(m_voice), voices);
                    break;
                case VoiceRun::Kind::Ramping:
                    row += render_run(chunk, row, run.frames, RampingGain(m_voice), voices);
                    break;
                }
            }
        }
        return m_filter;
    }

private:
    template <RunGain Gain>
    [[gnu::always_inline]] size_t render_run(const Chunk &chunk, size_t row, size_t frames,
                                             Gain gain, VoiceService &voices) {
        const double step = m_voice.reverse ? -m_voice.increment : m_voice.increment;
        const SampleWords samples = m_samples;
        VoiceFilter<Mode> filter(m_filter, m_voice.filter);
        float *out = chunk.row(row);
        double position = m_voice.position;
        float input = samples.interpolate(position);
        position += step;
        for (size_t k = 1; k < frames; ++k, out += 8) {
            const float next = samples.interpolate(position);
            position += step;
            gain.next().mix(out, filter.step(input));
            input = next;
        }
        gain.next().mix(out, filter.step(input));
        m_filter = filter.state;
        m_voice.position = position;
        gain.store(m_voice, frames);
        voices.set_render_frame(chunk.frame(row + frames - 1));
        return frames;
    }
    [[gnu::always_inline]] size_t render_frame(const Chunk &chunk, size_t row, VoiceService &voices,
                                               std::span<const uint64_t> frame_ticks) {
        const size_t frame = chunk.frame(row);
        voices.set_render_frame(frame);
        step_gain(m_voice);
        if (m_voice.sample_finished) {
            emit(chunk.row(row), 0.f, StereoGain{});
            return 1;
        }
        if (m_samples.outside(m_voice.position)) {
            m_voice.active = false;
            silence(chunk, row);
            return chunk.frames - row;
        }
        const float input = m_samples.interpolate(m_voice);
        m_voice.filter.advance();
        const StereoGain gain = current_gain(m_voice);
        const bool ended =
            advance_voice_position(m_voice.position, m_voice.reverse, m_voice.increment,
                                   m_voice.start, m_voice.end, m_voice.end - m_voice.start,
                                   m_voice.loop, m_voice.bidirectional) == SampleAdvance::Ended;
        emit(chunk.row(row), input, gain);
        if (!ended) return 1;
        voices.end_sample(m_voice, frame_ticks[frame]);
        silence(chunk, row + 1);
        return chunk.frames - row;
    }
    [[gnu::always_inline]] void emit(float *row, float input, StereoGain gain) {
        VoiceFilter<Mode> filter(m_filter, m_voice.filter);
        gain.mix(row, filter.step(input));
        m_filter = filter.state;
    }
    [[gnu::always_inline]] void silence(const Chunk &chunk, size_t from_row) {
        VoiceFilter<Mode> filter(m_filter, m_voice.filter);
        for (size_t row = from_row; row < chunk.frames; ++row)
            StereoGain{}.mix(chunk.row(row), filter.step(0.f));
        m_filter = filter.state;
    }

    Voice &m_voice;
    const SampleWords m_samples;
    VoiceFilterState m_filter;
};

} // namespace f3rt::hle
