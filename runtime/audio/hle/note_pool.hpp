#pragma once
#include "audio/hle/events.hpp"
#include <array>
#include <cstdint>
#include <stdexcept>

namespace f3rt::hle {

inline constexpr size_t NOTE_CAPACITY = 4096;
inline constexpr uint16_t NO_NOTE = 0xffff;

struct ActiveNote {
    uint64_t instance = 0;
    uint16_t next = NO_NOTE;
    uint16_t remaining = 0;
    uint8_t key = 0;
    uint8_t kernel_key = 0;
};

class NotePool {
public:
    NotePool() noexcept { reset(); }

    void reset() noexcept {
        m_free_head = 0;
        for (uint16_t n = 0; n < NOTE_CAPACITY; ++n) {
            m_notes[n] = {};
            m_notes[n].next = (n + 1 == NOTE_CAPACITY) ? NO_NOTE : uint16_t(n + 1);
        }
    }

    void allocate_note(uint16_t &track_head, uint64_t instance, uint8_t key, uint8_t kernel_key,
                       uint16_t duration) {
        if (m_free_head == NO_NOTE) {
            throw std::runtime_error("HLE note bookkeeping pool exhausted");
        }
        const uint16_t index = m_free_head;
        m_free_head = m_notes[index].next;

        auto &n = m_notes[index];
        n.instance = instance;
        n.key = key;
        n.kernel_key = kernel_key;
        n.remaining = duration;
        n.next = track_head;
        track_head = index;
    }

    template <typename Sink> void release_all(uint16_t &track_head, Sink &sink, uint64_t tick) {
        while (track_head != NO_NOTE) {
            const uint16_t n = track_head;
            track_head = m_notes[n].next;
            release_single(n, sink, tick);
        }
    }

    // Unlinks the first note of `kernel_key` and returns its instance id (0 when none). No sink: the caller releases
    // the voice itself.
    uint64_t release_matching_key(uint16_t &track_head, uint8_t kernel_key) {
        uint16_t *link = &track_head;
        while (*link != NO_NOTE) {
            const uint16_t n = *link;
            if (m_notes[n].kernel_key == kernel_key) {
                *link = m_notes[n].next;
                const uint64_t instance = m_notes[n].instance;
                release_entry(n);
                return instance;
            }
            link = &m_notes[n].next;
        }
        return 0;
    }

    template <typename Sink> void step_durations(uint16_t &track_head, Sink &sink, uint64_t tick) {
        uint16_t *link = &track_head;
        while (*link != NO_NOTE) {
            const uint16_t n = *link;
            if (m_notes[n].remaining) {
                --m_notes[n].remaining;
            }
            if (!m_notes[n].remaining) {
                *link = m_notes[n].next;
                release_single(n, sink, tick);
            } else {
                link = &m_notes[n].next;
            }
        }
    }

private:
    void release_entry(uint16_t index) {
        m_notes[index].next = m_free_head;
        m_free_head = index;
    }

    template <typename Sink> void release_single(uint16_t index, Sink &sink, uint64_t tick) {
        sink.note_off(m_notes[index].instance, tick);
        release_entry(index);
    }

    std::array<ActiveNote, NOTE_CAPACITY> m_notes{};
    uint16_t m_free_head = 0;
};

} // namespace f3rt::hle
