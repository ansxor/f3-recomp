#pragma once
#include "f3rt/game_video.hpp"
#include "platform/thread.hpp"
#include <array>
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

// Chunks pulled by each participant of one dispatch: slot 0 is the caller, slot 1 + i helper i. A
// helper that did not join the dispatch reads zero.
using ChunkCounts = std::array<unsigned, 1 + MAX_COMPOSE_HELPERS>;

// Persistent helpers plus the caller. Participants pull chunks from a shared cursor, and the caller
// waits only for the helpers that joined: a helper that has not been scheduled by the time the
// chunks run out (or the dispatch ends) finds nothing to do and never delays the frame, and the
// caller's own share shrinks as helpers join. `body` must stay alive and callable for the whole
// call; the dispatch returns only after every joined participant has finished it.
class RowWorkers {
public:
    RowWorkers();
    ~RowWorkers();
    RowWorkers(const RowWorkers &) = delete;
    RowWorkers &operator=(const RowWorkers &) = delete;

    void run(unsigned count, const std::function<void(unsigned chunk)> &body,
             WorkWindow *window = nullptr);
    // Per-participant chunk counts of the last dispatch; read them after run() returns, before the
    // next one.
    ChunkCounts chunk_counts() const;

private:
    unsigned compose_chunks(const std::function<void(unsigned)> &body) const noexcept;
    void work(unsigned slot);
    void stop();

    std::vector<Thread> m_threads;
    std::mutex m_submission_mutex, m_mutex;
    std::condition_variable m_ready, m_done;
    mutable std::atomic<unsigned> m_cursor{0};
    std::array<std::atomic<unsigned>, 1 + MAX_COMPOSE_HELPERS> m_pulled{};
    const std::function<void(unsigned)> *m_body = nullptr;
    unsigned m_count = 0;
    uint64_t m_generation = 0;
    unsigned m_remaining = 0; // the caller plus the helpers that joined the current dispatch
    bool m_stopping = false;
};

// The process-wide compose pool the row kernels share (one set of helper threads, submissions
// serialized internally). Created on first use.
RowWorkers &compose_workers();
} // namespace f3rt
