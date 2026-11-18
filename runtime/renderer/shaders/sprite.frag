#version 450
layout(set = 2, binding = 0) uniform usampler2D trail_seed;
#ifdef VIDEO_MSL
#define ASSET_BINDING 0
#else
#define ASSET_BINDING 1
#endif
layout(std430, set = 2, binding = ASSET_BINDING) readonly buffer Assets { uint words[]; } assets;
layout(location = 0) flat in ivec4 raster;
layout(location = 1) flat in uint texel_base;
layout(location = 2) flat in uint color_base;
layout(location = 3) flat in uint blend_weight;
layout(location = 4) flat in uint retain_trails;
layout(location = 0) out uint indexed_color;
uint pen_at(uint a, uint b) {
    uint texel = texel_base ^ ((b << 4u) | a);
    return ((assets.words[texel >> 2u] >> ((texel & 3u) * 8u)) & 255u) & (color_base >> 16u);
}
// CPU texel n starts at output pixel s(n) = floor((p + n*k) / 256) - origin;
// s(n) <= d  <=>  n*k <= 256*d + bias, bias = 256*(origin+1) - p - 1.
// X spans tile exactly (zero-width texels skipped): pixel x belongs to the
// last texel starting at or before it. Y rows end at max(s(n)+1, s(n+1)):
// pixel y is covered by the last row starting before it when no row starts
// on it; otherwise by every row starting on it. Earliest opaque row wins.
void main() {
    ivec2 d = ivec2(gl_FragCoord.xy);
    if (retain_trails != 0u && texelFetch(trail_seed, d, 0).r != 0u) discard;
    int ux = 256 * d.x + raster.x;
    if (ux < 0) discard;
    int a = ux / raster.z;
    if (a > 15) discard;
    int uy = 256 * d.y + raster.y, before = uy - 256;
    if (uy < 0) discard;
    int last = uy / raster.w;
    // Steps of at least one pixel give every row a natural span: only `last`.
    if (raster.w >= 256) {
        uint pen = last > 15 ? 0u : pen_at(uint(a), uint(last));
        if (pen == 0u) discard;
        indexed_color = blend_weight | ((color_base & 65535u) | pen);
        return;
    }
    int first = before < 0 ? 0 : before / raster.w + 1;
    int hi = min(last, 15);
    for (int b = min(first, last); b <= hi; ++b) {
        uint pen = pen_at(uint(a), uint(b));
        if (pen != 0u) {
            indexed_color = blend_weight | ((color_base & 65535u) | pen);
            return;
        }
    }
    discard;
}
