// Highway SIMD kernel for HLE voice block rendering.
// Output is perceptually transparent rather than bit-exact; FMA and arithmetic
// reassociation are intentional for SIMD throughput.

#undef HWY_TARGET_INCLUDE
#define HWY_TARGET_INCLUDE "runtime/hle_voice_kernel.cpp"
#include <hwy/foreach_target.h>
#include <hwy/highway.h>

#include "hle_voice_kernel.hpp"

HWY_BEFORE_NAMESPACE();
namespace f3rt::hle {
namespace HWY_NAMESPACE {
namespace hn = hwy::HWY_NAMESPACE;

void RenderVoiceBlock(const VoiceBlock &b) {
    if (b.frames == 0 || b.lanes == 0) {
        return;
    }

    const hn::ScalableTag<float> d;
    const size_t N = hn::Lanes(d);

    for (size_t lane = 0; lane < b.lanes; lane += N) {
        auto s0 = hn::LoadU(d, b.state + 0 * voice_lane_stride + lane);
        auto s1 = hn::LoadU(d, b.state + 1 * voice_lane_stride + lane);
        auto s2 = hn::LoadU(d, b.state + 2 * voice_lane_stride + lane);
        auto s3 = hn::LoadU(d, b.state + 3 * voice_lane_stride + lane);

        auto prev0 = hn::LoadU(d, b.previous + 0 * voice_lane_stride + lane);
        auto prev1 = hn::LoadU(d, b.previous + 1 * voice_lane_stride + lane);
        auto prev2 = hn::LoadU(d, b.previous + 2 * voice_lane_stride + lane);
        auto prev3 = hn::LoadU(d, b.previous + 3 * voice_lane_stride + lane);

        HWY_ALIGN float modes[64] = {};
        for (size_t i = 0; i < N; ++i) {
            modes[i] = static_cast<float>(b.mode[lane + i]);
        }
        const auto m = hn::Load(d, modes);
        const auto hp2 = hn::Eq(m, hn::Set(d, 0.0f));
        const auto lp2_uses_a2 = hn::Eq(m, hn::Set(d, 2.0f));
        const auto hp3 = hn::Lt(m, hn::Set(d, 2.0f));

        for (size_t f = 0; f < b.frames; ++f) {
            const size_t o = f * voice_lane_stride + lane;
            auto x = hn::LoadU(d, b.input + o);
            const auto a1 = hn::LoadU(d, b.a1 + o);
            const auto a2 = hn::LoadU(d, b.a2 + o);
            const auto hp = hn::LoadU(d, b.high_pole + o);
            const auto gl = hn::LoadU(d, b.gain_left + o);
            const auto gr = hn::LoadU(d, b.gain_right + o);

            // Stage 0: low-pass with a1
            const auto y0 = hn::MulAdd(a1, hn::Sub(x, s0), s0);
            prev0 = x;
            s0 = y0;
            x = y0;

            // Stage 1: low-pass with a1
            const auto y1 = hn::MulAdd(a1, hn::Sub(x, s1), s1);
            prev1 = x;
            s1 = y1;
            x = y1;

            // Stage 2: mode 0 -> HP(hp), mode 2 -> LP(a2), modes 1,3 -> LP(a1)
            const auto a_s2 = hn::IfThenElse(lp2_uses_a2, a2, a1);
            const auto lp2 = hn::MulAdd(a_s2, hn::Sub(x, s2), s2);
            const auto hp2_val = hn::MulAdd(hp, s2, hn::Sub(x, prev2));
            const auto y2 = hn::IfThenElse(hp2, hp2_val, lp2);
            prev2 = x;
            s2 = y2;
            x = y2;

            // Stage 3: modes 0,1 -> HP(hp), modes 2,3 -> LP(a2)
            const auto lp3 = hn::MulAdd(a2, hn::Sub(x, s3), s3);
            const auto hp3_val = hn::MulAdd(hp, s3, hn::Sub(x, prev3));
            const auto y3 = hn::IfThenElse(hp3, hp3_val, lp3);
            prev3 = x;
            s3 = y3;

            hn::StoreU(hn::Mul(y3, gl), d, b.out_left + o);
            hn::StoreU(hn::Mul(y3, gr), d, b.out_right + o);
        }

        hn::StoreU(s0, d, b.state + 0 * voice_lane_stride + lane);
        hn::StoreU(s1, d, b.state + 1 * voice_lane_stride + lane);
        hn::StoreU(s2, d, b.state + 2 * voice_lane_stride + lane);
        hn::StoreU(s3, d, b.state + 3 * voice_lane_stride + lane);

        hn::StoreU(prev0, d, b.previous + 0 * voice_lane_stride + lane);
        hn::StoreU(prev1, d, b.previous + 1 * voice_lane_stride + lane);
        hn::StoreU(prev2, d, b.previous + 2 * voice_lane_stride + lane);
        hn::StoreU(prev3, d, b.previous + 3 * voice_lane_stride + lane);
    }
}

}  // namespace HWY_NAMESPACE
}  // namespace f3rt::hle
HWY_AFTER_NAMESPACE();

#if HWY_ONCE
namespace f3rt::hle {

HWY_EXPORT(RenderVoiceBlock);

void render_voice_block(const VoiceBlock &b) {
    if (b.frames == 0 || b.lanes == 0) {
        return;
    }
    HWY_DYNAMIC_DISPATCH(RenderVoiceBlock)(b);
}

}  // namespace f3rt::hle
#endif
