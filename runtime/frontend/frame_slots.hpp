#pragma once
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace f3rt {
struct CapturedFrame;

// One emulated frame as the presenter may read it. `captured` names a capture buffer that stays
// immutable while published; `pixels` carries the CPU/FDP path.
struct FrameSnapshot {
    CapturedFrame *captured = nullptr;
    size_t capture = 0;
    std::vector<uint32_t> pixels;
    uint64_t frame = 0,
             epoch = 0; // epoch changes on every emulation discontinuity (load, resume, clock resync)
    unsigned scale = 1; // GameVideo GPU scale this frame was captured at
    std::chrono::steady_clock::time_point
        start; // scheduled emulation start of the frame (motion interpolation clock)
};

// Triple-buffered, latest-wins frame hand-off between the emulation side and the presenter, so a
// slow presenter drops stale frames and never delays emulation. Callers serialize every call under
// their own mutex and signal their own condition variable.
class FrameSlots {
public:
    static constexpr int SLOT_COUNT = 3;
    // Emulation side: capture buffers the presenter may hold, one per slot.
    std::array<CapturedFrame *, SLOT_COUNT> captures{};

    // Emulation thread: a slot (and capture buffer) the presenter cannot be reading.
    FrameSnapshot &begin();
    // Emulation thread: hand the slot written by begin() to the presenter.
    void publish();
    // Emulation thread: re-issue the newest frame with a new scale while no new frame is coming.
    void restate(unsigned scale);
    // Presenter: newest published snapshot (fresh when not yet seen) or the one it already holds.
    const FrameSnapshot *take(bool &fresh);
    bool unseen() const { return m_published != m_seen; }

private:
    std::array<FrameSnapshot, SLOT_COUNT> m_slots;
    int m_latest = -1, m_held = -1, m_writing = -1;
    uint64_t m_published = 0, m_seen = 0;
};
} // namespace f3rt
