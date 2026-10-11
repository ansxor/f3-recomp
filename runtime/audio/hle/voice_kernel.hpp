#pragma once
#include <cstddef>
#include <cstdint>

namespace f3rt::hle {

// Structure-of-arrays block of synth voices rendered over the same frames.
// Per-frame arrays are indexed [frame * voice_lane_stride + lane]; per-lane
// filter state is indexed [stage * voice_lane_stride + lane].
inline constexpr size_t voice_lane_stride = 32;
// `lanes` is always a multiple of this, so every SIMD width up to 16 floats
// divides it. Unused lanes carry zero input, zero gain and mode 3.
inline constexpr size_t voice_lane_multiple = 16;
inline constexpr size_t voice_block_frames = 64;

struct VoiceBlock {
    size_t frames = 0;                 // 1..voice_block_frames
    size_t lanes = 0;                  // multiple of voice_lane_multiple, <= voice_lane_stride
    const float *input = nullptr;      // interpolated sample value, int16 range
    const float *a1 = nullptr;         // low-pass coefficient for stages 0-1 (and 2 unless mode 2)
    const float *a2 = nullptr;         // low-pass coefficient for stage 2 in mode 2 and stage 3 in modes 2-3
    const float *high_pole = nullptr;  // high-pass pole for stage 2 in mode 0 and stage 3 in modes 0-1
    const float *gain_left = nullptr;  // final per-frame gain, including the 1/524288 scale
    const float *gain_right = nullptr;
    const uint8_t *mode = nullptr;     // [lane], 0..3
    float *state = nullptr;            // [4][stride] in/out
    float *previous = nullptr;         // [4][stride] in/out: last input seen by each stage
    float *out_left = nullptr;         // [frame][stride]: filtered * gain_left
    float *out_right = nullptr;        // [frame][stride]: filtered * gain_right
};

// Four cascaded one-pole stages per lane, in order 0..3, each consuming the
// previous stage's output x:
//   low-pass  (coefficient a):  y = state + a * (x - state)
//   high-pass (pole p):         y = x - previous + p * state
// then previous = x, state = y. Stage 0-1: low-pass a1. Stage 2: mode 0
// high-pass, mode 2 low-pass a2, modes 1/3 low-pass a1. Stage 3: modes 0-1
// high-pass, modes 2-3 low-pass a2. Selected at runtime for the best
// available SIMD target.
void render_voice_block(const VoiceBlock &block);

}
