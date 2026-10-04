#version 450
#extension GL_GOOGLE_include_directive : require
#define UNIFORM_SET 1
#include "scene.glsl"
#ifdef VIDEO_MSL
#define SCENE_BINDING 1
#else
#define SCENE_BINDING 0
#endif
layout(std430, set = 0, binding = SCENE_BINDING) readonly buffer Scene { uint words[]; } scene;
layout(location = 0) flat out uint texel;
layout(location = 1) flat out uint color_base;
void main() {
    uint base = SPRITES + uint(gl_InstanceIndex) * 8u;
    int x = int(scene.words[base]), y = int(scene.words[base + 1u]);
    int sx = int(scene.words[base + 2u]), sy = int(scene.words[base + 3u]);
    uint p = 255u - uint(gl_VertexIndex) / 6u;
    int a = int(p & 15u), b = int(p >> 4u);
    int s = int(params.dimensions.x), left = 46 - int(params.dimensions.y);
    bool native_plane = s == 1 && params.dimensions.y == 0u;
    int origin_x = native_plane ? 0 : left, origin_y = native_plane ? 0 : 24;
    vec2 extent = native_plane ? vec2(432,256) : vec2(params.dimensions.zw);
    int px = (x + a * sx) * s + 128;
    int py = (y + b * sy) * s + 255;
    int x0 = (px >> 8) - origin_x * s, x1 = ((px + sx * s) >> 8) - origin_x * s;
    int y0 = (py >> 8) - origin_y * s, y1 = max(y0 + 1, ((py + sy * s) >> 8) - origin_y * s);
    bool culled = x + 16 * sx <= left * 256 || x > (365 + int(params.dimensions.y)) * 256 ||
                  y + 16 * sy <= 24 * 256 || y > 255 * 256 || x0 >= x1;
    const ivec2 corners[6] = ivec2[6](ivec2(0,0),ivec2(1,0),ivec2(0,1),ivec2(0,1),ivec2(1,0),ivec2(1,1));
    ivec2 c = corners[gl_VertexIndex % 6];
    vec2 pos = vec2(c.x == 0 ? x0 : x1, c.y == 0 ? y0 : y1);
    gl_Position = culled ? vec4(2,2,0,1) : vec4(pos.x * 2.0 / extent.x - 1.0,
        1.0 - pos.y * 2.0 / extent.y, 0, 1);
    uint flags = scene.words[base + 6u];
    uint tx = uint(a) ^ ((flags & 1u) != 0u ? 15u : 0u);
    uint ty = uint(b) ^ ((flags & 2u) != 0u ? 15u : 0u);
    texel = (scene.words[base + 4u] & 32767u) * 256u + ty * 16u + tx;
    color_base = ((0x1000u + (scene.words[base + 5u] << 4u)) & 65535u) | (params.controls.y << 16u);
}
