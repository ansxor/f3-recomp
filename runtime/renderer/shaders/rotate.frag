#version 450
layout(set = 2, binding = 0) uniform sampler2D source_image;
layout(std140, set = 3, binding = 0) uniform Parameters {
    uvec4 rotation;
} params;
layout(location = 0) out vec4 color;
void main() {
    ivec2 size = textureSize(source_image, 0);
    ivec2 dst = ivec2(gl_FragCoord.xy);
    ivec2 src = params.rotation.x == 90u ? ivec2(dst.y, size.y - 1 - dst.x)
                                         : ivec2(size.x - 1 - dst.y, dst.x);
    color = texelFetch(source_image, src, 0);
}
