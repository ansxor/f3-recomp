#include "audio/hle/synthesis.hpp"
#include "audio/hle/voice.hpp"
#include <algorithm>
#include <cmath>

namespace f3rt::hle {
namespace {

double sample_at(const SoundBank &bank, size_t at) {
    if (bank.samples.empty()) return 0;
    return int16_t(bank.samples[at & (bank.samples.size() - 1)]);
}

void render_voice_block(VoiceService &voices, Voice &v, SoundBank &bank, VoiceRenderState &state,
                        size_t chunk_start, size_t chunk_frames, double samples_limit,
                        std::span<const uint64_t> frame_ticks) {
    float *input = state.input;
    float *a1 = state.a1;
    float *a2 = state.a2;
    float *high_pole = state.high_pole;
    float *gain_left = state.gain_left;
    float *gain_right = state.gain_right;
    const double length = v.end - v.start, increment = v.increment;
    auto fill_remaining = [&](size_t start_row) {
        for (size_t idx = start_row; idx < chunk_frames; ++idx) {
            input[idx] = 0.f;
            gain_left[idx] = 0.f;
            gain_right[idx] = 0.f;
            a1[idx] = float(v.filter.a1);
            a2[idx] = float(v.filter.a2);
            high_pole[idx] = float(v.filter.high_pole);
        }
    };
    size_t r = 0;
    while (r < chunk_frames) {
        size_t n = 0;
        const bool ramping = v.gain_frames > 0;
        if (!v.sample_finished && v.filter.ramp == 0 &&
            (!ramping ||
             (v.left_segment.frames > 0 && v.right_segment.frames > 0 && v.gain_frames > 1))) {
            bool pos_ok = false;
            double pos_bound = 0;
            if (!v.reverse) {
                if (v.position >= 0 && v.position < samples_limit - 1 &&
                    v.end < samples_limit - 1) {
                    if (increment > 0) {
                        pos_bound = std::floor((v.end - v.position) / increment) - 1.0;
                        pos_ok = (pos_bound >= 1.0);
                    }
                }
            } else {
                if (v.position >= 0 && v.start >= 0 && v.position < samples_limit - 1) {
                    if (increment > 0) {
                        pos_bound = std::floor((v.position - v.start) / increment) - 1.0;
                        pos_ok = (pos_bound >= 1.0);
                    }
                }
            }
            if (pos_ok) {
                n = chunk_frames - r;
                if (ramping) {
                    n = std::min(n, size_t(v.left_segment.frames));
                    n = std::min(n, size_t(v.right_segment.frames));
                    n = std::min(n, size_t(v.gain_frames - 1));
                }
                n = std::min(n, size_t(pos_bound));
            }
        }
        const bool slow = n < 1;
        if (slow) {
            const size_t frame = chunk_start + r;
            voices.set_render_frame(frame);
            const uint64_t frame_tick = frame_ticks[frame];
            if (v.gain_frames) {
                v.left += v.step_left;
                v.right += v.step_right;
                if (!--v.gain_frames) {
                    v.left = v.target_left;
                    v.right = v.target_right;
                    v.gain_dirty = true;
                } else {
                    v.left_gain = ramp_gain(v.left, v.step_left, v.left_segment);
                    v.right_gain = ramp_gain(v.right, v.step_right, v.right_segment);
                    v.gain_dirty = false;
                }
            }
            if (v.sample_finished) {
                input[r] = 0.f;
                gain_left[r] = 0.f;
                gain_right[r] = 0.f;
                a1[r] = float(v.filter.a1);
                a2[r] = float(v.filter.a2);
                high_pole[r] = float(v.filter.high_pole);
                ++r;
                continue;
            }
            if (v.position < 0 || v.position >= samples_limit) {
                v.active = false; // out-of-range phase: drop the voice silently, as the ROM does.
                fill_remaining(r);
                break;
            }
            const size_t index = size_t(v.position);
            const double fraction = v.position - double(index);
            const double first =
                (!v.loop && v.reverse && index < size_t(v.start)) ? 0.0 : sample_at(bank, index);
            const double second = (!v.loop && !v.reverse && index + 1 >= size_t(v.end))
                                      ? 0.0
                                      : sample_at(bank, index + 1);
            input[r] = interpolate_sample(first, second, fraction);
            v.filter.advance();
            a1[r] = float(v.filter.a1);
            a2[r] = float(v.filter.a2);
            high_pole[r] = float(v.filter.high_pole);
            if (v.gain_dirty) {
                v.left_gain = interpolated_gain(v.left);
                v.right_gain = interpolated_gain(v.right);
                v.gain_dirty = false;
            }
            const float scale = 1.f / 524288.f;
            gain_left[r] = v.left_gain * scale;
            gain_right[r] = v.right_gain * scale;
            if (advance_voice_position(v.position, v.reverse, increment, v.start, v.end, length,
                                       v.loop, v.bidirectional) == SampleAdvance::Ended) {
                voices.end_sample(v, frame_tick);
                fill_remaining(r + 1);
                break;
            }
            ++r;
        } else {
            const float a1c = float(v.filter.a1);
            const float a2c = float(v.filter.a2);
            const float hpc = float(v.filter.high_pole);
            double pos = v.position;
            const double inc = v.reverse ? -increment : increment;
            if (ramping) {
                double v_left = v.left, v_right = v.right;
                double seg_l_val = v.left_segment.value;
                const double seg_l_slope = v.left_segment.slope;
                double seg_r_val = v.right_segment.value;
                const double seg_r_slope = v.right_segment.slope;
                const double step_l = v.step_left, step_r = v.step_right;
                const float scale = 1.f / 524288.f;
                for (size_t k = 0; k < n; ++k) {
                    const size_t idx = r + k;
                    const size_t i = size_t(pos);
                    const double frac = pos - double(i);
                    input[idx] = interpolate_sample(int16_t(bank.samples[i]),
                                                    int16_t(bank.samples[i + 1]), frac);
                    pos += inc;
                    a1[idx] = a1c;
                    a2[idx] = a2c;
                    high_pole[idx] = hpc;
                    v_left += step_l;
                    v_right += step_r;
                    seg_l_val += seg_l_slope;
                    seg_r_val += seg_r_slope;
                    gain_left[idx] = float(seg_l_val) * scale;
                    gain_right[idx] = float(seg_r_val) * scale;
                }
                v.left = v_left;
                v.right = v_right;
                v.left_segment.frames -= unsigned(n);
                v.left_segment.value = seg_l_val;
                v.right_segment.frames -= unsigned(n);
                v.right_segment.value = seg_r_val;
                v.gain_frames -= unsigned(n);
                v.left_gain = float(seg_l_val);
                v.right_gain = float(seg_r_val);
                v.gain_dirty = false;
            } else {
                if (v.gain_dirty) {
                    v.left_gain = interpolated_gain(v.left);
                    v.right_gain = interpolated_gain(v.right);
                    v.gain_dirty = false;
                }
                const float gl = v.left_gain * (1.f / 524288.f);
                const float gr = v.right_gain * (1.f / 524288.f);
                for (size_t k = 0; k < n; ++k) {
                    const size_t idx = r + k;
                    const size_t i = size_t(pos);
                    const double frac = pos - double(i);
                    input[idx] = interpolate_sample(int16_t(bank.samples[i]),
                                                    int16_t(bank.samples[i + 1]), frac);
                    pos += inc;
                    a1[idx] = a1c;
                    a2[idx] = a2c;
                    high_pole[idx] = hpc;
                    gain_left[idx] = gl;
                    gain_right[idx] = gr;
                }
            }
            v.position = pos;
            voices.set_render_frame(chunk_start + r + n - 1);
            r += n;
        }
    }
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
        state.filters[slot].instanceClear();
        state.filter_epoch[slot] = voice.filter_epoch;
    }
    *state.mode_zone[slot] = float(voice.filter.mode);
    voices.set_render_voice(unsigned(slot));
    const double samples_limit = double(bank.samples.size());
    for (size_t chunk_start = frame_start; chunk_start < frame_end;
         chunk_start += VOICE_BLOCK_FRAMES) {
        const size_t chunk_frames = std::min(VOICE_BLOCK_FRAMES, frame_end - chunk_start);
        float *ins[6] = {state.input,     state.a1,         state.a2,
                         state.high_pole, state.gain_left, state.gain_right};
        render_voice_block(voices, voice, bank, state, chunk_start, chunk_frames, samples_limit,
                           frame_ticks);
        state.filters[slot].compute(int(chunk_frames), ins, state.filter_out.data());
        if (auto bus = otis_target_bus(voice.pair)) {
            buses.subspan(chunk_start, chunk_frames).slice(*bus) += state.filter_out;
        }
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
