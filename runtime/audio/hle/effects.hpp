#pragma once
#include "audio/hle/events.hpp"
#include "audio/hle/sound_rom_5a7e9117.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

namespace f3rt::hle {
constexpr size_t scale_tap(size_t n) {
    return std::max<size_t>(1, (n * size_t(SAMPLE_RATE) + 24000) / 48000);
}
// A measured, deliberately non-bit-exact replacement for the factory DSP.
// No DSP instructions, registers, device emulator or captured audio are used.
class Effects {
    struct Line {
        std::vector<float> samples;
        size_t cursor = 0;
        float low = 0;
        explicit Line(size_t length) : samples(length) {}
        float read() const { return samples[cursor]; }
        void put(float value) {
            samples[cursor] = value;
            if (++cursor == samples.size()) cursor = 0;
        }
        void clear() {
            std::fill(samples.begin(), samples.end(), 0);
            cursor = 0;
            low = 0;
        }
        void silence() {
            std::fill(samples.begin(), samples.end(), 0);
            low = 0;
        }
        void advance(size_t frames) { cursor = (cursor + frames) % samples.size(); }
    };
    std::span<const uint8_t> m_rom;
    std::array<uint8_t, 10> m_parameters{};
    static constexpr size_t K_DELAY_BUFFER_SIZE = f3rt::hle::scale_tap(96000);
    static constexpr size_t K_BUCKET_COUNT = 16;
    static constexpr size_t K_BUCKET_SIZE = f3rt::hle::scale_tap(6000); // 16 * K_BUCKET_SIZE = K_DELAY_BUFFER_SIZE
    static constexpr size_t scale_tap(size_t n) { return f3rt::hle::scale_tap(n); }

    std::array<Line, 8> m_comb{{Line(f3rt::hle::scale_tap(1613)), Line(f3rt::hle::scale_tap(1733)),
                                Line(f3rt::hle::scale_tap(1867)), Line(f3rt::hle::scale_tap(1999)),
                                Line(f3rt::hle::scale_tap(1637)), Line(f3rt::hle::scale_tap(1759)),
                                Line(f3rt::hle::scale_tap(1901)), Line(f3rt::hle::scale_tap(2027))}};
    std::array<Line, 4> m_diffuse{{Line(f3rt::hle::scale_tap(241)), Line(f3rt::hle::scale_tap(379)),
                                   Line(f3rt::hle::scale_tap(263)), Line(f3rt::hle::scale_tap(401))}};
    std::array<Line, 2> m_delay{{Line(K_DELAY_BUFFER_SIZE), Line(K_DELAY_BUFFER_SIZE)}};
    float m_decay = .7f, m_damping = .25f, m_feedback = .45f;
    float m_delay_samples = 13188, m_wet_delay = .65f, m_dry_delay = .35f;
    float m_wet_reverb = .25f, m_dry_reverb = .92f;

    // Threshold arithmetic: maximum output gain is Audio::output_boost (12.0) * route (3.2) = 38.4.
    // Wet mix scaling is 0.5, and wet path gain through comb/allpass/delay is bounded by ~3.0.
    // Maximum wet PCM contribution is <= line_peak * 3.0 * (38.4 * 0.5) * 32768 ~= line_peak * 1.89e6.
    // For wet contribution < 0.25 int16 LSB, line_peak < 0.25 / 1.89e6 ~= 1.32e-7.
    // We choose 1e-8f (~0.019 LSB max), safely decaying into silence well before int16 truncation.
    static constexpr float K_THRESHOLD = 1e-8f;

    std::array<float, K_BUCKET_COUNT> m_bucket_peaks{};
    size_t m_bucket_frames = 0;
    size_t m_bucket_idx = 0;
    float m_current_bucket_peak = 0.f;
    uint64_t m_silent_frames = K_DELAY_BUFFER_SIZE;
    bool m_bypassed = true;

    float table(unsigned index) const {
        const size_t p = sound_5a7e9117::VOLUME_PAN_TABLE_OFFSET + std::min(index, 127u) * 2;
        return float(uint16_t(uint16_t(m_rom[p]) << 8 | m_rom[p + 1])) / 32768.f;
    }
    void update() {
        // Native delay address is Q8: p2 * 0x7000 / 256 = p2 * 112 samples.
        m_delay_samples =
            std::max(1.f, float(m_parameters[2]) * 112.f * SAMPLE_RATE / 29761.f + 1.f);
        m_feedback = std::clamp(float(int8_t(m_parameters[1])) / 128.f, -.95f, .95f);
        m_decay = std::clamp(.3f + float(m_parameters[0]) / 180.f, .3f, .94f);
        const int damping_level = m_parameters[6] >> 4;
        m_damping = std::clamp((float(damping_level * damping_level) + 3.f) / 128.f, .02f, .95f);
        // Empirical scale from preset11's measured 4096 impulse: first
        // delay-route peak2663, first echo2094 after the .75 input damping.
        m_wet_delay = table(127 - m_parameters[8]) * .70f;
        m_dry_delay = table(127 - m_parameters[8]) * (2.f / 3.f);
        m_wet_reverb = table(m_parameters[9]);
        m_dry_reverb = table(127 - m_parameters[9]);
    }
    float reverb(float value, unsigned channel) {
        float sum = 0;
        for (unsigned i = 0; i < 4; ++i) {
            auto &line = m_comb[channel * 4 + i];
            const float out = line.read();
            line.low += m_damping * (out - line.low);
            line.put(value + m_decay * line.low);
            sum += out * .25f;
        }
        for (unsigned i = 0; i < 2; ++i) {
            auto &line = m_diffuse[channel * 2 + i];
            const float out = line.read() - sum * .5f;
            line.put(sum + out * .5f);
            sum = out;
        }
        return sum;
    }

    float render_reverb_channel(unsigned channel, size_t chunk, const float *in, float *tail) {
        const size_t c_base = size_t(channel) * 4;
        const size_t d_base = size_t(channel) * 2;
        float *c_s0 = m_comb[c_base + 0].samples.data();
        float *c_s1 = m_comb[c_base + 1].samples.data();
        float *c_s2 = m_comb[c_base + 2].samples.data();
        float *c_s3 = m_comb[c_base + 3].samples.data();
        const size_t c_size0 = m_comb[c_base + 0].samples.size();
        const size_t c_size1 = m_comb[c_base + 1].samples.size();
        const size_t c_size2 = m_comb[c_base + 2].samples.size();
        const size_t c_size3 = m_comb[c_base + 3].samples.size();
        size_t c_cur0 = m_comb[c_base + 0].cursor;
        size_t c_cur1 = m_comb[c_base + 1].cursor;
        size_t c_cur2 = m_comb[c_base + 2].cursor;
        size_t c_cur3 = m_comb[c_base + 3].cursor;
        float c_low0 = m_comb[c_base + 0].low;
        float c_low1 = m_comb[c_base + 1].low;
        float c_low2 = m_comb[c_base + 2].low;
        float c_low3 = m_comb[c_base + 3].low;

        float *d_s0 = m_diffuse[d_base + 0].samples.data();
        float *d_s1 = m_diffuse[d_base + 1].samples.data();
        const size_t d_size0 = m_diffuse[d_base + 0].samples.size();
        const size_t d_size1 = m_diffuse[d_base + 1].samples.size();
        size_t d_cur0 = m_diffuse[d_base + 0].cursor;
        size_t d_cur1 = m_diffuse[d_base + 1].cursor;

        float peak = 0.f;

        for (size_t f = 0; f < chunk; ++f) {
            const float val = in[f];

            const float out0 = c_s0[c_cur0];
            c_low0 += m_damping * (out0 - c_low0);
            const float w0 = val + m_decay * c_low0;
            c_s0[c_cur0] = w0;
            if (++c_cur0 == c_size0) c_cur0 = 0;

            const float out1 = c_s1[c_cur1];
            c_low1 += m_damping * (out1 - c_low1);
            const float w1 = val + m_decay * c_low1;
            c_s1[c_cur1] = w1;
            if (++c_cur1 == c_size1) c_cur1 = 0;

            const float out2 = c_s2[c_cur2];
            c_low2 += m_damping * (out2 - c_low2);
            const float w2 = val + m_decay * c_low2;
            c_s2[c_cur2] = w2;
            if (++c_cur2 == c_size2) c_cur2 = 0;

            const float out3 = c_s3[c_cur3];
            c_low3 += m_damping * (out3 - c_low3);
            const float w3 = val + m_decay * c_low3;
            c_s3[c_cur3] = w3;
            if (++c_cur3 == c_size3) c_cur3 = 0;

            peak = std::max({peak, std::abs(w0), std::abs(w1), std::abs(w2), std::abs(w3)});

            float sum = 0.f;
            sum += out0 * .25f;
            sum += out1 * .25f;
            sum += out2 * .25f;
            sum += out3 * .25f;

            const float out_d0 = d_s0[d_cur0] - sum * .5f;
            const float w_d0 = sum + out_d0 * .5f;
            d_s0[d_cur0] = w_d0;
            if (++d_cur0 == d_size0) d_cur0 = 0;

            const float out_d1 = d_s1[d_cur1] - out_d0 * .5f;
            const float w_d1 = out_d0 + out_d1 * .5f;
            d_s1[d_cur1] = w_d1;
            if (++d_cur1 == d_size1) d_cur1 = 0;

            peak = std::max({peak, std::abs(w_d0), std::abs(w_d1)});

            tail[f] = out_d1;
        }

        m_comb[c_base + 0].cursor = c_cur0;
        m_comb[c_base + 0].low = c_low0;
        m_comb[c_base + 1].cursor = c_cur1;
        m_comb[c_base + 1].low = c_low1;
        m_comb[c_base + 2].cursor = c_cur2;
        m_comb[c_base + 2].low = c_low2;
        m_comb[c_base + 3].cursor = c_cur3;
        m_comb[c_base + 3].low = c_low3;
        m_diffuse[d_base + 0].cursor = d_cur0;
        m_diffuse[d_base + 1].cursor = d_cur1;

        return peak;
    }

    // Physical PCB wiring from OTIS (ES5505) to ESP (ES5510) observed on hardware by researcher "12":
    // - docs/notes/raw/docs/taito-f3/5505-5510.txt (12's oscilloscope trace through TC0660FCM:
    //   OTIS ch 0 -> NC; OTIS ch 1 -> ESP ch 0 serio0 [Dry];
    //   OTIS ch 2 -> ESP ch 1 serio1 [Delay]; OTIS ch 3 -> ESP ch 2 serio2 [Reverb];
    //   ESP ch 3 serio3 -> DAC).
    //
    // Hardware wiring:
    // - OTIS pair 0 (ch 0/1, buses 0/1): unconnected (nc) -> silent, reaches nothing.
    // - OTIS pair 1 (ch 2/3, buses 2/3): feeds ESP ch 0 serio0 -> dry output.
    // - OTIS pair 2 (ch 4/5, buses 4/5): feeds ESP ch 1 serio1 -> delay send.
    // - OTIS pair 3 (ch 6/7, buses 6/7): feeds ESP ch 2 serio2 -> reverb send.
    // ESP ch 0 serio0 passes dry; ch 1 serio1 provides cross-delay; ch 2 serio2 provides reverb.
    static constexpr size_t K_BUS_DRY_L = 2;
    static constexpr size_t K_BUS_DRY_R = 3;
    static constexpr size_t K_BUS_DELAY_L = 4;
    static constexpr size_t K_BUS_DELAY_R = 5;
    static constexpr size_t K_BUS_REVERB_L = 6;
    static constexpr size_t K_BUS_REVERB_R = 7;
    float render_delay_block(size_t chunk, const float *buses,
                             const std::array<float, 2> &input_gain, float *reverb_in0,
                             float *reverb_in1, float *dry_term0, float *dry_term1) {
        const float length = m_delay_samples;
        float *d_s0 = m_delay[0].samples.data();
        float *d_s1 = m_delay[1].samples.data();
        size_t cur0 = m_delay[0].cursor;
        size_t cur1 = m_delay[1].cursor;
        float low0 = m_delay[0].low;
        float low1 = m_delay[1].low;
        float peak = 0.f;

        for (size_t f = 0; f < chunk; ++f) {
            const float p0 = float(cur0 + K_DELAY_BUFFER_SIZE) - length;
            const size_t base0 = size_t(p0);
            const float frac0 = p0 - float(base0);
            const size_t idx0 = base0 >= K_DELAY_BUFFER_SIZE ? base0 - K_DELAY_BUFFER_SIZE : base0;
            const size_t next0 = idx0 + 1 == K_DELAY_BUFFER_SIZE ? 0 : idx0 + 1;
            const float echo0 = d_s0[idx0] * (1 - frac0) + d_s0[next0] * frac0;

            const float p1 = float(cur1 + K_DELAY_BUFFER_SIZE) - length;
            const size_t base1 = size_t(p1);
            const float frac1 = p1 - float(base1);
            const size_t idx1 = base1 >= K_DELAY_BUFFER_SIZE ? base1 - K_DELAY_BUFFER_SIZE : base1;
            const size_t next1 = idx1 + 1 == K_DELAY_BUFFER_SIZE ? 0 : idx1 + 1;
            const float echo1 = d_s1[idx1] * (1 - frac1) + d_s1[next1] * frac1;

            const float send_d0 = buses[f * 8 + K_BUS_DELAY_L] * input_gain[0];
            const float send_r0 = buses[f * 8 + K_BUS_REVERB_L] * input_gain[0];
            low0 += .75f * (send_d0 - low0);
            const float w0 = low0 + m_feedback * echo1;
            d_s0[cur0] = w0;
            if (++cur0 == K_DELAY_BUFFER_SIZE) cur0 = 0;

            reverb_in0[f] = send_r0 + send_d0 * .25f + echo0 * .2f;
            const float dry0 = buses[f * 8 + K_BUS_DRY_L] * input_gain[0];
            dry_term0[f] =
                dry0 + send_d0 * m_dry_delay + echo0 * m_wet_delay + send_r0 * m_dry_reverb;

            const float send_d1 = buses[f * 8 + K_BUS_DELAY_R] * input_gain[1];
            const float send_r1 = buses[f * 8 + K_BUS_REVERB_R] * input_gain[1];
            low1 += .75f * (send_d1 - low1);
            const float w1 = low1 + m_feedback * echo0;
            d_s1[cur1] = w1;
            if (++cur1 == K_DELAY_BUFFER_SIZE) cur1 = 0;

            reverb_in1[f] = send_r1 + send_d1 * .25f + echo1 * .2f;
            const float dry1 = buses[f * 8 + K_BUS_DRY_R] * input_gain[1];
            dry_term1[f] =
                dry1 + send_d1 * m_dry_delay + echo1 * m_wet_delay + send_r1 * m_dry_reverb;

            peak = std::max({peak, std::abs(w0), std::abs(w1)});
        }

        m_delay[0].cursor = cur0;
        m_delay[0].low = low0;
        m_delay[1].cursor = cur1;
        m_delay[1].low = low1;
        return peak;
    }

    // Post-DAC analog reconstruction filter and DC blocker
    std::array<float, 2> m_dc_x1{0.f, 0.f};
    std::array<float, 2> m_dc_y1{0.f, 0.f};
    std::array<float, 2> m_lp_z1{0.f, 0.f};
    std::array<float, 2> m_lp_z2{0.f, 0.f};
    float m_filter_r = 0.f;
    float m_lp_b0 = 0.f, m_lp_b1 = 0.f, m_lp_b2 = 0.f;
    float m_lp_a1 = 0.f, m_lp_a2 = 0.f;

    void init_post_filter() {
        constexpr float PI = 3.14159265358979323846f;
        const float fs = float(SAMPLE_RATE);
        m_filter_r = std::exp(-2.0f * PI * 20.0f / fs);
        const float K = std::tan(PI * 13800.0f / fs);
        const float norm = 1.0f / (1.0f + std::sqrt(2.0f) * K + K * K);
        m_lp_b0 = K * K * norm;
        m_lp_b1 = 2.0f * m_lp_b0;
        m_lp_b2 = m_lp_b0;
        m_lp_a1 = 2.0f * (K * K - 1.0f) * norm;
        m_lp_a2 = (1.0f - std::sqrt(2.0f) * K + K * K) * norm;
    }

    void post_filter(float *stereo, size_t frames) {
        if (frames == 0) return;
        const bool silent_filter =
            (std::abs(m_dc_x1[0]) < K_THRESHOLD && std::abs(m_dc_x1[1]) < K_THRESHOLD &&
             std::abs(m_dc_y1[0]) < K_THRESHOLD && std::abs(m_dc_y1[1]) < K_THRESHOLD &&
             std::abs(m_lp_z1[0]) < K_THRESHOLD && std::abs(m_lp_z1[1]) < K_THRESHOLD &&
             std::abs(m_lp_z2[0]) < K_THRESHOLD && std::abs(m_lp_z2[1]) < K_THRESHOLD);
        if (silent_filter) {
            bool all_zero = true;
            for (size_t i = 0; i < frames * 2; ++i) {
                if (stereo[i] != 0.f) {
                    all_zero = false;
                    break;
                }
            }
            if (all_zero) return;
        }

        const float R = m_filter_r;
        const float b0 = m_lp_b0, b1 = m_lp_b1, b2 = m_lp_b2;
        const float a1 = m_lp_a1, a2 = m_lp_a2;

        for (size_t f = 0; f < frames; ++f) {
            for (size_t ch = 0; ch < 2; ++ch) {
                const float x = stereo[f * 2 + ch];
                const float hp = x - m_dc_x1[ch] + R * m_dc_y1[ch];
                m_dc_x1[ch] = x;
                m_dc_y1[ch] = hp;

                const float y = b0 * hp + m_lp_z1[ch];
                m_lp_z1[ch] = b1 * hp - a1 * y + m_lp_z2[ch];
                m_lp_z2[ch] = b2 * hp - a2 * y;

                stereo[f * 2 + ch] = y;
            }
        }

        for (size_t ch = 0; ch < 2; ++ch) {
            if (std::abs(m_dc_x1[ch]) < K_THRESHOLD) m_dc_x1[ch] = 0.f;
            if (std::abs(m_dc_y1[ch]) < K_THRESHOLD) m_dc_y1[ch] = 0.f;
            if (std::abs(m_lp_z1[ch]) < K_THRESHOLD) m_lp_z1[ch] = 0.f;
            if (std::abs(m_lp_z2[ch]) < K_THRESHOLD) m_lp_z2[ch] = 0.f;
        }
    }

public:
    explicit Effects(std::span<const uint8_t> rom) : m_rom(rom) {
        init_post_filter();
        reset();
    }
    void select(uint8_t preset) {
        if (preset >= 13) return;
        // c0f504 restores the ten bytes in reverse order into c84a+2*n.
        for (unsigned n = 0; n < 10; ++n)
            m_parameters[n] = m_rom[sound_5a7e9117::PRESET_TABLE_OFFSET + preset * 16 + 15 - n];
        update();
    }
    void parameter(uint8_t index, uint8_t value) {
        if (index < m_parameters.size()) {
            m_parameters[index] = value;
            update();
        }
    }
    void reset() {
        for (auto &line : m_comb) line.clear();
        for (auto &line : m_diffuse) line.clear();
        for (auto &line : m_delay) line.clear();
        m_bucket_peaks.fill(0.f);
        m_current_bucket_peak = 0.f;
        m_bucket_frames = 0;
        m_silent_frames = K_DELAY_BUFFER_SIZE;
        m_dc_x1.fill(0.f);
        m_dc_y1.fill(0.f);
        m_lp_z1.fill(0.f);
        m_lp_z2.fill(0.f);
        m_bypassed = true;
        select(0);
    }
    void mix(const float *buses, float *stereo, size_t frames,
             const std::array<float, 2> &input_gain, const std::array<float, 2> &output_gain) {
        if (frames == 0) return;

        bool silent_sends = true;
        for (size_t f = 0; f < frames; ++f) {
            if (buses[f * 8 + K_BUS_DELAY_L] != 0.f || buses[f * 8 + K_BUS_DELAY_R] != 0.f ||
                buses[f * 8 + K_BUS_REVERB_L] != 0.f || buses[f * 8 + K_BUS_REVERB_R] != 0.f) {
                silent_sends = false;
                break;
            }
        }

        if (m_bypassed) {
            if (silent_sends) {
                for (auto &line : m_comb) line.advance(frames);
                for (auto &line : m_diffuse) line.advance(frames);
                for (auto &line : m_delay) line.advance(frames);
                for (size_t f = 0; f < frames; ++f) {
                    const float dry0 = buses[f * 8 + K_BUS_DRY_L] * input_gain[0];
                    const float dry1 = buses[f * 8 + K_BUS_DRY_R] * input_gain[1];
                    stereo[f * 2 + 0] = dry0 * output_gain[0] * .5f;
                    stereo[f * 2 + 1] = dry1 * output_gain[1] * .5f;
                }
                post_filter(stereo, frames);
                return;
            }
            m_bypassed = false;
            m_silent_frames = 0;
        }

        if (!silent_sends) {
            m_silent_frames = 0;
        } else {
            m_silent_frames += frames;
        }

        alignas(32) float reverb_in[2][256];
        alignas(32) float dry_term[2][256];
        alignas(32) float tail[2][256];

        size_t offset = 0;
        while (offset < frames) {
            const size_t chunk = std::min<size_t>(frames - offset, 256);
            const float *b_ptr = buses + offset * 8;
            float *s_ptr = stereo + offset * 2;

            const float d_peak = render_delay_block(chunk, b_ptr, input_gain, reverb_in[0],
                                                    reverb_in[1], dry_term[0], dry_term[1]);
            const float ch0_peak = render_reverb_channel(0, chunk, reverb_in[0], tail[0]);
            const float ch1_peak = render_reverb_channel(1, chunk, reverb_in[1], tail[1]);

            for (size_t f = 0; f < chunk; ++f) {
                s_ptr[f * 2 + 0] =
                    (dry_term[0][f] + tail[0][f] * m_wet_reverb) * output_gain[0] * .5f;
                s_ptr[f * 2 + 1] =
                    (dry_term[1][f] + tail[1][f] * m_wet_reverb) * output_gain[1] * .5f;
            }

            const float chunk_peak = std::max({d_peak, ch0_peak, ch1_peak});
            m_current_bucket_peak = std::max(m_current_bucket_peak, chunk_peak);
            m_bucket_frames += chunk;
            if (m_bucket_frames >= K_BUCKET_SIZE) {
                m_bucket_peaks[m_bucket_idx] = m_current_bucket_peak;
                m_bucket_idx = (m_bucket_idx + 1) % K_BUCKET_COUNT;
                m_current_bucket_peak = 0.f;
                m_bucket_frames = 0;
            }

            offset += chunk;
        }

        post_filter(stereo, frames);

        if (m_silent_frames >= K_DELAY_BUFFER_SIZE) {
            float peak = m_current_bucket_peak;
            for (float b : m_bucket_peaks) peak = std::max(peak, b);
            if (peak < K_THRESHOLD) {
                for (auto &line : m_comb) line.silence();
                for (auto &line : m_diffuse) line.silence();
                for (auto &line : m_delay) line.silence();
                m_bucket_peaks.fill(0.f);
                m_current_bucket_peak = 0.f;
                m_bucket_frames = 0;
                m_bypassed = true;
            }
        }
    }
};
} // namespace f3rt::hle
