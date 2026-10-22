#pragma once
#include "audio/hle/events.hpp"
#include <cstdint>
#include <vector>

namespace f3rt::hle {

// The shared 1 kHz service grid: every listener sees the same TIMER_TICKS boundaries, so the sequence tempo pulses,
// the sequence fade pulse and the voice ring's service tick (which VoiceService steps from the render at the first
// frame that reaches it) all sit on one grid. Listeners outlive the Clock and are never removed; the graph is built
// once, at construction.
class Clock {
public:
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void clock_tick(uint64_t tick) = 0;
    };

    // The first delivered tick is the first grid boundary after `origin`.
    explicit Clock(uint64_t origin = 0) : m_next((origin / TIMER_TICKS + 1) * TIMER_TICKS) {}

    // Re-anchors the grid: the next delivered tick is the first boundary after `tick`.
    void reset(uint64_t tick) noexcept { m_next = (tick / TIMER_TICKS + 1) * TIMER_TICKS; }

    uint64_t next_tick() const noexcept { return m_next; }

    void add(Listener &listener) { m_listeners.push_back(&listener); }

    // Notifies every listener with the current boundary, then moves to the next one.
    void advance_one() {
        const uint64_t tick = m_next;
        for (Listener *listener : m_listeners) listener->clock_tick(tick);
        m_next += TIMER_TICKS;
    }

private:
    std::vector<Listener *> m_listeners;
    uint64_t m_next;
};

} // namespace f3rt::hle
