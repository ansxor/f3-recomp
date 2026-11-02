#pragma once
#include "f3rt/game_video.hpp"
#include "platform/thread.hpp"
#include <atomic>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <vector>

namespace f3rt {
// Composed rows are pulled in chunks of this many scanlines by every participant.
inline constexpr unsigned COMPOSE_CHUNK_ROWS = 8;
// Chunks covering the presented picture (geometry::FIRST_LINE .. END_LINE).
constexpr unsigned compose_chunk_count() {
    return (geometry::END_LINE - geometry::FIRST_LINE + COMPOSE_CHUNK_ROWS - 1) /
           COMPOSE_CHUNK_ROWS;
}

// Observes one participant's own share of a dispatch: begin() runs immediately before the caller
// pulls chunks and end() immediately after. Helper participants are not covered. Used by the 3DS
// frontend to sample hardware performance counters around the composer's own row work; a null
// window leaves the dispatch unobserved.
class WorkWindow {
public:
    virtual ~WorkWindow() = default;
    virtual void begin() = 0;
    virtual void end() = 0;
};

// Persistent helpers plus the caller. Participants pull chunks from a shared cursor, so a helper
// that only receives idle-time slices never stalls an otherwise finished frame, and the caller's
// own share shrinks as helpers finish early. `body` must stay alive and callable for the whole
// call; the dispatch returns only after every participant has finished it.
class RowWorkers {
public:
    RowWorkers();
    ~RowWorkers();
    RowWorkers(const RowWorkers &) = delete;
    RowWorkers &operator=(const RowWorkers &) = delete;

    void run(unsigned count, const std::function<void(unsigned chunk)> &body,
             WorkWindow *window = nullptr);
    // Participants (helpers plus the caller) that pulled at least one chunk of the last dispatch.
    unsigned participants() const { return m_participants.load(std::memory_order_relaxed); }

private:
    bool compose_chunks(const std::function<void(unsigned)> &body) const noexcept;
    void work();
    void stop();

    std::vector<Thread> m_threads;
    std::mutex m_submission_mutex, m_mutex;
    std::condition_variable m_ready, m_done;
    mutable std::atomic<unsigned> m_cursor{0};
    mutable std::atomic<unsigned> m_participants{0};
    const std::function<void(unsigned)> *m_body = nullptr;
    unsigned m_count = 0;
    uint64_t m_generation = 0;
    unsigned m_remaining = 0;
    bool m_stopping = false;
};

// The process-wide compose pool the row kernels share (one set of helper threads, submissions
// serialized internally). Created on first use.
RowWorkers &compose_workers();
} // namespace f3rt
