#pragma once

#include "f3rt/machine.hpp"
#include "f3rt/input.hpp"
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>

namespace f3rt::runner {

// Applies standard 16-bit gameplay word (bits 0..3 dirs, 4..6 buttons, 7 start, 8 coin) to machine.
void apply_gameplay_word(Machine &m, uint16_t word);

class InputSource {
public:
    virtual ~InputSource() = default;
    virtual void apply(Machine &machine, uint64_t frame) = 0;
};

class NullInputSource : public InputSource {
public:
    void apply(Machine &, uint64_t) override {}
};

class ScriptInputSource : public InputSource {
public:
    explicit ScriptInputSource(InputScript script);
    static std::unique_ptr<ScriptInputSource> from_file(const std::string &path);
    static std::unique_ptr<ScriptInputSource> from_text(std::string_view text, const std::string &source = "text");
    void apply(Machine &machine, uint64_t frame) override;
    const InputScript &script() const { return script_; }

private:
    InputScript script_;
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

// Seeded PRNG LCG gameplay scheduler (formerly GameplaySchedule in gameplay_inputs.hpp)
class SeededScheduleInputSource : public InputSource {
public:
    explicit SeededScheduleInputSource(uint64_t seed, ScheduleConfig config = {});
    void apply(Machine &machine, uint64_t frame) override;
    void reset();
    std::array<uint16_t, 2> step(uint64_t frame);

    uint64_t seed() const { return seed_; }
    const ScheduleConfig &config() const { return config_; }

private:
    void advance_internal(uint64_t f);

    uint64_t seed_;
    ScheduleConfig config_;
    uint64_t current_frame_{0};
    uint64_t rng1_{0};
    uint64_t rng2_{0};
    std::array<bool, 7> p1_keys_{};
    std::array<bool, 7> p2_keys_{};
    std::array<uint16_t, 2> current_words_{};
};

} // namespace f3rt::runner

namespace f3rt::test {
using ScheduleConfig = runner::ScheduleConfig;
using GameplaySchedule = runner::SeededScheduleInputSource;
} // namespace f3rt::test

