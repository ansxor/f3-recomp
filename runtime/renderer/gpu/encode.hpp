#pragma once
#include "f3rt_video_cxx/video.h"
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

using GpuUniforms = rust_video::SceneUniforms;

// Caller-owned chain scratch. The references point into `rust_video::GpuPrepScratch`, whose
// storage is stable for the lifetime of this object (move-only).
class GpuPrepScratch {
public:
    GpuPrepScratch();
    ~GpuPrepScratch();
    GpuPrepScratch(const GpuPrepScratch &) = delete;
    GpuPrepScratch &operator=(const GpuPrepScratch &) = delete;

    rust::Box<rust_video::GpuPrepScratch> m_scratch;
    // Bind into the box's stable storage.
    MotionState &motion_state;
    InterpolationCoefficients &interp_coefficients;
};

struct EncodedScene {
    GpuUniforms uniforms{};
    uint32_t upload_bytes = 0;
    bool fallback = false;
    bool use_interpolation = false;
    bool has_trails = false;
    bool blend_weighted = false;
    // Owns the trail plane; `trail_seed` points into it. Move-only.
    rust::Vec<uint16_t> trail_storage;
    std::span<const uint16_t> trail_seed;
    MotionInterpolationStats motion_stats{};
    InterpolationStats interp_stats{};

    EncodedScene() = default;
    EncodedScene(const EncodedScene &other);
    EncodedScene &operator=(const EncodedScene &other);
    EncodedScene(EncodedScene &&other) noexcept;
    EncodedScene &operator=(EncodedScene &&other) noexcept;
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
    // Throws std::invalid_argument if `out` is too small.
    EncodedScene encode_into(std::span<uint32_t> out, uint32_t pf_tile_count = 0,
                             uint32_t sp_tile_count = 0) const;
};

} // namespace f3rt
