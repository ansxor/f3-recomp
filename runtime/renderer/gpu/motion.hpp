#pragma once
#include "f3rt/game_video.hpp"
#include "renderer/game/captured_frame.hpp"
#include "renderer/game/clip.hpp"
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
// Temporal geometry for one present: the scene's motion-affected fields at
// the sampled alpha, in 24.8 fixed point. Initialized to the canonical frame
// values, so any field motion did not accept equals the captured one. Text
// scroll is carried here because SceneRow only holds integer scroll.
struct MotionState {
    struct Row {
        std::array<int32_t, 4> source_x{}; // playfield source x
        std::array<int32_t, 4> phase{};    // playfield source_y * 256 + y_fraction
        int32_t text_x = 0, text_y = 0;
    };
    std::array<int32_t, CapturedFrame::max_sprites> sprite_x{}, sprite_y{};
    std::array<Row, 256> rows{};
};
struct MotionResult {
    MotionInterpolationStats stats;
    MotionState state; // meaningful only when stats.paired
};
// Render-only history. Allocate only when explicitly enabling temporal capture.
// Coordinates are 24.8; interpolation rounds to the nearest fixed-point unit.
class GpuMotionHistory {
public:
    // Only options.border matters: it bounds layer clip ranges exactly as the
    // GPU encoder does, so history compares the same visible ranges.
    explicit GpuMotionHistory(GameVideoOptions options = {}) noexcept : border_(int(options.border)) {}
    void capture(const CapturedFrame &frame, uint64_t frame_id) noexcept;
    void reset() noexcept;
    MotionResult apply(const CapturedFrame &frame, float alpha) const noexcept;
private:
    struct Sprite {
        SceneSprite value;
        int32_t dx = 0, dy = 0;
        bool eligible = false;
    };
    struct Row {
        SceneRow scene;
        std::array<LayerId, layer_count> order{};
        std::array<bool, 5> valid{}; // scrolled_layers: enabled, unmosaiced, sane clips
        std::array<int32_t, 10> delta{};
        std::array<bool, 10> eligible{};
    };
    ClipRanges ranges(const SceneRow &row, LayerId id) const noexcept;
    bool same_controls(const SceneRow &a, const SceneRow &b, LayerId id) const noexcept;
    std::array<Sprite, 1024> sprites_{};
    std::array<Row, 232> rows_{};
    uint64_t frame_ = 0;
    unsigned sprite_count_ = 0;
    uint8_t pen_mask_ = 0;
    int border_ = 0;
    bool captured_ = false, paired_ = false;
    MotionInterpolationStats pair_stats_{};
};
} // namespace f3rt
