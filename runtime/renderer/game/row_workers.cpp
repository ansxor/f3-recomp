#include "renderer/game/row_workers.hpp"
#include <algorithm>
#include <thread>
#include <utility>

namespace f3rt {
RowWorkers::RowWorkers() {
    const unsigned hardware = std::thread::hardware_concurrency();
    const unsigned count = hardware ? std::min(MAX_COMPOSE_HELPERS, hardware - 1) : 1;
    m_threads.reserve(count);
    try {
        for (unsigned i = 0; i < count; ++i)
            m_threads.emplace_back([this, i] { work(i); });
    } catch (...) {
        stop();
        throw;
    }
}

RowWorkers::~RowWorkers() { stop(); }

void RowWorkers::run(unsigned count, const std::function<void(unsigned)> &body,
                     WorkWindow *window) {
    std::unique_lock submission(m_submission_mutex);
    {
        std::lock_guard lock(m_mutex);
        m_body = &body;
        m_count = count;
        m_cursor.store(0, std::memory_order_relaxed);
        m_trace = {};
        m_remaining = 1;
        m_start = ComposeClock::now();
        ++m_generation;
    }
    m_ready.notify_all();
    if (window) window->begin();
    m_trace.participants[0] = pull_chunks(body);
    if (window) window->end();
    std::unique_lock lock(m_mutex);
    --m_remaining;
    m_done.wait(lock, [this] { return m_remaining == 0; });
    m_trace.total_us = uint32_t(
        std::chrono::duration_cast<std::chrono::microseconds>(ComposeClock::now() - m_start)
            .count());
    m_body = nullptr;
    m_count = 0;
}

ChunkCounts RowWorkers::chunk_counts() const {
    ChunkCounts counts;
    for (size_t i = 0; i < counts.size(); ++i) counts[i] = m_trace.participants[i].chunks;
    return counts;
}

ParticipantTrace RowWorkers::pull_chunks(const std::function<void(unsigned)> &body) noexcept {
    const unsigned count = m_count;
    const auto micros = [this](ComposeClock::time_point at) {
        return uint32_t(
            std::chrono::duration_cast<std::chrono::microseconds>(at - m_start).count());
    };
    ParticipantTrace share;
    share.joined = true;
    share.start_us = micros(ComposeClock::now());
    for (unsigned chunk; (chunk = m_cursor.fetch_add(1, std::memory_order_relaxed)) < count;) {
        body(chunk);
        ++share.chunks;
    }
    share.end_us = micros(ComposeClock::now());
    return share;
}

void RowWorkers::work(unsigned slot) {
    uint64_t observed = 0;
    std::unique_lock lock(m_mutex);
    for (;;) {
        m_ready.wait(lock, [this, observed] { return m_stopping || m_generation != observed; });
        if (m_stopping) return;
        observed = m_generation;
        if (!m_body || m_cursor.load(std::memory_order_relaxed) >= m_count) continue;
        const std::function<void(unsigned)> *body = m_body;
        ++m_remaining;
        lock.unlock();
        const ParticipantTrace share = pull_chunks(*body);
        lock.lock();
        m_trace.participants[1 + slot] = share;
        if (--m_remaining == 0) m_done.notify_one();
    }
}

RowWorkers &compose_workers() {
    static RowWorkers workers;
    return workers;
}

void RowWorkers::stop() {
    {
        std::lock_guard lock(m_mutex);
        m_stopping = true;
    }
    m_ready.notify_all();
    for (auto &thread : m_threads) if (thread.joinable()) thread.join();
    m_threads.clear();
}
} // namespace f3rt
