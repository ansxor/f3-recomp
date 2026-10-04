#include "game_lines.hpp"
#include "f3rt/video.hpp"
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace f3rt {
namespace {

constexpr std::array<uint32_t, 32> line_hooks{
    0x00136e,
    0x005a22, 0x005a5e, 0x005a9a, 0x005af4,
    0x005cd8, 0x005d10,
    0x010044,
    0x08cfba, 0x08cfe0,
    0x091490, 0x091506, 0x0915d2, 0x091834, 0x09217c, 0x098dba,
    0x099b5a, 0x099f86, 0x09a6e6, 0x09a8de, 0x09acbe, 0x09ad3e,
    0x09a252, 0x09a28a, 0x09a2f6,
    0x09d66a, 0x09d72a, 0x09d7b6,
    0x09ecb0,
    0x0fe620, 0x0fefe6, 0x0ff0fa
};

bool is_covered_write(uint32_t pc) {
    static constexpr struct Range { uint32_t start, end; } ranges[] = {
        {0x00136e, 0x00145e}, // Display register uploader (0x660000..0x66001e)
        {0x005a2e, 0x005a42}, // PF0 clear rowscroll (0x62a000)
        {0x005a6a, 0x005a7e}, // PF1 clear rowscroll (0x62a200)
        {0x005aa6, 0x005ad8}, // PF2 clear rowscroll, zoom, colscroll (0x62a400, 0x628400, 0x624400)
        {0x005b00, 0x005b32}, // PF3 clear rowscroll, zoom, colscroll (0x62a600, 0x628600, 0x624600)
        {0x005d30, 0x005d6c}, // Line RAM profile init (0x624000..0x62b000, 0x620000)
        {0x010044, 0x01007c}, // Boot control registers (0x660000..0x66001a)
        {0x0100ba, 0x0100c4}, // Boot line RAM clear
        {0x08cfd2, 0x08cfda}, // Alpha blend save (0x626200)
        {0x08cff2, 0x08cff8}, // Alpha blend restore (0x626200)
        {0x0914b8, 0x0914c2}, // Water effect select init (0x62738e.., 0x62b78e..)
        {0x09152e, 0x091538}, // Water effect select init (0x62736a.., 0x62b76a..)
        {0x0915dc, 0x091610}, // Water effect main lines (0x626360, 0x62b760, 0x627760, 0x626160)
        {0x09187a, 0x091882}, // Water clip window animation (0x625760, 0x624760)
        {0x09218a, 0x09218a}, // Selection transition sprite priorities (0x627630)
        {0x098dc8, 0x098dc8}, // Full-screen alpha profile (0x626200)
        {0x099b72, 0x099b74}, // Attract sprite modes and priorities (0x626030/0x627630)
        {0x099fc2, 0x099fca}, // Attract reverse/normal blend transition
        {0x09a25c, 0x09a25c}, // Attract alpha fade from task D2
        {0x09a2ac, 0x09a2b0}, // Attract PF1/PF3 blend restoration
        {0x09a300, 0x09a300}, // Second attract alpha fade from task D2
        {0x09a6f4, 0x09a6f4}, // Attract PF2 blend/priority (0x62b400)
        {0x09a8ec, 0x09a8ec}, // Attract sprite priorities (0x627600)
        {0x09accc, 0x09accc}, // Attract PF2 blending on
        {0x09ad4c, 0x09ad4c}, // Attract PF2 blending off
        {0x09d684, 0x09d6a0}, // Gameboard PF2 palette add gradient (0x629530, 0x629500, 0x62b400)
        {0x09d75e, 0x09d7aa}, // Gameboard PF2 zoom & rowscroll (0x628530, 0x62a400)
        {0x09d7d6, 0x09d812}, // Gameboard PF2 column scroll (0x624530, 0x624500)
        {0x09ecde, 0x09ecee}, // PF0 wavy rowscroll (0x62a000)
    };
    for (const auto &r : ranges) {
        if (pc >= r.start && pc <= r.end) return true;
    }
    return false;
}

std::string format_layer(const SceneLayer &l) {
    std::ostringstream ss;
    ss << "{en=" << l.enabled
       << " pri=" << int(l.priority)
       << " bm=" << int(l.blend_mode)
       << " clip_en=" << int(l.clip_enabled)
       << " clip_inv=" << int(l.clip_inverted)
       << " clip_mode=" << l.clip_inverse
       << " b_sel=" << l.blend_select
       << " mos=" << l.mosaic << "}";
    return ss.str();
}

} // namespace

std::span<const uint32_t> GameLines::hooks() {
    return line_hooks;
}

void GameLines::observe_write(uint32_t pc, uint32_t address) {
    address &= 0xffffff;
    const bool is_lineram = (address >= 0x620000 && address < 0x630000);
    const bool is_ctrl = (address >= 0x660000 && address < 0x660040);
    if (!is_lineram && !is_ctrl) return;

    if (is_covered_write(pc)) return;

    if (supported_ || !unsupported_pc_) {
        unsupported_pc_ = pc;
    }
    supported_ = false;
}

void GameLines::load_default_profile(GameMemory &memory) {
    // ROM 5cd8 sets A1 base 0x624000 + (flip_byte ? 0 : 0x30)
    // and uploads exactly 232 words (0xe7 count) per subsection.
    const bool flip_byte = (memory.u8(0x40013e) != 0);
    const unsigned start_line = flip_byte ? 0 : 24;
    const unsigned end_line = start_line + 232;

    const uint16_t w_2_0 = memory.u16(0x005d74 + 16);
    const uint16_t w_2_1 = memory.u16(0x005d74 + 18);
    const uint16_t w_2_2 = memory.u16(0x005d74 + 20);
    const uint16_t w_2_3 = memory.u16(0x005d74 + 22);

    const uint16_t w_3_0 = memory.u16(0x005d74 + 24);
    const uint16_t w_3_1 = memory.u16(0x005d74 + 26);
    const uint16_t w_3_2 = memory.u16(0x005d74 + 28);
    const uint16_t w_3_3 = memory.u16(0x005d74 + 30);

    uint16_t pf_scale[4];
    for (int i = 0; i < 4; ++i) pf_scale[i] = memory.u16(0x005d74 + 32 + i * 2);

    uint16_t pf_pal_add[4];
    for (int i = 0; i < 4; ++i) pf_pal_add[i] = memory.u16(0x005d74 + 40 + i * 2);

    uint16_t pf_rs[4];
    for (int i = 0; i < 4; ++i) pf_rs[i] = memory.u16(0x005d74 + 48 + i * 2);

    uint16_t pf_mix[4];
    for (int i = 0; i < 4; ++i) pf_mix[i] = memory.u16(0x005d74 + 56 + i * 2);

    static constexpr int FIX_Y[] = { 0, 3, 2, 1 };

    for (unsigned y = start_line; y < end_line && y < 256; ++y) {
        auto &line = lines_[y];
        for (auto &c : line.clip) c = {0, 0};
        for (int idx = 0; idx < 4; ++idx) {
            uint8_t alpha = (w_2_1 >> (4 * idx)) & 0x0f;
            line.blend[idx] = std::min<uint8_t>(8, 0x0f - alpha);
        }
        line.x_sample = uint8_t(16 - ((w_2_2 >> 4) & 0x0f));
        line.bg_palette = w_2_3;

        line.pivot.pivot_control = uint8_t((w_2_0 >> 8) & 0xff);
        line.pivot.blend_select_v = (w_2_0 & 0x0200) != 0;
        line.pivot.pivot_enable = w_3_0;
        line.pivot.set_mix(w_3_1);
        line.pivot.x_sample_enable = (w_2_2 & 0x200) != 0;

        for (int g = 0; g < 4; ++g) {
            line.sp[g].set_mix((line.sp[g].mix_value & 0xc00f) | ((w_3_2 & 0x3ff) << 4));
            line.sp[g].blend_select_v = ((w_3_2 >> (12 + g)) & 1) != 0;
            line.sp[g].prio = (w_3_3 >> (g * 4)) & 0x0f;
            line.sp[g].blend_mode = (w_2_0 >> (g * 2)) & 3;
            line.sp[g].x_sample_enable = (w_2_2 & 0x100) != 0;
        }

        for (int i = 0; i < 4; ++i) {
            line.pf[i].colscroll = 0;
            line.pf[i].x_scale = 256 - ((pf_scale[i] >> 8) & 0xff);
            line.pf[FIX_Y[i]].y_scale = (pf_scale[i] & 0xff) << 1;
            line.pf[i].pal_add = pf_pal_add[i] * 16;
            int32_t rs = int32_t(pf_rs[i]) << 2;
            line.pf[i].rowscroll = (rs & 0xffffff00) - (rs & 0x000000ff);
            line.pf[i].set_mix(pf_mix[i]);
            line.pf[i].x_sample_enable = (w_2_2 & (1 << i)) != 0;
        }
    }
}

void GameLines::reset() {
    supported_ = false;
    unsupported_pc_ = 0;
    flipscreen_ = 0;

    control_0_.fill(0);
    control_1_.fill(0);
    for (auto &line : lines_) line = {};
    for (auto &row : rows_) row = {};
}

void GameLines::observe(GameMemory &memory, const f3_cpu &cpu) {
    switch (cpu.pc) {
    case 0x010044:
        control_0_.fill(uint16_t(cpu.d[0]));
        control_1_[4] = control_1_[5] = uint16_t(cpu.d[0]);
        break;
    case 0x00136e: {
        // Display register uploader (0x136e..0x1466)
        // High-level source: work RAM at A5=0x408000
        const uint16_t ctrl_word = memory.u16(0x4000ec);
        const uint16_t flip = memory.u16(0x40013e);
        flipscreen_ = flip;

        int32_t pf_x[4], pf_y[4];
        for (int i = 0; i < 4; ++i) {
            pf_x[i] = int32_t(memory.u32(0x400116 + i * 8));
            pf_y[i] = int32_t(memory.u32(0x40011a + i * 8));
            if (flip) {
                pf_x[i] = -pf_x[i];
                pf_y[i] = -pf_y[i];
            }
        }

        // Read calibration base table at 0x4000d8
        uint32_t base_ptr = 0x4000d8;
        for (int i = 0; i < 4; ++i) {
            // X scroll: NOT.W then ASR.L #10, then add base table word
            int32_t x_val = pf_x[i];
            x_val ^= 0xffff;
            int16_t x_shifted = int16_t(x_val >> 10);
            const uint16_t base_x = memory.u16(base_ptr); base_ptr += 2;
            control_0_[i] = uint16_t(x_shifted + int16_t(base_x));

            // Y scroll: ASR.L #9, then add base table word
            int32_t y_val = pf_y[i];
            int16_t y_shifted = int16_t(y_val >> 9);
            const uint16_t base_y = memory.u16(base_ptr); base_ptr += 2;
            control_0_[i + 4] = uint16_t(y_shifted + int16_t(base_y));
        }

        // Pivot X / Y: 16-bit words at 0x400136, 0x40013a
        const int16_t piv_x = int16_t(memory.u16(0x400136));
        const uint16_t base_piv_x = memory.u16(base_ptr); base_ptr += 2;
        control_1_[4] = uint16_t(piv_x + int16_t(base_piv_x));

        const int16_t piv_y = int16_t(memory.u16(0x40013a));
        const uint16_t base_piv_y = memory.u16(base_ptr); base_ptr += 2;
        control_1_[5] = uint16_t(piv_y + int16_t(base_piv_y));

        control_1_[7] = ctrl_word;
        break;
    }

    case 0x005cd8: {
        // Line profile initialization to the 232-line window
        load_default_profile(memory);
        supported_ = true;
        unsupported_pc_ = 0;
        break;
    }

    case 0x005d10: {
        // Line latches init (ROM 0x5d10..0x5d62)
        // Does NOT reload profile values; only sets pivot_control=8 at line 0 (or 255 if flipped)
        const bool flip_w = (memory.u16(0x40013e) != 0);
        const unsigned p_line = flip_w ? 255 : 0;
        lines_[p_line].pivot.pivot_control = 0x08;
        break;
    }

    case 0x005a22: {
        // PF0 clear: reset rowscroll (ROM 0x5a22..0x5a58)
        for (auto &line : lines_) {
            line.pf[0].rowscroll = 0;
        }
        break;
    }

    case 0x005a5e: {
        // PF1 clear: reset rowscroll (ROM 0x5a5e..0x5a94)
        for (auto &line : lines_) {
            line.pf[1].rowscroll = 0;
        }
        break;
    }

    case 0x005a9a: {
        // PF2 clear: reset rowscroll, zoom, colscroll (ROM 0x5a9a..0x5aee)
        // Does NOT touch mix or pal_add
        for (auto &line : lines_) {
            line.pf[2].rowscroll = 0;
            line.pf[2].x_scale = 256;
            line.pf[2].y_scale = 256;
            line.pf[2].colscroll = 0;
        }
        break;
    }

    case 0x005af4: {
        // PF3 clear: reset rowscroll, zoom, colscroll (ROM 0x5af4..0x5b48)
        // Does NOT touch mix or pal_add
        for (auto &line : lines_) {
            line.pf[3].rowscroll = 0;
            line.pf[3].x_scale = 256;
            line.pf[1].y_scale = 256; // PF3 zoom low byte controls PF1 vertical step.
            line.pf[3].colscroll = 0;
        }
        break;
    }


    case 0x091490:
    case 0x091506: {
        const bool flipped = memory.u16(0x40013e) != 0;
        const unsigned start = cpu.pc == 0x091490 ? (flipped ? 199 : 224) : (flipped ? 181 : 202);
        for (unsigned y = start; y < start + 20; ++y) {
            lines_[y].pivot.set_mix(0x380f);
            lines_[y].pf[3].set_mix(0x3800);
        }
        break;
    }
    case 0x0915d2: {
        // Character select water line effect
        // 76 lines: scanlines 176..251 (0x4b count in loop)
        for (unsigned y = 176; y <= 251 && y < 256; ++y) {
            auto &line = lines_[y];
            // Alpha blend: 0xbcba (idx 0: 15-10=5, idx 1: 15-11=4, idx 2: 15-12=3, idx 3: 15-11=4)
            line.blend = {5, 4, 3, 4};
            // PF3 mix: 0x380e (prio 14, blend 0, enabled, plane 3 clip enabled, clip_inv_mode=true)
            line.pf[3].set_mix(0x380e);
            // Sprite priority: 0xdd81 (prio: [1, 8, 13, 13])
            line.sp[0].prio = 1;
            line.sp[1].prio = 8;
            line.sp[2].prio = 13;
            line.sp[3].prio = 13;
            // Blend mode: 0x00eb (sp blend: [3, 2, 2, 3])
            line.sp[0].blend_mode = 3;
            line.sp[1].blend_mode = 2;
            line.sp[2].blend_mode = 2;
            line.sp[3].blend_mode = 3;
            line.pivot.pivot_control = 0;
            line.pivot.blend_select_v = false;
        }
        break;
    }

    case 0x091834: {
        // Character select water clip animation
        const uint16_t amount = uint16_t(uint16_t(60 - uint16_t(cpu.d[7])) * 6);
        const bool flipped = memory.u16(0x40013e) != 0;
        for (unsigned y = 176; y <= 251 && y < 256; ++y) {
            if (flipped) {
                lines_[y].clip[3].left = int16_t((0x6980 - amount) & 511);
                lines_[y].clip[3].right = 0x1ff;
            } else {
                lines_[y].clip[3].left = 0;
                lines_[y].clip[3].right = amount & 511;
            }
            lines_[y].pf[3].colscroll = 0;
            lines_[y].clip[2].left &= 255;
            lines_[y].clip[2].right &= 255;
        }
        break;
    }

    case 0x09217c:
        for (unsigned y = 24; y < 216; ++y)
            for (auto &sprite : lines_[y].sp) sprite.prio = 14;
        break;

    case 0x098dba:
        for (auto &line : lines_) line.blend = {3, 4, 5, 4}; // ROM $babc.
        break;

    case 0x099b5a:
        for (unsigned y = 24; y < 249; ++y) {
            auto &line = lines_[y];
            line.pivot.pivot_control = 0;
            line.pivot.blend_select_v = false;
            for (unsigned group = 0; group < 4; ++group) {
                line.sp[group].blend_mode = (0xdf >> (group * 2)) & 3;
                line.sp[group].prio = (0xdd88 >> (group * 4)) & 15;
            }
        }
        break;
    case 0x099f86:
        for (unsigned y = 0; y < 248; ++y) {
            lines_[y].pf[2].set_mix(0x700b);
            lines_[y].pf[3].set_mix(0xb00d);
            lines_[y].blend = {0, 4, 8, 4};
            auto &next = lines_[y + 1];
            next.pivot.pivot_control = 0;
            next.pivot.blend_select_v = false;
            for (unsigned group = 0; group < 4; ++group) {
                next.sp[group].prio = (0xcc88 >> (group * 4)) & 15;
                next.sp[group].blend_mode = 3;
            }
        }
        break;
    case 0x09a252:
    case 0x09a2f6:
        for (unsigned y = 0; y < 248; ++y)
            for (unsigned slot = 0; slot < 4; ++slot)
                lines_[y].blend[slot] = std::min(8u, 15u - ((cpu.d[2] >> (slot * 4)) & 15));
        break;
    case 0x09a28a:
        for (unsigned y = 0; y < 248; ++y) {
            lines_[y].pf[1].set_mix(0x700b);
            lines_[y].pf[3].set_mix(0x300d);
            lines_[y].blend = {0, 4, 8, 4};
        }
        break;
    case 0x09a6e6:
    case 0x09acbe:
        for (unsigned y = 0; y < 248; ++y) lines_[y].pf[2].set_mix(0x700c);
        break;
    case 0x09ad3e:
        for (unsigned y = 0; y < 248; ++y) lines_[y].pf[2].set_mix(0x300c);
        break;
    case 0x09a8de:
        for (unsigned y = 0; y < 248; ++y)
            for (unsigned group = 0; group < 4; ++group)
                lines_[y].sp[group].prio = (0xee88 >> (group * 4)) & 15;
        break;

    case 0x09d66a: {
        // In-game puzzle gameboard effect setup (ROM 0x9d66a..0x9d6a6)
        const bool flipped = (flipscreen_ != 0);
        const unsigned start_line = flipped ? 128 : 152;

        // Apply PF2 mix = 0x3005 (prio 5) across all lines
        for (auto &line : lines_) {
            line.pf[2].set_mix(0x3005);
        }

        // Read palette add gradient directly from GameMemory at 0x9d6a8
        uint32_t table_ptr = 0x09d6a8;
        unsigned cur_line = start_line;
        while (table_ptr < 0x09d6d8) {
            int16_t count = int16_t(memory.u16(table_ptr)); table_ptr += 2;
            if (count <= 0) break;
            uint16_t val = memory.u16(table_ptr); table_ptr += 2;
            for (int k = 0; k < count && cur_line < 256; ++k, ++cur_line) {
                lines_[cur_line].pf[2].pal_add = val * 16;
            }
        }

        // Fall through to compute trapezoidal zoom & rowscroll
        [[fallthrough]];
    }

    case 0x09d72a: {
        // Gameboard trapezoidal zoom and centering rowscroll (PF2)
        // Exact ROM 0x9d72a..0x9d7ae trace:
        const bool flipped = (flipscreen_ != 0);
        if (!flipped) {
            // Normal: count 0x97 = 151 (152 iterations), origin at line 152
            int d1 = 0;
            for (int k = 0; k <= 0x97; ++k) {
                int line_down = 151 - k;
                int line_up = 152 + k;
                if (line_down >= 0 && line_down < 256) {
                    lines_[line_down].pf[2].x_scale = 256 - (d1 & 255);
                    lines_[line_down].pf[2].y_scale = 256;
                }
                if (line_up < 256) {
                    lines_[line_up].pf[2].x_scale = 256 - (d1 & 255);
                    lines_[line_up].pf[2].y_scale = 256;
                }
                d1 += 2;
            }
        } else {
            // Flipped: count 0x7f = 127 (128 iterations), origin at line 128
            int d1 = 0;
            for (int k = 0; k <= 0x7f; ++k) {
                int line_down = 127 - k;
                int line_up = 128 + k;
                if (line_down >= 0 && line_down < 256) {
                    lines_[line_down].pf[2].x_scale = 256 - (d1 & 255);
                    lines_[line_down].pf[2].y_scale = 256;
                }
                if (line_up < 256) {
                    lines_[line_up].pf[2].x_scale = 256 - (d1 & 255);
                    lines_[line_up].pf[2].y_scale = 256;
                }
                d1 += 2;
            }
        }

        // ROM 0x9d774..0x9d7aa: compute rowscroll for all 256 lines from x_scale
        for (unsigned y = 0; y < 256; ++y) {
            int d1_zoom = 256 - lines_[y].pf[2].x_scale;
            int prod = (d1_zoom + 1) * 0xac;
            int div = prod / 0x100;
            int rem = prod % 0x100;
            int d2 = (-rem) & 0x3f;
            uint16_t rowscroll_w = uint16_t((((div + 0x48) << 6) & 0xffc0) | d2);
            int32_t rs = int32_t(rowscroll_w) << 2;
            lines_[y].pf[2].rowscroll = (rs & 0xffffff00) - (rs & 0x000000ff);
        }
        break;
    }

    case 0x09d7b6: {
        // Observe AFTER the task's TRAP #5 wake, not before its frame yield.
        const uint16_t phase = memory.u16(cpu.a[5] - 0x6f6) & 127;
        const bool flipped = memory.u16(0x40013e) != 0;
        const unsigned center = flipped ? 128 : 152;
        for (unsigned y = 0; y < 256; ++y) {
            auto &line = lines_[y];
            line.pf[2].colscroll = y < center ? (flipped ? phase : 511 - phase) : (flipped ? 510 - phase : phase);
            for (unsigned plane = 0; plane < 2; ++plane) {
                line.clip[plane].left &= 255;
                line.clip[plane].right &= 255;
            }
        }
        // The normal uploader has 152 iterations on BOTH sides of row 152;
        // its lower half continues into PF3 rows 0..47 (no end check in ROM).
        if (!flipped) for (unsigned y = 0; y < 48; ++y) {
            auto &line = lines_[y];
            line.pf[3].colscroll = phase;
            for (unsigned plane = 2; plane < 4; ++plane) {
                line.clip[plane].left &= 255;
                line.clip[plane].right &= 255;
            }
        }
        break;
    }

    case 0x09ecb0: {
        // PF0 wavy rowscroll (sine wave)
        // Exact ROM 0x9ecb0..0x9ecf4 trace:
        // Reads initial phase from u16(A5-0x6f6)&0xff
        const uint16_t step_base = memory.u16(cpu.a[5] - 0x6f6) & 0xff;
        uint16_t d4 = step_base;
        for (unsigned y = 0; y < 256; ++y) {
            const int16_t ext_d0 = int16_t(int8_t(d4 & 0xff));
            const uint32_t sin_offset = uint32_t(int32_t(0x1c84) + ext_d0 * 4);
            int32_t d0 = int32_t(memory.u32(sin_offset)) * 32;
            const uint16_t d1 = uint16_t(-int16_t(d0 & 0xffff));
            d0 = int32_t((uint32_t(d0) & 0xffff0000u) | d1);
            const uint32_t shifted = uint32_t(d0) << 6;
            const uint16_t word = uint16_t((shifted >> 16) | (shifted << 16));
            const int32_t rs = int32_t(word) << 2;
            lines_[y].pf[0].rowscroll = (rs & 0xffffff00) - (rs & 0x000000ff);
            d4 = (d4 + 2) & 0xff;
        }
        break;
    }

    case 0x08cfba:
    case 0x08cfe0: {
        const unsigned start = memory.u16(0x400140) / 2;
        for (unsigned n = 0; n < 232 && start + n < 256; ++n) {
            const uint16_t value = cpu.pc == 0x08cfba ? 0xbdbd : memory.u16(0x41ce26 + n * 2);
            for (unsigned slot = 0; slot < 4; ++slot)
                lines_[start + n].blend[slot] = std::min(8u, 15u - ((value >> (slot * 4)) & 15));
        }
        break;
    }

    case 0x0fe620:
    case 0x0fefe6:
    case 0x0ff0fa: {
        // Ending slideshow effects are explicitly unsupported; fallback to oracle
        supported_ = false;
        unsupported_pc_ = cpu.pc;
        break;
    }

    default:
        break;
    }

    if (!memory.supported) {
        supported_ = false;
        if (!unsupported_pc_) unsupported_pc_ = cpu.pc;
    }
}

void GameLines::get_pf_scroll(int pf_num, int32_t &reg_sx, int32_t &reg_sy, bool flipped) const {
    int16_t sx_raw = int16_t(control_0_[pf_num]);
    int16_t sy_raw = int16_t(control_0_[pf_num + 4]);

    sy_raw += (1 << 7); // 9.7 fixed point

    if (flipped) {
        sx_raw += (320 << 6);
        sx_raw += ((512 + 192) << 6);
        sy_raw = -sy_raw;
    }

    sx_raw += ((40 - 4 * pf_num) << 6);

    int32_t sx = int32_t(sx_raw) << (8 - 6);
    int32_t sy = int32_t(sy_raw) << (8 - 7);
    sx ^= 0b1111'1100;
    if (flipped) {
        sx = sx - (46 << 8);
        sy = -sy;
    } else {
        sx = sx - (46 << 8);
    }

    reg_sx = sx;
    reg_sy = sy;
}

void GameLines::prepare(bool flipped) {
    std::array<int32_t, 4> reg_sx{};
    std::array<int32_t, 4> reg_sy{};
    std::array<int32_t, 4> reg_fx_y{};
    for (int pf = 0; pf < 4; ++pf) {
        get_pf_scroll(pf, reg_sx[pf], reg_sy[pf], flipped);
        reg_fx_y[pf] = reg_sy[pf];
    }

    const int16_t text_reg_sx = flipped ? (int16_t(control_1_[4]) - 12) : (-int16_t(control_1_[4]) - 5);
    const int16_t text_reg_sy = flipped ? int16_t(control_1_[5]) : -int16_t(control_1_[5]);

    for (unsigned screen_y = 0; screen_y < 256; ++screen_y) {
        const int y = flipped ? (255 - int(screen_y)) : int(screen_y);
        const auto &line = lines_[y];
        auto &row = rows_[screen_y];

        // Clips (calibrated oracle l-1/r-2)
        for (int i = 0; i < 4; ++i) {
            row.clips[i].left = int16_t(line.clip[i].left - 1);
            row.clips[i].right = int16_t(line.clip[i].right - 2);
        }

        // Blend
        row.blend = line.blend;

        // Background
        row.background = line.bg_palette;

        // Mosaic period
        row.mosaic_period = line.x_sample;

        // Bitmap mode
        row.bitmap = (line.pivot.pivot_control & 0xa0) != 0;

        // Text layer
        row.text.priority = line.pivot.prio;
        row.text.blend_mode = line.pivot.blend_mode;
        row.text.clip_enabled = line.pivot.clip_enable;
        row.text.clip_inverted = line.pivot.clip_inv;
        row.text.clip_inverse = line.pivot.clip_inv_mode;
        row.text.enabled = (line.pivot.mix_value & 0x2000) != 0 && line.pivot.blend_mode != 0b11;
        row.text.blend_select = line.pivot.blend_select_v;
        row.text.mosaic = line.pivot.x_sample_enable;

        row.text_x = int16_t((46 + text_reg_sx) & 0x1ff);
        int line_y_text = (text_reg_sy + int(screen_y)) & (row.bitmap ? 0xff : 0x1ff);
        // source_y / text_y contract is BEFORE global flip
        row.text_y = int16_t(line_y_text);

        // Sprite layers
        for (int i = 0; i < 4; ++i) {
            row.sprites[i].priority = line.sp[i].prio;
            row.sprites[i].blend_mode = line.sp[i].blend_mode;
            row.sprites[i].clip_enabled = line.sp[i].clip_enable;
            row.sprites[i].clip_inverted = line.sp[i].clip_inv;
            row.sprites[i].clip_inverse = line.sp[i].clip_inv_mode;
            row.sprites[i].enabled = (line.sp[i].mix_value & 0x2000) != 0 && line.sp[i].blend_mode != 0b00;
            row.sprites[i].blend_select = line.sp[i].blend_select_v;
            row.sprites[i].mosaic = line.sp[i].x_sample_enable;
        }

        // Playfields
        for (int i = 0; i < 4; ++i) {
            row.playfields[i].layer.priority = line.pf[i].prio;
            row.playfields[i].layer.blend_mode = line.pf[i].blend_mode;
            row.playfields[i].layer.clip_enabled = line.pf[i].clip_enable;
            row.playfields[i].layer.clip_inverted = line.pf[i].clip_inv;
            row.playfields[i].layer.clip_inverse = line.pf[i].clip_inv_mode;
            row.playfields[i].layer.enabled = (line.pf[i].mix_value & 0x2000) != 0 && line.pf[i].blend_mode != 0b11;
            row.playfields[i].layer.blend_select = false;
            row.playfields[i].layer.mosaic = line.pf[i].x_sample_enable;

            int32_t reg_fx_x = reg_sx[i] + line.pf[i].rowscroll;
            reg_fx_x += 10 * (line.pf[i].x_scale - 256);
            row.playfields[i].source_x = reg_fx_x + (46 << 8);
            row.playfields[i].x_step = line.pf[i].x_scale;
            row.playfields[i].y_step = line.pf[i].y_scale;
            row.playfields[i].y_fraction = uint8_t(reg_fx_y[i]);
            row.playfields[i].palette_add = line.pf[i].pal_add;

            int32_t line_y_pf = ((reg_fx_y[i] >> 8) + line.pf[i].colscroll) & 0x1ff;
            // source_y / text_y contract is BEFORE global flip
            row.playfields[i].source_y = line_y_pf;
        }

        if (screen_y != 0) {
            for (int i = 0; i < 4; ++i) {
                reg_fx_y[i] += line.pf[i].y_scale;
            }
        }
    }
}

void GameLines::compare_rows(const Video &oracle, uint64_t frame) {
    prepare(oracle.flipscreen());
    if (!supported()) {
        std::ostringstream ss;
        ss << "GameLines unsupported producer at frame " << frame
           << " PC 0x" << std::hex << unsupported_pc();
        throw std::runtime_error(ss.str());
    }

    for (unsigned screen_y = 24; screen_y < 256; ++screen_y) {
        const auto &g = row(screen_y);
        const auto &o = oracle.inspect_scene_row(screen_y);

        if (g.background != o.background) {
            std::ostringstream ss;
            ss << "Frame " << frame << " row " << screen_y
               << " background mismatch: game 0x" << std::hex << g.background
               << " vs oracle 0x" << o.background;
            throw std::runtime_error(ss.str());
        }

        if (g.mosaic_period != o.mosaic_period) {
            std::ostringstream ss;
            ss << "Frame " << frame << " row " << screen_y
               << " mosaic_period mismatch: game " << int(g.mosaic_period)
               << " vs oracle " << int(o.mosaic_period);
            throw std::runtime_error(ss.str());
        }

        if (g.bitmap != o.bitmap) {
            std::ostringstream ss;
            ss << "Frame " << frame << " row " << screen_y
               << " bitmap mismatch: game " << g.bitmap
               << " vs oracle " << o.bitmap;
            throw std::runtime_error(ss.str());
        }

        for (int i = 0; i < 4; ++i) {
            if (g.blend[i] != o.blend[i]) {
                std::ostringstream ss;
                ss << "Frame " << frame << " row " << screen_y << " blend[" << i
                   << "] mismatch: game " << int(g.blend[i])
                   << " vs oracle " << int(o.blend[i]);
                throw std::runtime_error(ss.str());
            }
            if (g.clips[i].left != o.clips[i].left || g.clips[i].right != o.clips[i].right) {
                std::ostringstream ss;
                ss << "Frame " << frame << " row " << screen_y << " clip[" << i
                   << "] mismatch: game [" << g.clips[i].left << ", " << g.clips[i].right
                   << "] vs oracle [" << o.clips[i].left << ", " << o.clips[i].right << "]";
                throw std::runtime_error(ss.str());
            }
        }

        // Text layer
        if (g.text.enabled != o.text.enabled ||
            g.text.priority != o.text.priority ||
            g.text.blend_mode != o.text.blend_mode ||
            g.text.clip_enabled != o.text.clip_enabled ||
            g.text.clip_inverted != o.text.clip_inverted ||
            g.text.clip_inverse != o.text.clip_inverse ||
            g.text.blend_select != o.text.blend_select ||
            g.text.mosaic != o.text.mosaic) {
            std::ostringstream ss;
            ss << "Frame " << frame << " row " << screen_y
               << " text layer mismatch: game " << format_layer(g.text)
               << " vs oracle " << format_layer(o.text);
            throw std::runtime_error(ss.str());
        }
        if (g.text.enabled) {
            if (g.text_x != o.text_x || g.text_y != o.text_y) {
                std::ostringstream ss;
                ss << "Frame " << frame << " row " << screen_y
                   << " text coords mismatch: game (" << g.text_x << ", " << g.text_y
                   << ") vs oracle (" << o.text_x << ", " << o.text_y << ")";
                throw std::runtime_error(ss.str());
            }
        }

        // Sprites
        for (int i = 0; i < 4; ++i) {
            if (g.sprites[i].enabled != o.sprites[i].enabled ||
                g.sprites[i].priority != o.sprites[i].priority ||
                g.sprites[i].blend_mode != o.sprites[i].blend_mode ||
                g.sprites[i].clip_enabled != o.sprites[i].clip_enabled ||
                g.sprites[i].clip_inverted != o.sprites[i].clip_inverted ||
                g.sprites[i].clip_inverse != o.sprites[i].clip_inverse ||
                g.sprites[i].blend_select != o.sprites[i].blend_select ||
                g.sprites[i].mosaic != o.sprites[i].mosaic) {
                std::ostringstream ss;
                ss << "Frame " << frame << " row " << screen_y << " sprite[" << i
                   << "] mismatch: game " << format_layer(g.sprites[i])
                   << " vs oracle " << format_layer(o.sprites[i]);
                throw std::runtime_error(ss.str());
            }
        }

        // Playfields
        for (int i = 0; i < 4; ++i) {
            const auto &g_pf = g.playfields[i];
            const auto &o_pf = o.playfields[i];
            if (g_pf.layer.enabled != o_pf.layer.enabled ||
                g_pf.layer.priority != o_pf.layer.priority ||
                g_pf.layer.blend_mode != o_pf.layer.blend_mode ||
                g_pf.layer.clip_enabled != o_pf.layer.clip_enabled ||
                g_pf.layer.clip_inverted != o_pf.layer.clip_inverted ||
                g_pf.layer.clip_inverse != o_pf.layer.clip_inverse ||
                g_pf.layer.blend_select != o_pf.layer.blend_select ||
                g_pf.layer.mosaic != o_pf.layer.mosaic) {
                std::ostringstream ss;
                ss << "Frame " << frame << " row " << screen_y << " PF[" << i
                   << "] layer mismatch: game " << format_layer(g_pf.layer)
                   << " vs oracle " << format_layer(o_pf.layer);
                throw std::runtime_error(ss.str());
            }
            if (g_pf.layer.enabled) {
                if (g_pf.source_x != o_pf.source_x ||
                    g_pf.source_y != o_pf.source_y ||
                    g_pf.x_step != o_pf.x_step ||
                    g_pf.y_step != o_pf.y_step ||
                    g_pf.y_fraction != o_pf.y_fraction ||
                    g_pf.palette_add != o_pf.palette_add) {
                    std::ostringstream ss;
                    ss << "Frame " << frame << " row " << screen_y << " PF[" << i
                       << "] coords mismatch: game {sx=" << g_pf.source_x
                       << " sy=" << g_pf.source_y << " step=" << g_pf.x_step
                       << " y_step=" << g_pf.y_step << " y_phase=" << int(g_pf.y_fraction)
                       << " pal_add=" << g_pf.palette_add
                       << "} vs oracle {sx=" << o_pf.source_x
                       << " sy=" << o_pf.source_y << " step=" << o_pf.x_step
                       << " y_step=" << o_pf.y_step << " y_phase=" << int(o_pf.y_fraction)
                       << " pal_add=" << o_pf.palette_add << "}";
                    throw std::runtime_error(ss.str());
                }
            }
        }
    }
}

} // namespace f3rt
