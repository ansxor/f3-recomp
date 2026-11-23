#include "renderer/gpu/interp.hpp"

namespace f3rt {

InterpolationStats analyze_gpu_interpolation_into(const SceneSnapshot &frame,
                                                  GameVideoOptions options, VideoInterpolation mode,
                                                  InterpolationFields fields,
                                                  const MotionState *motion,
                                                  std::span<const uint64_t> tile_pen_masks,
                                                  InterpolationCoefficients &out) noexcept {
    const auto masks = rust::Slice<const uint64_t>(tile_pen_masks.data(), tile_pen_masks.size());
    const rust_video::GameVideoOptions opts{options.scale, options.border};
    if (motion) {
        return rust_video::analyze_gpu_interpolation_motion(frame.inner(), opts, mode, fields,
                                                            *motion, masks, out);
    }
    return rust_video::analyze_gpu_interpolation(frame.inner(), opts, mode, fields, masks, out);
}

InterpolationAnalysis analyze_gpu_interpolation(const SceneSnapshot &frame,
                                                GameVideoOptions options, VideoInterpolation mode,
                                                InterpolationFields fields,
                                                const MotionState *motion,
                                                std::span<const uint64_t> tile_pen_masks) noexcept {
    InterpolationAnalysis result{};
    result.stats = analyze_gpu_interpolation_into(frame, options, mode, fields, motion,
                                                  tile_pen_masks, result.coefficients);
    return result;
}

const char *interpolation_reason_name(InterpolationReason reason) noexcept {
    switch (reason) {
    case InterpolationReason::Off:
        return "off";
    case InterpolationReason::NativeScale:
        return "native-scale";
    case InterpolationReason::Oracle:
        return "oracle";
    case InterpolationReason::NoEffect:
        return "no-effect";
    case InterpolationReason::InvalidRows:
        return "invalid-rows";
    case InterpolationReason::DiscreteBoundary:
        return "discrete-boundary";
    case InterpolationReason::PaletteDiscontinuity:
        return "palette-discontinuity";
    case InterpolationReason::Applied:
        return "applied";
    }
    return "unknown";
}

InterpolationLog::InterpolationLog() : m_log(rust_video::new_interp_log()) {}
InterpolationLog::~InterpolationLog() = default;
InterpolationLog::InterpolationLog(InterpolationLog &&) noexcept = default;
InterpolationLog &InterpolationLog::operator=(InterpolationLog &&) noexcept = default;

void InterpolationLog::reset() noexcept { m_log->reset(); }

void InterpolationLog::observe(InterpolationConfig config, const InterpolationStats &stats) {
    m_log->log_if_changed(config.mode, config.fields, stats);
}

} // namespace f3rt
