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
// raster: X/Y inverse-span biases, then X/Y output-grid texel steps.
layout(location = 0) flat out ivec4 raster;
layout(location = 1) flat out uint texel_base;
layout(location = 2) flat out uint color_base;
// One quad per descriptor covers the union of its 16x16 texel spans; the
// fragment shader inverts the CPU span formulas (see sprite.frag).
void main() {
    uint base = SPRITES + uint(gl_InstanceIndex) * F3_SCENE_SPRITE_STRIDE;
    int x = int(scene.words[base + F3_SPRITE_X]), y = int(scene.words[base + F3_SPRITE_Y]);
    int sx = int(scene.words[base + F3_SPRITE_SCALE_X]), sy = int(scene.words[base + F3_SPRITE_SCALE_Y]);
    int s = int(params.dimensions.x), left = 46 - int(params.dimensions.y);
    bool native_plane = s == 1 && params.dimensions.y == 0u;
    int origin_x = native_plane ? 0 : left * s, origin_y = native_plane ? 0 : 24 * s;
    vec2 extent = native_plane ? vec2(432,256) : vec2(params.dimensions.zw);
    // Texel a starts at ((px + a*kx) >> 8) - origin_x; texel row b likewise.
    // Biases stay in output-grid 24.8 units; scaling them changes CPU phases.
    int px = x * s + 128, py = y * s + 255, kx = sx * s, ky = sy * s;
    int x0 = (px >> 8) - origin_x, x1 = ((px + 16 * kx) >> 8) - origin_x;
    int y0 = (py >> 8) - origin_y;
    int y1 = max(((py + 15 * ky) >> 8) - origin_y + 1, ((py + 16 * ky) >> 8) - origin_y);
    // Nominal 24.8 rectangle is culled before raster rounding.
    bool culled = x + 16 * sx <= left * 256 || x > (365 + int(params.dimensions.y)) * 256 ||
                  y + 16 * sy <= 24 * 256 || y > 255 * 256 || x0 >= x1;
    const ivec2 corners[6] = ivec2[6](ivec2(0,0),ivec2(1,0),ivec2(0,1),ivec2(0,1),ivec2(1,0),ivec2(1,1));
    ivec2 c = corners[gl_VertexIndex];
    vec2 pos = vec2(c.x == 0 ? x0 : x1, c.y == 0 ? y0 : y1);
    gl_Position = culled ? vec4(2,2,0,1) : vec4(pos.x * 2.0 / extent.x - 1.0,
        1.0 - pos.y * 2.0 / extent.y, 0, 1);
    // Output pixel d lies in texel n's start iff n*k <= 256*d + bias.
    raster = ivec4(256 * (origin_x + 1) - px - 1, 256 * (origin_y + 1) - py - 1, kx, ky);
    uint flags = scene.words[base + F3_SPRITE_FLIP];
    texel_base = (scene.words[base + F3_SPRITE_TILE] & 32767u) * 256u |
                 ((flags & F3_SPRITE_FLIP_X) != 0u ? 15u : 0u) | ((flags & F3_SPRITE_FLIP_Y) != 0u ? 240u : 0u);
    color_base = ((0x1000u + (scene.words[base + F3_SPRITE_PALETTE] << 4u)) & 65535u) | (params.controls.y << 16u);
}
