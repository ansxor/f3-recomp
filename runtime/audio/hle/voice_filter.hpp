#pragma once
// ES5505 four-pole voice filter: one voice, one sample per step.
//
// Each stage keeps its previous output and reads its input x (and x', the previous input, which is the previous
// stage's previous output):
//   low-pass:   y = previous + coef*(x - previous)
//   high-pass:  y = highpass_pole*previous + (x - x')   (evaluated as (x + pole*previous) - x')
// A stage picks one equation, so the unused one cannot leak a non-finite value. Every stage updates every sample
// whatever the mode, so a mode change keeps its state.
//
//   mode   stage 1    stage 2    stage 3    stage 4
//   0      LP coef1   LP coef1   HP         HP
//   1      LP coef1   LP coef1   LP coef1   HP
//   2      LP coef1   LP coef1   LP coef2   LP coef2
//   3      LP coef1   LP coef1   LP coef1   LP coef2
//
// Any mode above 3 behaves as mode 3.
#include <cstdint>

namespace f3rt::hle {

struct VoiceFilterState {
    float stage1 = 0.f, stage2 = 0.f, stage3 = 0.f, stage4 = 0.f;
    void clear() { *this = {}; }
};

constexpr unsigned filter_mode(uint8_t register_mode) noexcept {
    return register_mode < 3 ? register_mode : 3;
}

template <unsigned Mode>
inline float filter_step(VoiceFilterState &s, float x, float coef1, float coef2,
                         float highpass_pole) noexcept {
    const float previous2 = s.stage2, previous3 = s.stage3, previous4 = s.stage4;
    const float stage1 = s.stage1 + coef1 * (x - s.stage1);
    const float stage2 = previous2 + coef1 * (stage1 - previous2);
    const float stage3 = Mode == 0 ? stage2 + highpass_pole * previous3 - previous2
                                   : previous3 + (Mode == 2 ? coef2 : coef1) * (stage2 - previous3);
    const float stage4 = Mode < 2 ? stage3 + highpass_pole * previous4 - previous3
                                  : previous4 + coef2 * (stage3 - previous4);
    s = {stage1, stage2, stage3, stage4};
    return stage4;
}

} // namespace f3rt::hle
