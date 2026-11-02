#include "frontend/frame_slots.hpp"

namespace f3rt {
FrameSnapshot &FrameSlots::begin() {
    int slot = 0;
    while (slot == m_latest || slot == m_held) ++slot;
    FrameSnapshot &s = m_slots[slot];
    s.captured = nullptr;
    if (captures[0]) {
        size_t c = 0;
        while ((m_latest >= 0 && c == m_slots[m_latest].capture) ||
               (m_held >= 0 && c == m_slots[m_held].capture))
            ++c;
        s.capture = c;
        s.captured = captures[c];
    }
    m_writing = slot;
    return s;
}
void FrameSlots::publish() {
    m_latest = m_writing;
    ++m_published;
}
void FrameSlots::restate(unsigned scale) {
    if (m_latest < 0) return;
    int slot = 0;
    while (slot == m_latest || slot == m_held) ++slot;
    m_slots[slot].captured = m_slots[m_latest].captured;
    m_slots[slot].capture = m_slots[m_latest].capture;
    m_slots[slot].frame = m_slots[m_latest].frame;
    m_slots[slot].epoch = m_slots[m_latest].epoch;
    m_slots[slot].start = m_slots[m_latest].start;
    m_slots[slot].scale = scale;
    m_latest = slot;
    ++m_published;
}
void FrameSlots::attach_buffers(std::span<uint32_t *> buffers, size_t stride) {
    for (size_t i = 0; i < buffers.size() && i < m_slots.size(); ++i) {
        m_slots[i].rgba = buffers[i];
        m_slots[i].stride = stride;
    }
}
const FrameSnapshot *FrameSlots::take(bool &fresh) {
    fresh = m_latest >= 0 && m_published != m_seen;
    if (fresh) {
        m_held = m_latest;
        m_seen = m_published;
    }
    return m_held >= 0 ? &m_slots[m_held] : nullptr;
}
} // namespace f3rt
