#include "inputs.hpp"

namespace f3rt::runner {

void apply_gameplay_word(Machine &m, uint16_t word) {
    constexpr std::array<uint32_t, 7> masks{1, 2, 4, 8, 1, 2, 4};
    for (unsigned i = 0; i < 7; ++i) {
        m.set_input(i < 4 ? 1 : 0, masks[i], (word & (1u << i)) != 0);
    }
    m.set_input(0, 0x1000, (word & 0x80) != 0);
    if ((word & 0x100) != 0) {
        m.system_inputs &= ~0x10u;
    } else {
        m.system_inputs |= 0x10u;
    }
}

ScriptInputSource::ScriptInputSource(InputScript script)
    : script_(std::move(script)) {}

std::unique_ptr<ScriptInputSource> ScriptInputSource::from_file(const std::string &path) {
    return std::make_unique<ScriptInputSource>(InputScript::load(path));
}

std::unique_ptr<ScriptInputSource> ScriptInputSource::from_text(std::string_view text, const std::string &source) {
    return std::make_unique<ScriptInputSource>(InputScript::parse(text, source));
}

void ScriptInputSource::apply(Machine &machine, uint64_t frame) {
    auto words = script_.words(frame + 1);
    apply_local_inputs(machine, words);
    script_.poke(machine, frame + 1);
}

SeededScheduleInputSource::SeededScheduleInputSource(uint64_t seed, ScheduleConfig config)
    : seed_(seed), config_(config) {
    reset();
}

void SeededScheduleInputSource::reset() {
    current_frame_ = 0;
    rng1_ = seed_;
    rng2_ = seed_ ^ 0x9e3779b97f4a7c15ULL;
    p1_keys_.fill(false);
    p2_keys_.fill(false);
    current_words_ = {};
}

void SeededScheduleInputSource::advance_internal(uint64_t f) {
    bool p1_coin = (f >= config_.p1_coin_frame &&
                    f < config_.p1_coin_frame + config_.p1_coin_duration);
    bool p2_coin = config_.versus &&
                   (f >= config_.p2_coin_frame &&
                    f < config_.p2_coin_frame + config_.p2_coin_duration);

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

std::array<uint16_t, 2> SeededScheduleInputSource::step(uint64_t f) {
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

void SeededScheduleInputSource::apply(Machine &machine, uint64_t frame) {
    auto words = step(frame);
    apply_gameplay_word(machine, words[0]);
}

} // namespace f3rt::runner
