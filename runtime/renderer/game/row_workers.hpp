#pragma once
#include "f3rt/game_video.hpp"
#include "platform/thread.hpp"
#include <array>
#include <atomic>
#include <chrono>
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

using ComposeClock = std::chrono::steady_clock;

// When a helper may compose: during [origin + n * period, origin + n * period + open) for every
// n >= 0. A helper claims a chunk only when it expects to finish it inside the current window,
// and otherwise waits for the next one, so the thread it would delay (the one that fills the rest
// of each period) never starts late by more than a chunk misestimate. The default window is
// always open.
struct HelperWindow {
    ComposeClock::time_point origin{};
    ComposeClock::duration period{0}, open{0};
    bool limited() const { return period.count() > 0; }
};

// The windows of the helpers by what their core is shared with (compose_helper_contention()).
struct ComposeSchedule {
    HelperWindow synth_core, emulation_core;
};

// One participant's share of a dispatch. Offsets count microseconds from the start of the dispatch.
struct ParticipantTrace {
    bool joined = false;     // took part at all (the caller always does)
    unsigned chunks = 0;
    uint32_t start_us = 0;   // first chunk claim
    uint32_t end_us = 0;     // end of the last chunk, or the stop when no chunk was taken
    uint32_t waited_us = 0;  // of that, spent waiting for a helper window to open
};

// Who did what in the last dispatch: slot 0 is the caller, slot 1 + i helper i.
struct DispatchTrace {
    std::array<ParticipantTrace, 1 + MAX_COMPOSE_HELPERS> participants;
    uint32_t total_us = 0; // the whole run(), including the caller's wait for the helpers
};

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

    // Helper windows for the next run() only (later dispatches are unconstrained again). The
    // caller of run() is never limited.
    void set_schedule(const ComposeSchedule &schedule);
    void run(unsigned count, const std::function<void(unsigned chunk)> &body,
             WorkWindow *window = nullptr);
    // Per-participant chunk counts and timing of the last dispatch; read them after run() returns,
    // before the next one.
    ChunkCounts chunk_counts() const;
    const DispatchTrace &trace() const { return m_trace; }

private:
    // One participant's pull loop; `window` limits when it may take chunks.
    ParticipantTrace pull_chunks(unsigned participant, const std::function<void(unsigned)> &body,
                                 const HelperWindow &window) noexcept;
    const HelperWindow &window_for(unsigned helper) const;
    void work(unsigned slot);
    void stop();

    std::vector<Thread> m_threads;
    std::mutex m_submission_mutex, m_mutex;
    // m_ready starts a dispatch, m_done ends it, m_wake lets a helper that waits for its window
    // leave as soon as the chunks are gone.
    std::condition_variable m_ready, m_done, m_wake;
    std::atomic<unsigned> m_cursor{0};
    const std::function<void(unsigned)> *m_body = nullptr;
    unsigned m_count = 0;
    uint64_t m_generation = 0;
    unsigned m_remaining = 0; // the caller plus the helpers that joined the current dispatch
    bool m_stopping = false;
    ComposeSchedule m_armed, m_schedule; // for the next dispatch, and for the open one
    ComposeClock::time_point m_start;      // start of the open dispatch
    // Each participant fills its own slot before leaving the dispatch; run() reads them after the
    // last one has (the mutex hands them over).
    DispatchTrace m_trace;
    // Running mean chunk time per helper, in nanoseconds; only that helper's thread touches it.
    std::array<int64_t, MAX_COMPOSE_HELPERS> m_chunk_ns{};
};

// The process-wide compose pool the row kernels share (one set of helper threads, submissions
// serialized internally). Created on first use.
RowWorkers &compose_workers();
} // namespace f3rt
