#version 450
// Compile for Vulkan offline; load the resulting .spv file, never this source:
// glslangValidator -V --target-env vulkan1.0 -o user_transform.spv user_transform.frag
// Entry main; only gl_FragCoord input; RGBA output location0.
layout(set = 2, binding = 0) uniform sampler2D source_image;
layout(std140, set = 3, binding = 0) uniform Parameters {
    vec4 dimensions; // internal width, height, scale, elapsed seconds
} params;
layout(location = 0) out vec4 color;
void main() {
    vec3 rgb = texture(source_image, gl_FragCoord.xy / params.dimensions.xy).rgb;
    color = vec4(1.0 - rgb, 1.0);
}
