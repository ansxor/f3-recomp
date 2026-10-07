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
const uint INTERPOLATION = F3_SCENE_INTERP;
const uint INTERPOLATION_ROW_STRIDE = F3_SCENE_INTERP_ROW_STRIDE, INTERPOLATION_PF_STRIDE = F3_INTERP_PF_STRIDE;
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
    uint a = scene.words[PALETTE + ((base + low * stride) & (F3_SCENE_PALETTE_WORDS - 1u))];
    uint b = weight == 0.0 ? a : scene.words[PALETTE + ((base + (low + 1u) * stride) & (F3_SCENE_PALETTE_WORDS - 1u))];
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
    uint packed_blend = scene.words[row + F3_ROW_BLEND];
    uvec4 blend = uvec4(packed_blend & 255u,(packed_blend >> 8u) & 255u,(packed_blend >> 16u) & 255u,packed_blend >> 24u);
    PixelMix pixel = PixelMix(0u,scene.words[row + F3_ROW_BACKGROUND],0u,8u,0u,0u,255u
#ifdef VIDEO_INTERP
        ,0u,0u
#endif
    );
    for (uint order = 0u; order < LAYER_COUNT; ++order) {
        uint index = scene.words[row + F3_ROW_ORDER + order];
        if ((params.controls.w & (1u << index)) == 0u) continue;
        uint layer = row + F3_ROW_LAYERS + F3_LAYER_STRIDE * index, flags = scene.words[layer + F3_LAYER_FLAGS];
        if ((flags & F3_LAYER_ENABLED) == 0u) continue;
        for (uint visit = 0u; visit < scene.words[layer + F3_LAYER_CLIP_COUNT]; ++visit) {
            int lo = (max(left,int(scene.words[layer + F3_LAYER_CLIPS + visit * 2u])) - left) * s;
            int hi = (min(366 + int(params.dimensions.y),int(scene.words[layer + F3_LAYER_CLIPS + 1u + visit * 2u])) - left) * s;
            if (pos.x < lo || pos.x >= hi || ((flags >> F3_LAYER_MODE_SHIFT) & F3_LAYER_MODE_MASK) == pixel.mode) continue;
            int q = pos.x + left * s;
            int period = int(scene.words[row + F3_ROW_MOSAIC]);
            if ((flags & F3_LAYER_MOSAIC) != 0u && period > 1) {
                int h = left + native_x, c = ((h + 68) % 432 + 432) % 432;
                q = (h - c % period) * s;
            }
            uint color = 0u, select = (flags >> F3_LAYER_SELECT_SHIFT) & 1u;
#ifdef VIDEO_INTERP
            uint color_rgb = 0u;
#endif
            if (index < LAYER_SP0) {
                uint pf = row + F3_ROW_PF + index * F3_PF_STRIDE;
                int x = floor_scale((int(scene.words[pf + F3_PF_SOURCE_X]) * s + (q - 46 * s) * int(scene.words[pf + F3_PF_X_STEP])) >> 8, s) & 1023;
#ifdef VIDEO_INTERP
                uint metadata = INTERPOLATION + uint(24 + native_y) * INTERPOLATION_ROW_STRIDE +
                    index * INTERPOLATION_PF_STRIDE;
                uint interp_flags = scene.words[metadata];
                float fraction = float(sub_y) / float(s);
                if (sub_y != 0 && (interp_flags & (F3_INTERP_FLAG_SOURCE | F3_INTERP_FLAG_ZOOM)) != 0u) {
                    float source = float(int(scene.words[pf + F3_PF_SOURCE_X]));
                    float step = float(int(scene.words[pf + F3_PF_X_STEP]));
                    if ((interp_flags & F3_INTERP_FLAG_SOURCE) != 0u)
                        source = interpolated_field(metadata + F3_INTERP_SOURCE, source, fraction);
                    if ((interp_flags & F3_INTERP_FLAG_ZOOM) != 0u)
                        step = interpolated_field(metadata + F3_INTERP_ZOOM, step, fraction);
                    x = int(floor((source + float(q - 46 * s) / float(s) * step) / 256.0)) & 1023;
                }
#endif
                int fy = truncate_scale(int(scene.words[pf + F3_PF_Y_FRACTION]) * s + sub_y * int(scene.words[pf + F3_PF_Y_STEP]), s);
                int y = (int(scene.words[pf + F3_PF_SOURCE_Y]) + (fy >> 8)) & 511;
#ifdef VIDEO_INTERP
                if (sub_y != 0 && (interp_flags & F3_INTERP_FLAG_VERTICAL) != 0u) {
                    float phase = float(int(scene.words[pf + F3_PF_SOURCE_Y]) * 256 + int(scene.words[pf + F3_PF_Y_FRACTION]));
                    y = (int(floor(interpolated_field(metadata + F3_INTERP_VERTICAL, phase, fraction))) >> 8) & 511;
                }
#endif
                // Raw 4-byte PF cell (bit layout shared with GameTiles::RowSampler,
                // see runtime/renderer/game/tiles.hpp): attributes<<16 | code.
                uint cell = PF_CELLS + (index * 2048u + uint(y / 16 * 64 + x / 16)) * 2u;
                uint packed = scene.words[cell];
                uint attr = packed >> 16u;
                uint code = packed & 65535u;
                uint tx = uint(x & 15) ^ ((attr & 0x4000u) != 0u ? 15u : 0u);
                uint ty = uint(y & 15) ^ ((attr & 0x8000u) != 0u ? 15u : 0u);
                uint offset = (code & 32767u) * 256u + ty * 16u + tx;
                uint pen = byte_pen(assets.words[offset >> 2u],offset) &
                    ((((attr >> 10u) & 3u & ~attr) << 4u) | 15u);
                color = ((attr & 511u) * 16u + pen) & 65535u;
                if (pen == 0u || color == 0u) continue;
#ifdef VIDEO_INTERP
                if (sub_y != 0 && (interp_flags & F3_INTERP_FLAG_PALETTE) != 0u) {
                    float current = float(scene.words[pf + F3_PF_PALETTE_ADD]);
                    float next = float(scene.words[pf + ROW_STRIDE + F3_PF_PALETTE_ADD]);
                    float addition = clamp(interpolated_field(metadata + F3_INTERP_PALETTE, current, fraction),
                        min(current, next), max(current, next));
                    color_rgb = palette_rgb(color, addition, interp_flags >> F3_INTERP_PALETTE_STRIDE_SHIFT);
                }
#endif
                color = (color + scene.words[pf + F3_PF_PALETTE_ADD]) & 65535u;
                select = (attr >> 9u) & 1u;
            } else if (index < LAYER_TEXT) {
                bool native_plane = s == 1 && params.dimensions.y == 0u;
                int x = native_plane ? q : q - left * s;
                if (x < 0 || x >= (native_plane ? 432 : int(params.dimensions.z))) continue;
                color = texelFetch(sprite_plane,ivec2(x,pos.y + (native_plane ? 24 : 0)),0).r;
                if (color == 0u || ((color >> 10u) & 3u) != index - LAYER_SP0) continue;
            } else {
                int x = floor_scale(int(scene.words[row + F3_ROW_TEXT_X]) * s + q - 46 * s, s) & 511;
                int y = int(scene.words[row + F3_ROW_TEXT_Y]) & 511;
                // Motion-only 24.8 text scroll; floor sampling matches native at integer endpoints.
                if ((params.controls.w & 0x80000000u) != 0u) {
                    x = floor_scale((int(scene.words[row + F3_ROW_MOTION_TEXT_X]) * s + (q - 46 * s) * 256) >> 8, s) & 511;
                    y = (floor_scale(int(scene.words[row + F3_ROW_MOTION_TEXT_Y]) * s + sub_y * 256, s) >> 8) & 511;
                }
                // Raw big-endian text-map word (bit layout shared with
                // GameText::pixel, see runtime/renderer/game/text.hpp): bits 0-7 glyph,
                // bit 8 flip X, bits 9-14 palette code, bit 15 flip Y.
                uint cell = scene.words[TEXT_CELLS + uint(y / 8 * 64 + x / 8)];
                uint tx = uint(x & 7) ^ ((cell & 0x0100u) != 0u ? 7u : 0u);
                uint ty = uint(y & 7) ^ ((cell & 0x8000u) != 0u ? 7u : 0u);
                // Raw glyph RAM byte-packed at GLYPHS: pixel (tx, ty) is nibble
                // (tx & 1) of byte ty*4 + (3 - tx/2).
                uint glyph_byte = (cell & 255u) * 32u + ty * 4u + (3u - tx / 2u);
                uint pen = (byte_pen(scene.words[GLYPHS + (glyph_byte >> 2u)], glyph_byte) >>
                    ((tx & 1u) * 4u)) & 15u;
                if (pen == 0u) continue;
                color = ((cell >> 9u) & 63u) * 16u + pen;
            }
            mix_pixel(pixel,flags,color,select,blend
#ifdef VIDEO_INTERP
                ,color_rgb
#endif
            );
        }
    }
    uint src = scene.words[PALETTE + (pixel.source & (F3_SCENE_PALETTE_WORDS - 1u))], dst = scene.words[PALETTE + (pixel.destination & (F3_SCENE_PALETTE_WORDS - 1u))];
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
