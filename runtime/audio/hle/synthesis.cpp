#include "audio/hle/synthesis.hpp"
#include "audio/hle/voice.hpp"
#include <algorithm>
#include <utility>

namespace f3rt::hle {
namespace {

constexpr float GAIN_SCALE = 1.f / 524288.f;

// One voice rendered over [frame_start, frame_end), a chunk at a time, straight into the bus pair `bus` (the left
// channel of frame 0 of the block; nullptr: the voice reaches no bus). A sample is interpolated, run through the
// filter and added to the bus in one pass; the four filter stages stay in registers for the whole call.
template <unsigned Mode> class VoiceSpan {
public:
    VoiceSpan(VoiceService &voices, Voice &voice, SoundBank &bank, VoiceRenderState &state,
              float *bus, std::span<const uint64_t> frame_ticks)
        : m_voices(voices), m_voice(voice), m_state(state), m_bus(bus), m_frame_ticks(frame_ticks),
          m_samples(reinterpret_cast<const int16_t *>(bank.samples.data())),
          m_mask(bank.samples.size() - 1), m_limit(double(bank.samples.size())),
          m_filter(state.filters[voice.slot]) {}
    ~VoiceSpan() { m_state.filters[m_voice.slot] = m_filter; }

    void render(size_t frame_start, size_t frame_end) {
        for (size_t chunk_start = frame_start; chunk_start < frame_end;
             chunk_start += VOICE_BLOCK_FRAMES)
            chunk(chunk_start, std::min(VOICE_BLOCK_FRAMES, frame_end - chunk_start));
    }

private:
    VoiceService &m_voices;
    Voice &m_voice;
    VoiceRenderState &m_state;
    float *m_bus;
    std::span<const uint64_t> m_frame_ticks;
    const int16_t *m_samples; // the sample ROM words, signed
    size_t m_mask; // the ROM size is a power of two (SoundBank checks it): addresses wrap with it
    double m_limit;
    VoiceFilterState m_filter;
    float *m_out = nullptr; // left channel of the chunk's first frame

    // The sample ROM read at phase `pos`, linearly interpolated.
    static float interpolate_at(const int16_t *samples, double pos) {
        const size_t i = size_t(pos);
        const int16_t *word = samples + i;
        return interpolate_sample(word[0], word[1], pos - double(i));
    }

    // One sample through the filter and onto the bus, at chunk row `row`.
    void emit(size_t row, float input, float gain_left, float gain_right) {
        const float y =
            filter_step<Mode>(m_filter, input, float(m_voice.filter.a1), float(m_voice.filter.a2),
                              float(m_voice.filter.high_pole));
        m_out[row * 8] += gain_left * y;
        m_out[row * 8 + 1] += gain_right * y;
    }

    // The rows of a chunk that follow an ended sample: silence through the filter.
    void silence(size_t from_row, size_t chunk_frames) {
        for (size_t row = from_row; row < chunk_frames; ++row) emit(row, 0.f, 0.f, 0.f);
    }

    // `count` samples whose filter coefficients and gains stay constant, read from the sample ROM at `position`.
    // The sample after the current one is interpolated while the current one is filtered: the two chains do not
    // depend on each other, so a core that issues in order (the 3DS's ARM11) overlaps their latencies.
    void constant_run(size_t row, size_t count, double &position, double step, float gain_left,
                      float gain_right) {
        const float coef1 = float(m_voice.filter.a1), coef2 = float(m_voice.filter.a2),
                    pole = float(m_voice.filter.high_pole);
        const int16_t *samples = m_samples;
        float *out = m_out + row * 8;
        VoiceFilterState filter = m_filter;
        double pos = position;
        float input = interpolate_at(samples, pos);
        pos += step;
        for (float *const last = out + (count - 1) * 8; out != last; out += 8) {
            const float next = interpolate_at(samples, pos);
            pos += step;
            const float y = filter_step<Mode>(filter, input, coef1, coef2, pole);
            out[0] += gain_left * y;
            out[1] += gain_right * y;
            input = next;
        }
        const float y = filter_step<Mode>(filter, input, coef1, coef2, pole);
        out[0] += gain_left * y;
        out[1] += gain_right * y;
        m_filter = filter;
        position = pos;
    }

    // `count` samples inside both gain segments: the gains follow the segment ramps, the coefficients are constant.
    void ramping_run(size_t row, size_t count, double &position, double step) {
        const float coef1 = float(m_voice.filter.a1), coef2 = float(m_voice.filter.a2),
                    pole = float(m_voice.filter.high_pole);
        const int16_t *samples = m_samples;
        float *out = m_out + row * 8;
        VoiceFilterState filter = m_filter;
        double pos = position;
        double left = m_voice.left, right = m_voice.right;
        double left_value = m_voice.left_segment.value;
        const double left_slope = m_voice.left_segment.slope;
        double right_value = m_voice.right_segment.value;
        const double right_slope = m_voice.right_segment.slope;
        const double step_left = m_voice.step_left, step_right = m_voice.step_right;
        const auto emit = [&](float input) {
            left += step_left;
            right += step_right;
            left_value += left_slope;
            right_value += right_slope;
            const float y = filter_step<Mode>(filter, input, coef1, coef2, pole);
            out[0] += float(left_value) * GAIN_SCALE * y;
            out[1] += float(right_value) * GAIN_SCALE * y;
            out += 8;
        };
        float input = interpolate_at(samples, pos);
        pos += step;
        for (size_t k = 1; k < count; ++k) {
            const float next = interpolate_at(samples, pos);
            pos += step;
            emit(input);
            input = next;
        }
        emit(input);
        m_filter = filter;
        position = pos;
        m_voice.left = left;
        m_voice.right = right;
        m_voice.left_segment.frames -= unsigned(count);
        m_voice.left_segment.value = left_value;
        m_voice.right_segment.frames -= unsigned(count);
        m_voice.right_segment.value = right_value;
        m_voice.gain_frames -= unsigned(count);
        m_voice.left_gain = float(left_value);
        m_voice.right_gain = float(right_value);
        m_voice.gain_dirty = false;
    }

    // How many samples from the current phase can be rendered without the end and loop rules: the position
    // moves by at most floor(left) - 1 steps before it could reach the end of the sample (the start when reversed),
    // `left` being the distance in steps. At most `cap`; 0 when fewer than one.
    size_t safe_samples(size_t cap) const {
        double left = -1;
        if (!m_voice.reverse) {
            if (m_voice.position >= 0 && m_voice.position < m_limit - 1 &&
                m_voice.end < m_limit - 1 && m_voice.increment > 0)
                left = (m_voice.end - m_voice.position) / m_voice.increment;
        } else {
            if (m_voice.position >= 0 && m_voice.start >= 0 && m_voice.position < m_limit - 1 &&
                m_voice.increment > 0)
                left = (m_voice.position - m_voice.start) / m_voice.increment;
        }
        if (left < 2.0) return 0;
        // floor(left) - 1 >= cap unless left < cap + 1, and then left (>= 2) truncates to its floor.
        return left < double(cap) + 1.0 ? size_t(left) - 1 : cap;
    }

    void chunk(size_t chunk_start, size_t chunk_frames) {
        if (m_bus) {
            m_out = m_bus + chunk_start * 8;
        } else {
            m_out = m_state.discard.data();
            std::fill_n(m_out, chunk_frames * 8, 0.f);
        }
        size_t r = 0;
        while (r < chunk_frames) {
            // A run needs a constant filter and, while the gain ramps, gain segments that hold: it ends where one
            // does. Everything else (and the samples near the end of the sample) takes the single-sample rules.
            const bool ramping = m_voice.gain_frames > 0;
            size_t n = 0;
            if (!m_voice.sample_finished && m_voice.filter.ramp == 0 &&
                (!ramping || (m_voice.left_segment.frames > 0 && m_voice.right_segment.frames > 0 &&
                              m_voice.gain_frames > 1))) {
                size_t cap = chunk_frames - r;
                if (ramping) {
                    cap = std::min(cap, size_t(m_voice.left_segment.frames));
                    cap = std::min(cap, size_t(m_voice.right_segment.frames));
                    cap = std::min(cap, size_t(m_voice.gain_frames - 1));
                }
                n = safe_samples(cap);
            }
            if (n < 1) {
                if (!single(chunk_start, chunk_frames, r)) return;
                continue;
            }
            const double step = m_voice.reverse ? -m_voice.increment : m_voice.increment;
            double position = m_voice.position;
            if (ramping) {
                ramping_run(r, n, position, step);
            } else {
                if (m_voice.gain_dirty) {
                    m_voice.left_gain = interpolated_gain(m_voice.left);
                    m_voice.right_gain = interpolated_gain(m_voice.right);
                    m_voice.gain_dirty = false;
                }
                constant_run(r, n, position, step, m_voice.left_gain * GAIN_SCALE,
                             m_voice.right_gain * GAIN_SCALE);
            }
            m_voice.position = position;
            m_voices.set_render_frame(chunk_start + r + n - 1);
            r += n;
        }
    }

    // The gain ramp of the voice, stepped once at the start of every sample.
    void step_gain() {
        if (!m_voice.gain_frames) return;
        m_voice.left += m_voice.step_left;
        m_voice.right += m_voice.step_right;
        if (!--m_voice.gain_frames) {
            m_voice.left = m_voice.target_left;
            m_voice.right = m_voice.target_right;
            m_voice.gain_dirty = true;
        } else {
            m_voice.left_gain = ramp_gain(m_voice.left, m_voice.step_left, m_voice.left_segment);
            m_voice.right_gain =
                ramp_gain(m_voice.right, m_voice.step_right, m_voice.right_segment);
            m_voice.gain_dirty = false;
        }
    }

    // The ROM word at `at`, wrapped into the ROM.
    double word(size_t at) const { return m_samples[at & m_mask]; }

    // The current phase's sample. The first and second neighbours read as silence outside a non-looping sample.
    float interpolate_voice() const {
        const size_t index = size_t(m_voice.position);
        const double fraction = m_voice.position - double(index);
        const double first =
            (!m_voice.loop && m_voice.reverse && index < size_t(m_voice.start)) ? 0.0 : word(index);
        const double second =
            (!m_voice.loop && !m_voice.reverse && index + 1 >= size_t(m_voice.end))
                ? 0.0
                : word(index + 1);
        return interpolate_sample(first, second, fraction);
    }

    // Applies the pending gain update; the gains of the current sample.
    std::pair<float, float> sample_gains() {
        if (m_voice.gain_dirty) {
            m_voice.left_gain = interpolated_gain(m_voice.left);
            m_voice.right_gain = interpolated_gain(m_voice.right);
            m_voice.gain_dirty = false;
        }
        return {m_voice.left_gain * GAIN_SCALE, m_voice.right_gain * GAIN_SCALE};
    }

    // The sample at row `r` with every per-sample rule, the end and loop handling included. Returns false when the
    // chunk is over (the voice ended or left the sample ROM).
    bool single(size_t chunk_start, size_t chunk_frames, size_t &r) {
        const size_t frame = chunk_start + r;
        m_voices.set_render_frame(frame);
        const uint64_t frame_tick = m_frame_ticks[frame];
        step_gain();
        if (m_voice.sample_finished) {
            emit(r, 0.f, 0.f, 0.f);
            ++r;
            return true;
        }
        if (m_voice.position < 0 || m_voice.position >= m_limit) {
            m_voice.active = false; // out-of-range phase: drop the voice silently, as the ROM does.
            silence(r, chunk_frames);
            return false;
        }
        const float input = interpolate_voice();
        m_voice.filter.advance();
        const auto [gain_left, gain_right] = sample_gains();
        const bool ended =
            advance_voice_position(m_voice.position, m_voice.reverse, m_voice.increment,
                                   m_voice.start, m_voice.end, m_voice.end - m_voice.start,
                                   m_voice.loop, m_voice.bidirectional) == SampleAdvance::Ended;
        // The sample is played with the coefficients and gains of this step even when it ends the voice.
        emit(r, input, gain_left, gain_right);
        if (ended) {
            m_voices.end_sample(m_voice, frame_tick);
            silence(r + 1, chunk_frames);
            return false;
        }
        ++r;
        return true;
    }
};

template <unsigned Mode>
void render_mode(VoiceService &voices, Voice &voice, SoundBank &bank, VoiceRenderState &state,
                 float *bus, size_t frame_start, size_t frame_end,
                 std::span<const uint64_t> frame_ticks) {
    VoiceSpan<Mode>(voices, voice, bank, state, bus, frame_ticks).render(frame_start, frame_end);
}

} // namespace

void reset_render_state(VoiceRenderState &state, uint64_t tick) {
    state.tick = tick;
    state.tick_remainder = 0;
}

void render_voice(VoiceService &voices, Voice &voice, SoundBank &bank, VoiceRenderState &state,
                  MutableAudioBlockSpan<8> buses, size_t frame_start, size_t frame_end,
                  std::span<const uint64_t> frame_ticks) {
    const size_t slot = voice.slot;
    if (state.filter_epoch[slot] != voice.filter_epoch) {
        state.filters[slot].clear();
        state.filter_epoch[slot] = voice.filter_epoch;
    }
    voices.set_render_voice(unsigned(slot));
    float *bus = nullptr;
    if (const auto select = otis_target_bus(voice.pair))
        bus = buses.data() + buses.slice(*select).channel_left();
    switch (filter_mode(voice.filter.mode)) {
    case 0:
        return render_mode<0>(voices, voice, bank, state, bus, frame_start, frame_end, frame_ticks);
    case 1:
        return render_mode<1>(voices, voice, bank, state, bus, frame_start, frame_end, frame_ticks);
    case 2:
        return render_mode<2>(voices, voice, bank, state, bus, frame_start, frame_end, frame_ticks);
    default:
        return render_mode<3>(voices, voice, bank, state, bus, frame_start, frame_end, frame_ticks);
    }
}

void render_voices(VoiceService &voices, SoundBank &bank, VoiceRenderState &state,
                   MutableAudioBlockSpan<8> buses, size_t frame_start, size_t frame_end,
                   std::span<const uint64_t> frame_ticks) {
    auto &pool = voices.pool();
    for (Voice &voice : pool) {
        if (!voice.active) continue;
        render_voice(voices, voice, bank, state, buses, frame_start, frame_end, frame_ticks);
    }
}

} // namespace f3rt::hle
