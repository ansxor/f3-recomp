#include "renderer/game/row_workers.hpp"

namespace f3rt {
RowWorkers::RowWorkers() {
    const unsigned count = compose_helper_count();
    m_threads.reserve(count);
    try {
        for (unsigned i = 0; i < count; ++i)
            m_threads.emplace_back(ThreadRole::ComposeHelper, i, [this] { work(); });
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
        m_participants.store(0, std::memory_order_relaxed);
        m_remaining = unsigned(m_threads.size()) + 1;
        ++m_generation;
    }
    m_ready.notify_all();
    if (window) window->begin();
    if (compose_chunks(body)) m_participants.fetch_add(1, std::memory_order_relaxed);
    if (window) window->end();
    std::unique_lock lock(m_mutex);
    if (--m_remaining == 0) m_done.notify_one();
    else m_done.wait(lock, [this] { return m_remaining == 0; });
    m_body = nullptr;
    m_count = 0;
}

bool RowWorkers::compose_chunks(const std::function<void(unsigned)> &body) const noexcept {
    const unsigned count = m_count;
    bool pulled = false;
    for (unsigned chunk; (chunk = m_cursor.fetch_add(1, std::memory_order_relaxed)) < count;) {
        body(chunk);
        pulled = true;
    }
    return pulled;
}

void RowWorkers::work() {
    uint64_t observed = 0;
    std::unique_lock lock(m_mutex);
    for (;;) {
        m_ready.wait(lock, [this, observed] { return m_stopping || m_generation != observed; });
        if (m_stopping) return;
        observed = m_generation;
        const std::function<void(unsigned)> *body = m_body;
        lock.unlock();
        if (compose_chunks(*body)) m_participants.fetch_add(1, std::memory_order_relaxed);
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
