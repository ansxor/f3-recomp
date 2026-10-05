#pragma once
#include "hle_events.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <span>
#include <vector>

namespace f3rt::hle {
// A measured, deliberately non-bit-exact replacement for the factory DSP.
// No DSP instructions, registers, device emulator or captured audio are used.
class Effects {
    struct Line {
        std::vector<float> samples;
        size_t cursor = 0;
        float low = 0;
        explicit Line(size_t length) : samples(length) {}
        float read() const { return samples[cursor]; }
        void put(float value) { samples[cursor] = value; cursor = (cursor + 1) % samples.size(); }
        void clear() { std::fill(samples.begin(), samples.end(), 0); cursor = 0; low = 0; }
    };
    std::span<const uint8_t> rom_;
    std::array<uint8_t, 10> parameters_{};
    std::array<Line, 8> comb_{{Line(1613), Line(1733), Line(1867), Line(1999),
                              Line(1637), Line(1759), Line(1901), Line(2027)}};
    std::array<Line, 4> diffuse_{{Line(241), Line(379), Line(263), Line(401)}};
    std::array<Line, 2> delay_{{Line(96000), Line(96000)}};
    float decay_ = .7f, damping_ = .25f, feedback_ = .45f;
    float delay_samples_ = 13188, wet_delay_ = .65f, dry_delay_ = .35f;
    float wet_reverb_ = .25f, dry_reverb_ = .92f;
    float table(unsigned index) const {
        const size_t p = 0xa634 + std::min(index, 127u) * 2;
        return float(uint16_t(uint16_t(rom_[p]) << 8 | rom_[p + 1])) / 32768.f;
    }
    void update() {
        // Native delay address is Q8: p2 * 0x7000 / 256 = p2 * 112 samples.
        delay_samples_ = std::max(1.f, parameters_[2] * 112.f * sample_rate / 29761.f + 1.f);
        feedback_ = std::clamp(float(int8_t(parameters_[1])) / 128.f, -.95f, .95f);
        decay_ = std::clamp(.3f + parameters_[0] / 180.f, .3f, .94f);
        damping_ = std::clamp(((parameters_[6] >> 4) * (parameters_[6] >> 4) + 3.f) / 128.f, .02f, .95f);
        // Empirical scale from preset11's measured 4096 impulse: first
        // delay-route peak2663, first echo2094 after the .75 input damping.
        wet_delay_ = table(127 - parameters_[8]) * .70f;
        dry_delay_ = table(127 - parameters_[8]) * (2.f / 3.f);
        wet_reverb_ = table(parameters_[9]); dry_reverb_ = table(127 - parameters_[9]);
    }
    float reverb(float value, unsigned channel) {
        float sum = 0;
        for (unsigned i = 0; i < 4; ++i) {
            auto &line = comb_[channel * 4 + i];
            const float out = line.read();
            line.low += damping_ * (out - line.low);
            line.put(value + decay_ * line.low);
            sum += out * .25f;
        }
        for (unsigned i = 0; i < 2; ++i) {
            auto &line = diffuse_[channel * 2 + i];
            const float out = line.read() - sum * .5f;
            line.put(sum + out * .5f); sum = out;
        }
        return sum;
    }
public:
    explicit Effects(std::span<const uint8_t> rom) : rom_(rom) { reset(); }
    void select(uint8_t preset) {
        if (preset >= 13) return;
        // c0f504 restores the ten bytes in reverse order into c84a+2*n.
        for (unsigned n = 0; n < 10; ++n) parameters_[n] = rom_[0x1c2d2 + preset * 16 + 15 - n];
        update();
    }
    void parameter(uint8_t index, uint8_t value) {
        if (index < parameters_.size()) { parameters_[index] = value; update(); }
    }
    void reset() {
        for (auto &line : comb_) line.clear();
        for (auto &line : diffuse_) line.clear();
        for (auto &line : delay_) line.clear();
        select(0);
    }
    void mix(const float *buses, float *stereo, size_t frames,
             const std::array<float, 2> &input_gain, const std::array<float, 2> &output_gain) {
        for (size_t f = 0; f < frames; ++f) {
            float echoes[2];
            const float length = delay_samples_;
            for (unsigned ch = 0; ch < 2; ++ch) {
                auto &line = delay_[ch];
                const float p = float(line.cursor + line.samples.size()) - length;
                const size_t base = size_t(p); const float fraction = p - float(base);
                echoes[ch] = line.samples[base % line.samples.size()] * (1 - fraction) +
                    line.samples[(base + 1) % line.samples.size()] * fraction;
            }
            for (unsigned ch = 0; ch < 2; ++ch) {
                const float send_delay = buses[f * 8 + 4 + ch] * input_gain[ch];
                const float send_reverb = buses[f * 8 + 6 + ch] * input_gain[ch];
                auto &line = delay_[ch];
                line.low += .75f * (send_delay - line.low);
                line.put(line.low + feedback_ * echoes[ch ^ 1]);
                const float tail = reverb(send_reverb + send_delay * .25f + echoes[ch] * .2f, ch);
                const float dry = (buses[f * 8 + ch] + buses[f * 8 + 2 + ch]) * input_gain[ch];
                stereo[f * 2 + ch] = (dry + send_delay * dry_delay_ + echoes[ch] * wet_delay_ +
                    send_reverb * dry_reverb_ + tail * wet_reverb_) * output_gain[ch] * .5f;
            }
        }
    }
};
} // namespace f3rt::hle
