#include "renderer/game/row_workers.hpp"
#include <utility>

namespace f3rt {
namespace {
// Headroom a limited helper keeps on top of its chunk estimate: chunk times vary around their mean.
constexpr std::chrono::microseconds CHUNK_MARGIN{300};
// What a limited helper assumes a chunk costs until it has timed one.
constexpr int64_t INITIAL_CHUNK_NS = 500'000;
} // namespace

RowWorkers::RowWorkers() {
    const unsigned count = compose_helper_count();
    m_chunk_ns.fill(INITIAL_CHUNK_NS);
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

void RowWorkers::set_schedule(const ComposeSchedule &schedule) {
    std::lock_guard lock(m_mutex);
    m_armed = schedule;
}

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
        m_trace = {};
        m_schedule = std::exchange(m_armed, {});
        m_remaining = 1; // helpers add themselves when they join
        m_start = ComposeClock::now();
        ++m_generation;
    }
    m_ready.notify_all();
    if (window) window->begin();
    m_trace.participants[0] = pull_chunks(0, body, HelperWindow{});
    // The chunks are gone: helpers still waiting for a window have nothing left to wait for.
    { std::lock_guard lock(m_mutex); }
    m_wake.notify_all();
    if (window) window->end();
    std::unique_lock lock(m_mutex);
    --m_remaining;
    m_done.wait(lock, [this] { return m_remaining == 0; });
    m_trace.total_us = uint32_t(
        std::chrono::duration_cast<std::chrono::microseconds>(ComposeClock::now() - m_start)
            .count());
    // Withdrawing the body under the lock that saw m_remaining == 0 closes the dispatch: a helper
    // that wakes from here on finds no body and never joins.
    m_body = nullptr;
    m_count = 0;
}

ChunkCounts RowWorkers::chunk_counts() const {
    ChunkCounts counts;
    for (size_t i = 0; i < counts.size(); ++i) counts[i] = m_trace.participants[i].chunks;
    return counts;
}

ParticipantTrace RowWorkers::pull_chunks(unsigned participant,
                                         const std::function<void(unsigned)> &body,
                                         const HelperWindow &window) noexcept {
    const unsigned count = m_count;
    const auto micros = [this](ComposeClock::time_point at) {
        return uint32_t(
            std::chrono::duration_cast<std::chrono::microseconds>(at - m_start).count());
    };
    ParticipantTrace share;
    share.joined = true;
    auto now = ComposeClock::now();
    share.start_us = micros(now);
    if (!window.limited()) {
        for (unsigned chunk; (chunk = m_cursor.fetch_add(1, std::memory_order_relaxed)) < count;) {
            body(chunk);
            ++share.chunks;
        }
        now = ComposeClock::now();
    } else {
        // One clock read per chunk serves as the end of one and the claim time of the next.
        int64_t &estimate = m_chunk_ns[participant - 1];
        for (;;) {
            // Claim now when a chunk fits in the current window, else at the start of the next
            // one (or of the first, before it opens).
            auto claim_at = now;
            if (now < window.origin) {
                claim_at = window.origin;
            } else {
                const auto phase = (now - window.origin) % window.period;
                if (phase + std::chrono::nanoseconds(estimate) + CHUNK_MARGIN > window.open)
                    claim_at = now - phase + window.period;
            }
            if (claim_at > now) {
                std::unique_lock lock(m_mutex);
                m_wake.wait_until(lock, claim_at, [this, count] {
                    return m_cursor.load(std::memory_order_relaxed) >= count;
                });
                const auto woke = ComposeClock::now();
                share.waited_us += uint32_t(
                    std::chrono::duration_cast<std::chrono::microseconds>(woke - now).count());
                now = woke;
                if (m_cursor.load(std::memory_order_relaxed) >= count) break;
                continue;
            }
            const unsigned chunk = m_cursor.fetch_add(1, std::memory_order_relaxed);
            if (chunk >= count) break;
            body(chunk);
            ++share.chunks;
            const auto after = ComposeClock::now();
            estimate += ((after - now).count() - estimate) / 8;
            now = after;
        }
    }
    share.end_us = micros(now);
    return share;
}

const HelperWindow &RowWorkers::window_for(unsigned helper) const {
    static const HelperWindow unlimited;
    switch (compose_helper_contention(helper)) {
    case HelperContention::None:
        break;
    case HelperContention::Synth:
        return m_schedule.synth_core;
    case HelperContention::Emulation:
        return m_schedule.emulation_core;
    }
    return unlimited;
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
        const HelperWindow window = window_for(slot);
        ++m_remaining;
        lock.unlock();
        const ParticipantTrace share = pull_chunks(1 + slot, *body, window);
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
    for (auto &thread : m_threads) thread.join();
    m_threads.clear();
}
} // namespace f3rt
