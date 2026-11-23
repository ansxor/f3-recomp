#pragma once
#include "f3rt/game_video.hpp"
#include "f3rt_video_cxx/video.h"
#include "renderer/game/scene_snapshot.hpp"
#include <cstdint>

namespace f3rt {
inline constexpr unsigned MOTION_LAYER_MASK = 0x80000000u;

// Bridge PODs (`rust_video::*`), shared with the Rust motion history.
using MotionInterpolationStats = rust_video::MotionInterpolationStats;
using MotionRowState = rust_video::MotionRowState;
using MotionState = rust_video::MotionState;
using MotionResult = rust_video::MotionResult;

// Render-only history. Allocate only when explicitly enabling temporal capture.
class GpuMotionHistory {
public:
    // Only options.border matters: it bounds layer clip ranges exactly as the GPU encoder does.
    explicit GpuMotionHistory(GameVideoOptions options = {});
    ~GpuMotionHistory();
    GpuMotionHistory(const GpuMotionHistory &) = delete;
    GpuMotionHistory &operator=(const GpuMotionHistory &) = delete;
    GpuMotionHistory(GpuMotionHistory &&) noexcept;
    GpuMotionHistory &operator=(GpuMotionHistory &&) noexcept;

    void capture(const SceneSnapshot &frame, uint64_t frame_id) noexcept;
    void reset() noexcept;
    // Writes the sampled geometry into caller-owned `out` (no 42 KiB return copy). `out` is
    // meaningful only when the returned stats.paired; an unpaired result leaves it untouched.
    MotionInterpolationStats apply_into(const SceneSnapshot &frame, float alpha,
                                        MotionState &out) const noexcept;
    MotionResult apply(const SceneSnapshot &frame, float alpha) const noexcept;

    const rust_video::GpuMotionHistory &inner() const { return *m_history; }

private:
    rust::Box<rust_video::GpuMotionHistory> m_history;
};

} // namespace f3rt
