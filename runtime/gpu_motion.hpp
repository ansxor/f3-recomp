#pragma once
#include "gpu_scene.hpp"
#include <array>
#include <span>

namespace f3rt {
inline constexpr unsigned motion_layer_mask = 0x80000000u;
struct MotionInterpolationStats {
    unsigned sprites = 0, playfield_rows = 0, text_rows = 0;
    bool paired = false;
    float alpha = 1.0f;
};
// Render-only history. Allocate only when explicitly enabling temporal capture.
// Coordinates are 24.8; interpolation rounds to the nearest fixed-point unit.
class GpuMotionHistory {
public:
    void capture(const GpuScene &scene, uint64_t frame) noexcept;
    void reset() noexcept;
    MotionInterpolationStats apply(const GpuScene &scene, float alpha,
                                  std::span<uint32_t> upload_words) const noexcept;
private:
    struct Sprite {
        std::array<uint32_t, 7> words{};
        int32_t dx = 0, dy = 0;
        bool eligible = false;
    };
    struct Row {
        std::array<uint32_t, 11> controls{};
        std::array<std::array<uint32_t, 35>, 5> layers{};
        std::array<uint32_t, 26> geometry{};
        std::array<int32_t, 10> delta{};
        std::array<bool, 5> eligible{};
        bool bitmap = false;
    };
    static Row read_row(const GpuScene &scene, unsigned y) noexcept;
    std::array<Sprite, 1024> sprites_{};
    std::array<Row, 232> rows_{};
    uint64_t frame_ = 0;
    unsigned sprite_count_ = 0, pen_mask_ = 0;
    bool captured_ = false, paired_ = false;
};
} // namespace f3rt
