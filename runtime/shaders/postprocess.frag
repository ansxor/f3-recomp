#version 450
// SDL fragment ABI: combined source sampler set2/slot0, float4 set3/slot0.
layout(set = 2, binding = 0) uniform sampler2D source_image;
layout(std140, set = 3, binding = 0) uniform Parameters {
    vec4 dimensions; // internal width, height, scale, elapsed seconds
} params;
layout(location = 0) out vec4 color;
void main() {
    vec2 uv = gl_FragCoord.xy / params.dimensions.xy;
    vec3 rgb = texture(source_image, uv).rgb;
    float scanline = 0.86 + 0.14 * cos(3.14159265 * (gl_FragCoord.y / params.dimensions.z - 0.5));
    vec2 centered = uv * 2.0 - 1.0;
    float vignette = 1.0 - 0.10 * dot(centered, centered);
    color = vec4(rgb * scanline * vignette, 1.0);
}
