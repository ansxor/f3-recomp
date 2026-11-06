#pragma once
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <span>
#include <vector>

namespace f3rt {
struct SceneSnapshot;

struct FrameSnapshot {
    SceneSnapshot *scene_snapshot = nullptr;
    size_t snapshot_index = 0;
    std::vector<uint32_t> pixels;
    uint32_t *rgba = nullptr;
    size_t stride = 0;
    uint64_t frame = 0, epoch = 0;
    unsigned scale = 1;
    std::chrono::steady_clock::time_point start;
};

class FrameSlots {
public:
    static constexpr int SLOT_COUNT = 3;
    void attach_snapshots(const std::array<SceneSnapshot *, SLOT_COUNT> &snapshots);
    void attach_buffers(std::span<uint32_t *> buffers, size_t stride);
    FrameSnapshot &begin();
    void publish();
    void restate(unsigned scale);
    const FrameSnapshot *take(bool &fresh);
    bool unseen() const;
    bool wait_unseen(const std::atomic<bool> &stop);
    void wake();

private:
    int free_slot() const;

    mutable std::mutex m_mutex;
    std::condition_variable m_cv;
    std::array<SceneSnapshot *, SLOT_COUNT> m_snapshots{};
    std::array<FrameSnapshot, SLOT_COUNT> m_slots;
    int m_latest = -1, m_held = -1, m_writing = -1;
    uint64_t m_published = 0, m_seen = 0;
};
} // namespace f3rt
