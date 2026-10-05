#pragma once
#include "gpu_scene.hpp"
#include <array>
#include <span>

namespace f3rt {
inline constexpr unsigned motion_layer_mask = 0x80000000u;
struct MotionInterpolationStats {
    unsigned sprites = 0, playfield_rows = 0, text_rows = 0;
    // Native-pair census, independent of alpha and repeated apply calls.
    // Moving sprites require a known match; unknown identities are separate rejects.
    // Row counts are layer/scanline pairs; a partially accepted row can also reject
    // its other moving axis. Unchanged geometry never contributes to motion/rejects.
    unsigned moving_sprites = 0, moving_playfield_rows = 0, moving_text_rows = 0;
    unsigned rejected_sprites = 0, rejected_rows = 0;
    unsigned sprite_count_rejections = 0, sprite_identity_rejections = 0;
    // Count rejections are the subset of unknown identities on count-changing pairs,
    // not a veto of the remaining matched sprites.
    unsigned sprite_transform_rejections = 0, sprite_jump_rejections = 0;
    unsigned row_control_rejections = 0, row_transform_rejections = 0, row_jump_rejections = 0;
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
        std::array<bool, 10> eligible{};
        bool bitmap = false;
    };
    static Row read_row(const GpuScene &scene, unsigned y) noexcept;
    std::array<Sprite, 1024> sprites_{};
    std::array<Row, 232> rows_{};
    uint64_t frame_ = 0;
    unsigned sprite_count_ = 0, pen_mask_ = 0;
    bool captured_ = false, paired_ = false;
    MotionInterpolationStats pair_stats_{};
};
} // namespace f3rt
