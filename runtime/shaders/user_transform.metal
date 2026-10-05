#include <metal_stdlib>
using namespace metal;
// Native Metal example: load directly through GpuVideo::set_postprocess(User).
// The fullscreen vertex shader exports only position; no user varyings.
// Uniform: internal width, height, scale, seconds since preset activation.
fragment float4 f3_postprocess(float4 position [[position]],
                              texture2d<float> source_image [[texture(0)]],
                              sampler source_sampler [[sampler(0)]],
                              constant float4 &parameters [[buffer(0)]]) {
    float3 rgb = source_image.sample(source_sampler, position.xy / parameters.xy).rgb;
    return float4(1.0 - rgb, 1.0);
}
