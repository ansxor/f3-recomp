#pragma once
#include "audio/hle/events.hpp"
#include <array>
#include <cstddef>
#include <stdexcept>

namespace f3rt::hle {

// The cross-service event bus. It carries only the events that need coordination between services or an external
// injection: notes, releases, track parameter writes and effect changes. Clock ticks, bank writes and reset are not
// events. A full mailbox throws: an event is never dropped.
class Mailbox {
public:
    static constexpr size_t CAPACITY = 1024;

    void clear() noexcept {
        m_head = m_tail = m_count = 0;
    }
    bool empty() const noexcept { return m_count == 0; }

    void push(Event event) {
        if (m_count == CAPACITY) throw std::runtime_error("HLE mailbox full");
        m_events[m_tail] = event;
        m_tail = (m_tail + 1) % CAPACITY;
        ++m_count;
    }

    // FIFO. Each event leaves the mailbox before `f` sees it, so `f` may push.
    template <class F> void drain(F &&f) {
        while (m_count) {
            Event event = m_events[m_head];
            m_head = (m_head + 1) % CAPACITY;
            --m_count;
            f(event);
        }
    }

private:
    std::array<Event, CAPACITY> m_events{};
    size_t m_head = 0, m_tail = 0, m_count = 0;
};

} // namespace f3rt::hle
