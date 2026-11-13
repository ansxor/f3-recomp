#pragma once
#include "f3rt/game_video.hpp"
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <mutex>
#include <thread>
#include <vector>

namespace f3rt {
inline constexpr unsigned MAX_COMPOSE_HELPERS = 3;
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

struct ParticipantTrace {
    bool joined = false;
    unsigned chunks = 0;
    uint32_t start_us = 0;
    uint32_t end_us = 0;
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

    void run(unsigned count, const std::function<void(unsigned chunk)> &body,
             WorkWindow *window = nullptr);
    ChunkCounts chunk_counts() const;
    const DispatchTrace &trace() const { return m_trace; }

private:
    ParticipantTrace pull_chunks(const std::function<void(unsigned)> &body) noexcept;
    void work(unsigned slot);
    void stop();

    std::vector<std::thread> m_threads;
    std::mutex m_submission_mutex, m_mutex;
    std::condition_variable m_ready, m_done;
    std::atomic<unsigned> m_cursor{0};
    const std::function<void(unsigned)> *m_body = nullptr;
    unsigned m_count = 0;
    uint64_t m_generation = 0;
    unsigned m_remaining = 0;
    bool m_stopping = false;
    ComposeClock::time_point m_start;
    DispatchTrace m_trace;
};

RowWorkers &compose_workers();
} // namespace f3rt
