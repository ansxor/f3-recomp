#pragma once
#include "renderer/game/captured_frame.hpp"
#include "renderer/gpu/interp.hpp"
#include "renderer/gpu/motion.hpp"
#include "renderer/gpu/scene_layout.h"
#include <cstdint>
#include <span>

namespace f3rt {
// Word counts of the GPU scene buffer (layout: scene_layout.h).
inline constexpr size_t encoded_scene_words = F3_SCENE_WORD_COUNT;
inline constexpr size_t encoded_interpolation_words = F3_SCENE_INTERP_WORD_COUNT;
inline constexpr size_t encoded_native_words = 320 * 232;

// Shader uniforms, two tightly packed uvec4s (see scene.glsl).
struct GpuUniforms {
    uint32_t scale, border, width, height;
    uint32_t sprite_count, pen_mask, fallback, layer_mask;
};

// The only code that writes GPU scene words: a one-way encoding of a captured
// frame into `out`, typically the mapped upload buffer, with no intermediate copy.
//   * fallback frame: copies native_pixels (320x232 ARGB) to out[0..encoded_native_words).
//   * otherwise writes the canonical scene (encoded_scene_words), with `motion`'s
//     sprite/playfield/text geometry replacing the captured values when non-null.
//   * non-null `coefficients` additionally writes the interpolation region
//     (encoded_interpolation_words in total).
// `options.border` bounds layer clip ranges. Throws std::invalid_argument if `out` is too small.
void encode(const CapturedFrame &frame, GameVideoOptions options, const MotionState *motion,
            const InterpolationCoefficients *coefficients, std::span<uint32_t> out);
} // namespace f3rt
