#pragma once
#include "audio/hle/events.hpp"
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>

namespace f3rt::hle {

// The sound driver programs the ES5505 voices of a sequencer pulse one after another on a CPU that is also
// servicing the 1 kHz timer interrupt, so the Nth voice of a busy pulse sounds later than the first. The clock
// models that single resource: a request made at `tick` for `voices` voices starts at max(tick, busy) and
// keeps the driver busy for voices * DRIVER_KEYON_CYCLES afterwards. Requests are made in the order the driver
// handles them (pulse order, track-major), which is the order VoiceService::note_on is called in.
class DriverClock {
public:
    uint64_t start(uint64_t tick, unsigned voices) {
        const uint64_t at = std::max(tick, m_busy);
        m_busy = at + uint64_t(voices) * DRIVER_KEYON_CYCLES;
        return at;
    }
    uint64_t busy_until() const { return m_busy; }
    void reset(uint64_t tick) { m_busy = tick; }

private:
    uint64_t m_busy = 0;
};

// Notes whose start the driver clock pushed past the tick they were requested at. Every queued entry starts
// exactly once, in (start, request) order. A note-off that arrives before the start is remembered and applied
// at the start plus the note's original length, so it can neither drop the note nor release it early.
template <class Payload, size_t Capacity> class NoteQueue {
public:
    static constexpr uint64_t NONE = std::numeric_limits<uint64_t>::max();

    bool push(const Payload &payload, uint64_t instance, uint64_t origin, uint64_t start) {
        for (Entry &e : m_entries) {
            if (e.state != State::Free) continue;
            e = {payload, instance, origin, start, 0, m_serial++, State::Waiting, false};
            ++m_used;
            return true;
        }
        return false;
    }
    bool empty() const { return m_used == 0; }
    size_t size() const { return m_used; }
    // Earliest tick at which service() has something to do.
    uint64_t next_event() const {
        uint64_t next = NONE;
        for (const Entry &e : m_entries) {
            if (e.state == State::Waiting) next = std::min(next, e.start);
            else if (e.state == State::Started) next = std::min(next, e.off);
        }
        return next;
    }
    void note_off(uint64_t instance, uint64_t at) {
        for (Entry &e : m_entries) {
            if (e.state == State::Free || e.instance != instance || e.has_off) continue;
            e.has_off = true;
            e.off = at + (e.start - std::min(e.start, e.origin));
        }
    }
    void clear() {
        for (Entry &e : m_entries) e.state = State::Free;
        m_used = 0;
    }
    // begin(payload, start) once per entry whose start <= limit; release(instance, tick) once per entry whose
    // note-off arrived, never before its begin. Order: ascending tick, begins before releases at equal ticks.
    template <class Begin, class Release> void service(uint64_t limit, Begin &&begin, Release &&release) {
        for (;;) {
            Entry *pick = nullptr;
            bool pick_release = false;
            for (Entry &e : m_entries) {
                if (e.state == State::Free) continue;
                const bool rel = e.state == State::Started;
                const uint64_t tick = rel ? e.off : e.start;
                if (tick > limit) continue;
                if (!pick) {
                    pick = &e;
                    pick_release = rel;
                    continue;
                }
                const uint64_t best = pick_release ? pick->off : pick->start;
                if (tick < best || (tick == best && (rel < pick_release ||
                                                     (rel == pick_release && e.serial < pick->serial)))) {
                    pick = &e;
                    pick_release = rel;
                }
            }
            if (!pick) return;
            Entry e = *pick;
            if (pick_release) {
                pick->state = State::Free;
                --m_used;
                release(e.instance, e.off);
                continue;
            }
            if (e.has_off) pick->state = State::Started;
            else {
                pick->state = State::Free;
                --m_used;
            }
            begin(e.payload, e.start);
        }
    }

private:
    enum class State : uint8_t { Free, Waiting, Started };
    struct Entry {
        Payload payload{};
        uint64_t instance = 0, origin = 0, start = 0, off = 0, serial = 0;
        State state = State::Free;
        bool has_off = false;
    };
    std::array<Entry, Capacity> m_entries{};
    uint64_t m_serial = 0;
    size_t m_used = 0;
};

} // namespace f3rt::hle
