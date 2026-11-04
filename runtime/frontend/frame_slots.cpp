#include "frontend/frame_slots.hpp"

namespace f3rt {
int FrameSlots::free_slot() const {
    int slot = 0;
    while (slot == m_latest || slot == m_held) ++slot;
    return slot;
}
void FrameSlots::attach_captures(const std::array<CapturedFrame *, SLOT_COUNT> &captures) {
    std::lock_guard lock(m_mutex);
    m_captures = captures;
}
void FrameSlots::attach_buffers(std::span<uint32_t *> buffers, size_t stride) {
    std::lock_guard lock(m_mutex);
    for (size_t i = 0; i < buffers.size() && i < m_slots.size(); ++i) {
        m_slots[i].rgba = buffers[i];
        m_slots[i].stride = stride;
    }
}
FrameSnapshot &FrameSlots::begin() {
    std::lock_guard lock(m_mutex);
    const int slot = free_slot();
    FrameSnapshot &s = m_slots[slot];
    s.captured = nullptr;
    if (m_captures[0]) {
        size_t capture = 0;
        while ((m_latest >= 0 && capture == m_slots[m_latest].capture) ||
               (m_held >= 0 && capture == m_slots[m_held].capture))
            ++capture;
        s.capture = capture;
        s.captured = m_captures[capture];
    }
    m_writing = slot;
    return s;
}
void FrameSlots::publish() {
    {
        std::lock_guard lock(m_mutex);
        m_latest = m_writing;
        ++m_published;
    }
    m_cv.notify_all();
}
void FrameSlots::restate(unsigned scale) {
    {
        std::lock_guard lock(m_mutex);
        if (m_latest < 0) return;
        const FrameSnapshot &latest = m_slots[m_latest];
        const int slot = free_slot();
        FrameSnapshot &s = m_slots[slot];
        s.captured = latest.captured;
        s.capture = latest.capture;
        s.frame = latest.frame;
        s.epoch = latest.epoch;
        s.start = latest.start;
        s.scale = scale;
        m_latest = slot;
        ++m_published;
    }
    m_cv.notify_all();
}
const FrameSnapshot *FrameSlots::take(bool &fresh) {
    std::lock_guard lock(m_mutex);
    fresh = m_latest >= 0 && m_published != m_seen;
    if (fresh) {
        m_held = m_latest;
        m_seen = m_published;
    }
    return m_held >= 0 ? &m_slots[m_held] : nullptr;
}
bool FrameSlots::unseen() const {
    std::lock_guard lock(m_mutex);
    return m_published != m_seen;
}
bool FrameSlots::wait_unseen(const std::atomic<bool> &stop) {
    std::unique_lock lock(m_mutex);
    m_cv.wait(lock, [&] { return stop.load() || m_published != m_seen; });
    return !stop.load();
}
void FrameSlots::wake() {
    {
        std::lock_guard lock(m_mutex);
    }
    m_cv.notify_all();
}
} // namespace f3rt
