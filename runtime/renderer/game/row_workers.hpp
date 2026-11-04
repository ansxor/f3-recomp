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
inline constexpr unsigned COMPOSE_CHUNK_ROWS = 8;
constexpr unsigned compose_chunk_count() {
    return (geometry::END_LINE - geometry::FIRST_LINE + COMPOSE_CHUNK_ROWS - 1) /
           COMPOSE_CHUNK_ROWS;
}

class WorkWindow {
public:
    virtual ~WorkWindow() = default;
    virtual void begin() = 0;
    virtual void end() = 0;
};

using ChunkCounts = std::array<unsigned, 1 + MAX_COMPOSE_HELPERS>;

using ComposeClock = std::chrono::steady_clock;

struct HelperWindow {
    ComposeClock::time_point origin{};
    ComposeClock::duration period{0}, open{0};
    bool limited() const { return period.count() > 0; }
};

struct ComposeSchedule {
    HelperWindow synth_core, emulation_core;
};

struct ParticipantTrace {
    bool joined = false;
    unsigned chunks = 0;
    uint32_t start_us = 0;
    uint32_t end_us = 0;
    uint32_t waited_us = 0;
};

struct DispatchTrace {
    std::array<ParticipantTrace, 1 + MAX_COMPOSE_HELPERS> participants;
    uint32_t total_us = 0;
};

class RowWorkers {
public:
    RowWorkers();
    ~RowWorkers();
    RowWorkers(const RowWorkers &) = delete;
    RowWorkers &operator=(const RowWorkers &) = delete;

    void set_schedule(const ComposeSchedule &schedule);
    void run(unsigned count, const std::function<void(unsigned chunk)> &body,
             WorkWindow *window = nullptr);
    ChunkCounts chunk_counts() const;
    const DispatchTrace &trace() const { return m_trace; }

private:
    ParticipantTrace pull_chunks(unsigned participant, const std::function<void(unsigned)> &body,
                                 const HelperWindow &window) noexcept;
    const HelperWindow &window_for(unsigned helper) const;
    void work(unsigned slot);
    void stop();

    std::vector<Thread> m_threads;
    std::mutex m_submission_mutex, m_mutex;
    std::condition_variable m_ready, m_done, m_wake;
    std::atomic<unsigned> m_cursor{0};
    const std::function<void(unsigned)> *m_body = nullptr;
    unsigned m_count = 0;
    uint64_t m_generation = 0;
    unsigned m_remaining = 0;
    bool m_stopping = false;
    ComposeSchedule m_armed, m_schedule;
    ComposeClock::time_point m_start;
    DispatchTrace m_trace;
    std::array<int64_t, MAX_COMPOSE_HELPERS> m_chunk_ns{};
};

RowWorkers &compose_workers();
} // namespace f3rt
