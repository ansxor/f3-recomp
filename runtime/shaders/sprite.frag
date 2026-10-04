#version 450
layout(std430, set = 2, binding = 0) readonly buffer Assets { uint words[]; } assets;
layout(location = 0) flat in uint texel;
layout(location = 1) flat in uint color_base;
layout(location = 0) out uint indexed_color;
void main() {
    uint pen = ((assets.words[texel >> 2u] >> ((texel & 3u) * 8u)) & 255u) & (color_base >> 16u);
    if (pen == 0u) discard;
    indexed_color = ((color_base & 65535u) + pen) & 65535u;
}
