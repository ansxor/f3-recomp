#pragma once
#include "audio/hle/clock.hpp"
#include <cstdint>
#include <utility>
#include <vector>

namespace f3rt::hle {

// A rate accumulator on the 1 kHz grid: every clock tick adds `step`, and each time the sum reaches `period` the
// timer pulses. `period` 625 is one musical pulse at the ROM's tempo unit, 12 is the sequence fade pulse. A step of
// zero never pulses.
class PulseTimer : public Clock::Listener {
public:
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void pulse(uint64_t tick, uint8_t tag) = 0;
    };

    PulseTimer(uint32_t period = 625, uint32_t step = 0) : m_period(period), m_step(step) {}

    void reset() noexcept { m_accumulator = 0; }
    void set_period(uint32_t period) noexcept { m_period = period; }
    void set_step(uint32_t step) noexcept { m_step = step; }

    // Registrations happen while the graph is built; listeners outlive the timer.
    void add(Listener &listener, uint8_t tag) { m_listeners.emplace_back(&listener, tag); }

    void clock_tick(uint64_t tick) override {
        m_accumulator += m_step;
        if (m_accumulator < m_period) return;
        m_accumulator -= m_period;
        for (const auto &[listener, tag] : m_listeners) listener->pulse(tick, tag);
    }

private:
    uint32_t m_period, m_step, m_accumulator = 0;
    std::vector<std::pair<Listener *, uint8_t>> m_listeners;
};

} // namespace f3rt::hle
