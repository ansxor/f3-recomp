#pragma once
#include "renderer/game/scene_snapshot.hpp"
#include "renderer/gpu/interp.hpp"
#include "renderer/gpu/motion.hpp"
#include "renderer/gpu/scene_layout.h"
#include <cstdint>
#include <optional>
#include <span>

namespace f3rt {
// Word counts of the GPU scene buffer (layout: scene_layout.h).
inline constexpr size_t ENCODED_SCENE_WORDS = F3_SCENE_WORD_COUNT;
inline constexpr size_t ENCODED_INTERPOLATION_WORDS = F3_SCENE_INTERP_WORD_COUNT;
inline constexpr size_t ENCODED_NATIVE_WORDS = geometry::NATIVE_PIXELS;

// Shader uniforms, three tightly packed uvec4s (see scene.glsl).
struct GpuUniforms {
    uint32_t scale, border, width, height;
    uint32_t sprite_count, pen_mask, fallback, layer_mask;
    uint32_t first_line = geometry::FIRST_LINE, native_height = geometry::HEIGHT, pf_tile_count = 0,
             sp_tile_count = 0;
};

struct GpuPrepScratch {
    MotionState motion_state;
    InterpolationCoefficients interp_coefficients;
};

struct EncodedScene {
    GpuUniforms uniforms{};
    uint32_t upload_bytes = 0;
    bool fallback = false;
    bool use_interpolation = false;
    bool has_trails = false;
    bool blend_weighted = false;
    std::span<const uint16_t> trail_seed;
    MotionInterpolationStats motion_stats{};
    InterpolationStats interp_stats{};
};

struct MotionScene;
struct PreparedScene;

// Stage 1: one snapshot plus how to draw it. Borrows the snapshot; it must outlive the chain.
struct ScenePlan {
    const SceneSnapshot &scene;
    GameVideoOptions options;
    SpriteList sprite_list = SpriteList::Presented;
    uint32_t layer_mask = ALL_LAYERS;

    // Temporal geometry at `alpha` (nullopt: canonical). The sampled state is written into
    // `scratch` and referenced from the result only when `history` pairs with this scene.
    // Only the presented sprite list is paired by the history; other lists stay canonical.
    MotionScene apply_motion(const GpuMotionHistory *history, std::optional<float> alpha,
                             MotionState &scratch) const noexcept;
};

struct MotionScene {
    ScenePlan plan;
    const MotionState *motion = nullptr;
    MotionInterpolationStats motion_stats{};

    PreparedScene analyze_interpolation(InterpolationConfig config,
                                        std::span<const uint64_t> tile_pen_masks,
                                        InterpolationCoefficients &scratch) const noexcept;
};

struct PreparedScene {
    MotionScene motion;
    const InterpolationCoefficients *coefficients = nullptr;
    InterpolationStats interp_stats{};
    bool use_interpolation = false;

    size_t required_words() const noexcept;
    uint32_t upload_bytes() const noexcept;
    // The only code that writes GPU scene words: a one-way encoding of the snapshot into `out`,
    // typically the mapped upload buffer, with no intermediate copy.
    //   * fallback frame: copies native_pixels (320 x geometry::HEIGHT ARGB) to out[0..ENCODED_NATIVE_WORDS).
    //   * otherwise writes the canonical scene (ENCODED_SCENE_WORDS), with `motion`'s
    //     sprite/playfield/text geometry replacing the snapshot values when non-null. Expanded
    //     output (`options.expanded()`) writes playfield rows through presented_playfield()
    //     (game_config::video.full_resolution_alt_maps); the snapshot rows themselves stay canonical.
    //   * non-null `coefficients` additionally writes the interpolation region
    //     (ENCODED_INTERPOLATION_WORDS in total).
    // `options.border` bounds layer clip ranges. Throws std::invalid_argument if `out` is too small.
    EncodedScene encode_into(std::span<uint32_t> out, uint32_t pf_tile_count = 0,
                             uint32_t sp_tile_count = 0) const;
};
} // namespace f3rt
