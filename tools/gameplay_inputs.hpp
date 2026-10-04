#pragma once

#include <array>
#include <cstdint>

namespace f3rt::test {

enum KeyIndex {
    KEY_UP = 0,
    KEY_DOWN = 1,
    KEY_LEFT = 2,
    KEY_RIGHT = 3,
    KEY_BUTTON1 = 4, // Z
    KEY_BUTTON2 = 5, // X
    KEY_BUTTON3 = 6  // C
};

struct ScheduleConfig {
    bool versus = true;
    uint32_t p1_coin_frame = 700;
    uint32_t p1_coin_duration = 20;
    uint32_t p2_coin_frame = 740;
    uint32_t p2_coin_duration = 20;
    uint32_t start_begin = 800;
    uint32_t start_end = 2400;
    uint32_t start_period = 90;
    uint32_t start_pulse_len = 5;
    uint32_t p2_start_offset = 45;
    uint32_t mashing_begin = 1200;
    uint32_t mashing_period = 6;
};

class GameplaySchedule {
public:
    explicit GameplaySchedule(uint64_t seed, ScheduleConfig config = {})
        : seed_(seed), config_(config) {
        reset();
    }

    void reset() {
        current_frame_ = 0;
        rng1_ = seed_;
        rng2_ = seed_ ^ 0x9e3779b97f4a7c15ULL;
        p1_keys_.fill(false);
        p2_keys_.fill(false);
        current_words_ = {};
    }

    uint64_t seed() const { return seed_; }
    const ScheduleConfig &config() const { return config_; }

    // Advance state to frame f and return [P1_InputWord, P2_InputWord].
    // Sequential calls with f = 0, 1, 2, ... are expected.
    // If f jumps forward, advances intermediate steps correctly.
    std::array<uint16_t, 2> step(uint64_t f) {
        if (f < current_frame_) {
            reset();
        }
        while (current_frame_ < f) {
            advance_internal(current_frame_);
            ++current_frame_;
        }
        advance_internal(f);
        current_frame_ = f + 1;
        return current_words_;
    }

    uint16_t player_input(unsigned slot, uint64_t f) {
        auto words = step(f);
        return words[slot & 1];
    }

    // Direct inspect of key states
    bool p1_key(KeyIndex k) const { return p1_keys_[k]; }
    bool p2_key(KeyIndex k) const { return p2_keys_[k]; }

private:
    void advance_internal(uint64_t f) {
        // Coin pulses (active-high bit 8)
        bool p1_coin = (f >= config_.p1_coin_frame &&
                        f < config_.p1_coin_frame + config_.p1_coin_duration);
        bool p2_coin = config_.versus &&
                       (f >= config_.p2_coin_frame &&
                        f < config_.p2_coin_frame + config_.p2_coin_duration);

        // Start pulses (active-high bit 7): must match original f % period
        bool p1_start = false;
        if (f >= config_.start_begin && f < config_.start_end) {
            uint32_t phase = uint32_t(f % config_.start_period);
            p1_start = (phase < config_.start_pulse_len);
        }

        bool p2_start = false;
        if (config_.versus && f >= config_.start_begin && f < config_.start_end) {
            uint32_t phase = uint32_t(f % config_.start_period);
            p2_start = (phase >= config_.p2_start_offset &&
                        phase < config_.p2_start_offset + config_.start_pulse_len);
        }

        // LCG button mashing
        if (f >= config_.mashing_begin && (f % config_.mashing_period == 0)) {
            rng1_ = rng1_ * 6364136223846793005ULL + 1442695040888963407ULL;
            unsigned k1 = (rng1_ >> 33) % 7;
            p1_keys_[k1] = ((rng1_ >> 20) & 1) != 0;

            if (config_.versus) {
                rng2_ = rng2_ * 6364136223846793005ULL + 1442695040888963407ULL;
                unsigned k2 = (rng2_ >> 33) % 7;
                p2_keys_[k2] = ((rng2_ >> 20) & 1) != 0;
            }
        }

        uint16_t w1 = (p1_coin ? 0x100 : 0) | (p1_start ? 0x80 : 0);
        for (unsigned i = 0; i < 7; ++i) {
            if (p1_keys_[i]) w1 |= uint16_t(1u << i);
        }

        uint16_t w2 = (p2_coin ? 0x100 : 0) | (p2_start ? 0x80 : 0);
        if (config_.versus) {
            for (unsigned i = 0; i < 7; ++i) {
                if (p2_keys_[i]) w2 |= uint16_t(1u << i);
            }
        }

        current_words_ = {w1, w2};
    }

    uint64_t seed_;
    ScheduleConfig config_;
    uint64_t current_frame_{0};
    uint64_t rng1_{0};
    uint64_t rng2_{0};
    std::array<bool, 7> p1_keys_{};
    std::array<bool, 7> p2_keys_{};
    std::array<uint16_t, 2> current_words_{};
};

} // namespace f3rt::test
