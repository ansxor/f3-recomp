#pragma once

// Greedy piecewise-linear simplification of a sampled curve with a vertical tolerance (tools/webplayer/README.md,
// section 4.8): the swing filter. The curve starts at an implicit onset point; the result lists the kept points
// after it, so that linear interpolation through the onset and the kept points stays within `tolerance` of every
// input sample.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <span>
#include <vector>

namespace f3rt::webplayer {

struct LanePoint {
    uint32_t frame = 0;
    int32_t value = 0;
    bool operator==(const LanePoint &) const = default;
};

// `samples` must have strictly increasing frames, all greater than `frame0`. Values of the result are rounded and
// clamped to [lo, hi]; the input samples are expected to lie in that range. Returns no points when every sample is
// within `tolerance` of `value0` (the lane is constant at its onset value). Otherwise the last point sits on the
// last sample's frame. `tolerance` must be >= 1 (integer value units; rounding of the kept values uses 0.5 of it).
inline std::vector<LanePoint> simplify_lane(uint32_t frame0, int32_t value0, std::span<const LanePoint> samples,
                                            int32_t tolerance, int32_t lo, int32_t hi) {
    std::vector<LanePoint> out;
    bool flat = true;
    for (const LanePoint &p : samples)
        if (std::abs(int64_t(p.value) - value0) > tolerance) {
            flat = false;
            break;
        }
    if (flat) return out;

    const double budget = double(tolerance) - 0.5;
    double ax = double(frame0), ay = double(value0);
    double slope_lo = -std::numeric_limits<double>::infinity();
    double slope_hi = std::numeric_limits<double>::infinity();
    auto close = [&](uint32_t end_frame) {
        const double slope = (slope_lo + slope_hi) / 2;
        const double predicted = ay + slope * (double(end_frame) - ax);
        const int32_t value = int32_t(std::clamp<double>(std::round(predicted), lo, hi));
        out.push_back({end_frame, value});
        ax = double(end_frame);
        ay = double(value);
        slope_lo = -std::numeric_limits<double>::infinity();
        slope_hi = std::numeric_limits<double>::infinity();
    };
    size_t i = 0;
    while (i < samples.size()) {
        const double dx = double(samples[i].frame) - ax;
        const double y = double(samples[i].value);
        const double low = (y - budget - ay) / dx;
        const double high = (y + budget - ay) / dx;
        const double next_lo = std::max(slope_lo, low), next_hi = std::min(slope_hi, high);
        if (next_lo <= next_hi) {
            slope_lo = next_lo;
            slope_hi = next_hi;
            ++i;
        } else {
            close(samples[i - 1].frame); // the first sample after an anchor is always feasible, so i > 0 here
        }
    }
    close(samples.back().frame);
    return out;
}

// Linear interpolation of the curve (frame0, value0) + points at `frame`; constant outside, after the last point.
inline double reconstruct_lane(uint32_t frame0, int32_t value0, std::span<const LanePoint> points, uint32_t frame) {
    double px = double(frame0), py = double(value0);
    for (const LanePoint &p : points) {
        if (frame <= p.frame) {
            const double dx = double(p.frame) - px;
            return dx <= 0 ? double(p.value) : py + (double(p.value) - py) * (double(frame) - px) / dx;
        }
        px = double(p.frame);
        py = double(p.value);
    }
    return py;
}

} // namespace f3rt::webplayer
