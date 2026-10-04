// Shared compositor body; VIDEO_INTERP builds the separate opt-in variant.
layout(set = 2, binding = 0) uniform usampler2D sprite_plane;
layout(std430, set = 2, binding = 1) readonly buffer Scene { uint words[]; } scene;
layout(std430, set = 2, binding = 2) readonly buffer Assets { uint words[]; } assets;
layout(std430, set = 2, binding = 3) readonly buffer Native { uint words[]; } native_frame;
layout(location = 0) out vec4 output_color;
struct PixelMix {
    uint source; uint destination; uint sw; uint dw; uint sp; uint dp; uint mode;
#ifdef VIDEO_INTERP
    uint source_rgb; uint destination_rgb;
#endif
};
void mix_pixel(inout PixelMix p, uint flags, uint color, uint select, uvec4 blend
#ifdef VIDEO_INTERP
    , uint color_rgb
#endif
) {
    uint priority = flags & 15u, mode = (flags >> 4u) & 3u;
    if (color == 0u || mode == p.mode) return;
    if (priority > p.sp) {
        uint slot = select;
        if (mode == 1u || mode == 2u) {
            if (mode == 1u) slot += 2u;
            if (blend[slot] == 0u) return;
            p.sw = blend[slot];
        } else {
            if (blend[slot] == 0u && blend[slot + 2u] == 0u) return;
            p.sw = blend[slot + 2u]; p.dw = blend[slot]; p.dp = priority; p.destination = color;
            #ifdef VIDEO_INTERP
            p.destination_rgb = color_rgb;
            #endif
        }
        p.source = color; p.mode = mode; p.sp = priority;
#ifdef VIDEO_INTERP
        p.source_rgb = color_rgb;
#endif
    } else if (priority >= p.dp) {
        p.destination = priority == p.dp ? 0u : color; p.dp = priority;
#ifdef VIDEO_INTERP
        p.destination_rgb = p.destination == 0u ? 0u : color_rgb;
#endif
        p.dw = blend[select + (p.mode == 1u ? 0u : 2u)];
    }
}
vec4 unpack_rgb(uint color) { return vec4(float((color >> 16u) & 255u),float((color >> 8u) & 255u),float(color & 255u),255.0) / 255.0; }
// Scale is validated as 1..8; specialize non-power-of-two divisors too.
int floor_scale(int n, int scale) {
    switch (scale) {
    case 1: return n;
    case 2: return n >> 1;
    case 3: return floor_divide(n, 3);
    case 4: return n >> 2;
    case 5: return floor_divide(n, 5);
    case 6: return floor_divide(n, 6);
    case 7: return floor_divide(n, 7);
    default: return n >> 3;
    }
}
int truncate_scale(int n, int scale) {
    if (scale == 3) return n / 3;
    if (scale == 5) return n / 5;
    if (scale == 6) return n / 6;
    if (scale == 7) return n / 7;
    int shift = scale == 8 ? 3 : scale >> 1;
    return (n + (n < 0 ? (1 << shift) - 1 : 0)) >> shift;
}
#ifdef VIDEO_INTERP
const uint INTERPOLATION = 131072u;
const uint INTERPOLATION_ROW_STRIDE = 52u, INTERPOLATION_PF_STRIDE = 13u;
// Anchored increments only; subrow-zero always uses the original integer path.
float interpolated_field(uint at, float native_value, float fraction) {
    float c1 = uintBitsToFloat(scene.words[at]);
    float c2 = uintBitsToFloat(scene.words[at + 1u]);
    float c3 = uintBitsToFloat(scene.words[at + 2u]);
    return native_value + fraction * (c1 + fraction * (c2 + fraction * c3));
}
// Same pen in checked compatible banks; never interpolate pen/index roles.
uint palette_rgb(uint base, float addition, uint stride) {
    float banks = addition / float(stride);
    uint low = uint(floor(banks));
    float weight = banks - float(low);
    uint a = scene.words[PALETTE + ((base + low * stride) & 8191u)];
    uint b = weight == 0.0 ? a : scene.words[PALETTE + ((base + (low + 1u) * stride) & 8191u)];
    uint rgb = 0xff000000u; // Valid override, including RGB black.
    for (uint shift = 0u; shift < 24u; shift += 8u) {
        float value = mix(float((a >> shift) & 255u),float((b >> shift) & 255u),weight);
        rgb |= uint(floor(value + 0.5)) << shift;
    }
    return rgb;
}
#endif
void main() {
    ivec2 pos = ivec2(gl_FragCoord.xy);
    int s = int(params.dimensions.x), left = 46 - int(params.dimensions.y);
    int native_x = floor_scale(pos.x, s), native_y = floor_scale(pos.y, s);
    int sub_y = pos.y - native_y * s;
    if (params.controls.z != 0u) {
        int x = native_x - int(params.dimensions.y);
        output_color = x < 0 || x >= 320 ? vec4(0,0,0,1) : unpack_rgb(native_frame.words[uint(native_y * 320 + x)]);
        return;
    }
    uint row = ROWS + uint(24 + native_y) * ROW_STRIDE;
    uint packed_blend = scene.words[row + 4u];
    uvec4 blend = uvec4(packed_blend & 255u,(packed_blend >> 8u) & 255u,(packed_blend >> 16u) & 255u,packed_blend >> 24u);
    PixelMix pixel = PixelMix(0u,scene.words[row],0u,8u,0u,0u,255u
#ifdef VIDEO_INTERP
        ,0u,0u
#endif
    );
    for (uint order = 0u; order < 9u; ++order) {
        uint index = scene.words[row + 5u + order];
        if ((params.controls.w & (1u << index)) == 0u) continue;
        uint layer = row + 16u + 34u * index, flags = scene.words[layer];
        if ((flags & 64u) == 0u) continue;
        for (uint visit = 0u; visit < scene.words[layer + 1u]; ++visit) {
            int lo = (max(left,int(scene.words[layer + 2u + visit * 2u])) - left) * s;
            int hi = (min(366 + int(params.dimensions.y),int(scene.words[layer + 3u + visit * 2u])) - left) * s;
            if (pos.x < lo || pos.x >= hi || ((flags >> 4u) & 3u) == pixel.mode) continue;
            int q = pos.x + left * s;
            int period = int(scene.words[row + 1u]);
            if ((flags & 256u) != 0u && period > 1) {
                int h = left + native_x, c = ((h + 68) % 432 + 432) % 432;
                q = (h - c % period) * s;
            }
            uint color = 0u, select = (flags >> 7u) & 1u;
#ifdef VIDEO_INTERP
            uint color_rgb = 0u;
#endif
            if (index < 4u) {
                uint pf = row + 322u + index * 6u;
                int x = floor_scale((int(scene.words[pf]) * s + (q - 46 * s) * int(scene.words[pf + 2u])) >> 8, s) & 1023;
#ifdef VIDEO_INTERP
                uint metadata = INTERPOLATION + uint(24 + native_y) * INTERPOLATION_ROW_STRIDE +
                    index * INTERPOLATION_PF_STRIDE;
                uint interp_flags = scene.words[metadata];
                float fraction = float(sub_y) / float(s);
                if (sub_y != 0 && (interp_flags & 3u) != 0u) {
                    float source = float(int(scene.words[pf]));
                    float step = float(int(scene.words[pf + 2u]));
                    if ((interp_flags & 1u) != 0u)
                        source = interpolated_field(metadata + 1u, source, fraction);
                    if ((interp_flags & 2u) != 0u)
                        step = interpolated_field(metadata + 4u, step, fraction);
                    x = int(floor((source + float(q - 46 * s) / float(s) * step) / 256.0)) & 1023;
                }
#endif
                int fy = truncate_scale(int(scene.words[pf + 4u]) * s + sub_y * int(scene.words[pf + 3u]), s);
                int y = (int(scene.words[pf + 1u]) + (fy >> 8)) & 511;
#ifdef VIDEO_INTERP
                if (sub_y != 0 && (interp_flags & 4u) != 0u) {
                    float phase = float(int(scene.words[pf + 1u]) * 256 + int(scene.words[pf + 4u]));
                    y = (int(floor(interpolated_field(metadata + 7u, phase, fraction))) >> 8) & 511;
                }
#endif
                uint cell = PF_CELLS + (index * 2048u + uint(y / 16 * 64 + x / 16)) * 2u;
                uint attr = scene.words[cell + 1u];
                uint tx = uint(x & 15) ^ ((attr & (1u << 24u)) != 0u ? 15u : 0u);
                uint ty = uint(y & 15) ^ ((attr & (1u << 25u)) != 0u ? 15u : 0u);
                uint offset = (scene.words[cell] & 32767u) * 256u + ty * 16u + tx;
                uint pen = byte_pen(assets.words[offset >> 2u],offset) & ((attr >> 16u) & 255u);
                color = ((attr & 65535u) + pen) & 65535u;
                if (pen == 0u || color == 0u) continue;
#ifdef VIDEO_INTERP
                if (sub_y != 0 && (interp_flags & 8u) != 0u) {
                    float current = float(scene.words[pf + 5u]);
                    float next = float(scene.words[pf + ROW_STRIDE + 5u]);
                    float addition = clamp(interpolated_field(metadata + 10u, current, fraction),
                        min(current, next), max(current, next));
                    color_rgb = palette_rgb(color, addition, interp_flags >> 16u);
                }
#endif
                color = (color + scene.words[pf + 5u]) & 65535u;
                select = (attr >> 26u) & 1u;
            } else if (index < 8u) {
                bool native_plane = s == 1 && params.dimensions.y == 0u;
                int x = native_plane ? q : q - left * s;
                if (x < 0 || x >= (native_plane ? 432 : int(params.dimensions.z))) continue;
                color = texelFetch(sprite_plane,ivec2(x,pos.y + (native_plane ? 24 : 0)),0).r;
                if (color == 0u || ((color >> 10u) & 3u) != index - 4u) continue;
            } else {
                int x = floor_scale(int(scene.words[row + 2u]) * s + q - 46 * s, s) & 511;
                int y = int(scene.words[row + 3u]) & 511;
                uint cell = scene.words[TEXT_CELLS + uint(y / 8 * 64 + x / 8)];
                uint tx = uint(x & 7) ^ ((cell & (1u << 16u)) != 0u ? 7u : 0u);
                uint ty = uint(y & 7) ^ ((cell & (1u << 17u)) != 0u ? 7u : 0u);
                uint offset = (cell & 255u) * 64u + ty * 8u + tx;
                uint pen = byte_pen(scene.words[GLYPHS + (offset >> 2u)],offset);
                if (pen == 0u) continue;
                color = ((cell >> 8u) & 255u) * 16u + pen;
            }
            mix_pixel(pixel,flags,color,select,blend
#ifdef VIDEO_INTERP
                ,color_rgb
#endif
            );
        }
    }
    uint src = scene.words[PALETTE + (pixel.source & 8191u)], dst = scene.words[PALETTE + (pixel.destination & 8191u)];
#ifdef VIDEO_INTERP
    if ((pixel.source_rgb & 0xff000000u) != 0u) src = pixel.source_rgb;
    if ((pixel.destination_rgb & 0xff000000u) != 0u) dst = pixel.destination_rgb;
#endif
    uint rgb = 0u;
    for (uint shift = 0u; shift < 24u; shift += 8u) {
        uint value = (((src >> shift) & 255u) * pixel.sw + ((dst >> shift) & 255u) * pixel.dw) >> 3u;
        rgb |= min(255u,value) << shift;
    }
    output_color = unpack_rgb(rgb);
}
