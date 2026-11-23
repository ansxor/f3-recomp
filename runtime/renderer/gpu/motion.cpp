#include "renderer/gpu/motion.hpp"

namespace f3rt {

GpuMotionHistory::GpuMotionHistory(GameVideoOptions options)
    : m_history(rust_video::new_motion_history({options.scale, options.border})) {}
GpuMotionHistory::~GpuMotionHistory() = default;
GpuMotionHistory::GpuMotionHistory(GpuMotionHistory &&) noexcept = default;
GpuMotionHistory &GpuMotionHistory::operator=(GpuMotionHistory &&) noexcept = default;

void GpuMotionHistory::capture(const SceneSnapshot &frame, uint64_t frame_id) noexcept {
    m_history->capture(frame.inner(), frame_id);
}

void GpuMotionHistory::reset() noexcept { m_history->reset(); }

MotionInterpolationStats GpuMotionHistory::apply_into(const SceneSnapshot &frame, float alpha,
                                                      MotionState &out) const noexcept {
    auto produced = MotionState{};
    const auto stats = m_history->apply_into(frame.inner(), alpha, produced);
    if (stats.paired) out = std::move(produced);
    return stats;
}

MotionResult GpuMotionHistory::apply(const SceneSnapshot &frame, float alpha) const noexcept {
    return m_history->apply(frame.inner(), alpha);
}

} // namespace f3rt
