#include "renderer/game/row_workers.hpp"

namespace f3rt {
RowWorkers::RowWorkers() {
    const unsigned count = compose_helper_count();
    m_threads.reserve(count);
    try {
        for (unsigned i = 0; i < count; ++i)
            m_threads.emplace_back(ThreadRole::ComposeHelper, i, [this, i] { work(i); });
    } catch (...) {
        stop();
        throw;
    }
}

RowWorkers::~RowWorkers() { stop(); }

void RowWorkers::run(unsigned count, const std::function<void(unsigned)> &body,
                     WorkWindow *window) {
    // Serialize submissions, not sampling: caller-owned bodies remain alive
    // until every participant has finished, including concurrent callers.
    std::unique_lock submission(m_submission_mutex);
    {
        std::lock_guard lock(m_mutex);
        m_body = &body;
        m_count = count;
        m_cursor.store(0, std::memory_order_relaxed);
        for (auto &pulled : m_pulled) pulled.store(0, std::memory_order_relaxed);
        m_remaining = 1; // helpers add themselves when they join
        ++m_generation;
    }
    m_ready.notify_all();
    if (window) window->begin();
    m_pulled[0].store(compose_chunks(body), std::memory_order_relaxed);
    if (window) window->end();
    std::unique_lock lock(m_mutex);
    --m_remaining;
    m_done.wait(lock, [this] { return m_remaining == 0; });
    // Withdrawing the body under the lock that saw m_remaining == 0 closes the dispatch: a helper
    // that wakes from here on finds no body and never joins.
    m_body = nullptr;
    m_count = 0;
}

ChunkCounts RowWorkers::chunk_counts() const {
    ChunkCounts counts;
    for (size_t i = 0; i < counts.size(); ++i)
        counts[i] = m_pulled[i].load(std::memory_order_relaxed);
    return counts;
}

unsigned RowWorkers::compose_chunks(const std::function<void(unsigned)> &body) const noexcept {
    const unsigned count = m_count;
    unsigned pulled = 0;
    for (unsigned chunk; (chunk = m_cursor.fetch_add(1, std::memory_order_relaxed)) < count;) {
        body(chunk);
        ++pulled;
    }
    return pulled;
}

void RowWorkers::work(unsigned slot) {
    uint64_t observed = 0;
    std::unique_lock lock(m_mutex);
    for (;;) {
        m_ready.wait(lock, [this, observed] { return m_stopping || m_generation != observed; });
        if (m_stopping) return;
        observed = m_generation;
        // Join only a dispatch that is still open and still has chunks to hand out.
        if (!m_body || m_cursor.load(std::memory_order_relaxed) >= m_count) continue;
        const std::function<void(unsigned)> *body = m_body;
        ++m_remaining;
        lock.unlock();
        m_pulled[1 + slot].store(compose_chunks(*body), std::memory_order_relaxed);
        lock.lock();
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
    for (auto &thread : m_threads) thread.join();
    m_threads.clear();
}
} // namespace f3rt
