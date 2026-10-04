// Host layout: runtime/gpu_scene.hpp. SDL uniforms are two tightly packed uvec4s.
layout(std140, set = UNIFORM_SET, binding = 0) uniform Parameters {
    uvec4 dimensions; // scale, border, width, height
    uvec4 controls;   // sprite count, pen mask, fallback, layer mask
} params;
const uint PF_CELLS = 0u;
const uint TEXT_CELLS = 16384u;
const uint GLYPHS = 20480u;
const uint PALETTE = 24576u;
const uint ROWS = 32768u;
const uint ROW_STRIDE = 352u;
const uint SPRITES = 122880u;
uint byte_pen(uint packed, uint offset) { return (packed >> ((offset & 3u) * 8u)) & 255u; }
int floor_divide(int n, int d) { return n >= 0 ? n / d : -1 - (-1 - n) / d; }
