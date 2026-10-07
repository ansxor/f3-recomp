// Land Maker scanline-layer VRAM decode.
//
// The 256 per-scanline line parameters are decoded directly from line RAM
// (0x620000, 0x10000 bytes) plus the FDP control registers, mirroring
// Video::read_line_ram in runtime/video.cpp (the reference renderer). Fields are
// carried forward between scanlines whenever a subsection latch is absent, so the
// decode walks all 256 lines in order into a persistent scratch line.
#include "game_lines.hpp"
#ifdef F3RT_VIDEO_WRITE_LOG
#include "game_video_log.hpp"
#endif
#include <algorithm>

namespace f3rt {
namespace {

inline uint16_t read_be16(const uint8_t *p) {
    return (uint16_t(p[0]) << 8) | uint16_t(p[1]);
}

#ifdef F3RT_VIDEO_WRITE_LOG
bool is_covered_write(uint32_t pc) {
    static constexpr struct Range { uint32_t start, end; } ranges[] = {
        {0x00136e, 0x00145e}, // Display register uploader (0x660000..0x66001e)
        {0x005a2e, 0x005a42}, // PF0 clear rowscroll (0x62a000)
        {0x005a6a, 0x005a7e}, // PF1 clear rowscroll (0x62a200)
        {0x005aa6, 0x005ad8}, // PF2 clear rowscroll, zoom, colscroll
        {0x005b00, 0x005b32}, // PF3 clear rowscroll, zoom, colscroll
        {0x005d30, 0x005d6c}, // Line RAM profile init (0x624000..0x62b000, 0x620000)
        {0x010044, 0x01007c}, // Boot control registers (0x660000..0x66001a)
        {0x0100ba, 0x0100c4}, // Boot line RAM clear
        {0x08cfd2, 0x08cfda}, // Alpha blend save (0x626200)
        {0x08cff2, 0x08cff8}, // Alpha blend restore (0x626200)
        {0x0914b8, 0x0914c2}, // Water effect select init
        {0x09152e, 0x091538}, // Water effect select init
        {0x0915dc, 0x091610}, // Water effect main lines
        {0x09187a, 0x091882}, // Water clip window animation
        {0x09218a, 0x09218a}, // Selection transition sprite priorities
        {0x098dc8, 0x098dc8}, // Full-screen alpha profile (0x626200)
        {0x099b72, 0x099b74}, // Attract sprite modes and priorities
        {0x099fc2, 0x099fca}, // Attract reverse/normal blend transition
        {0x09a25c, 0x09a25c}, // Attract alpha fade from task D2
        {0x09a2ac, 0x09a2b0}, // Attract PF1/PF3 blend restoration
        {0x09a300, 0x09a300}, // Second attract alpha fade from task D2
        {0x09a6f4, 0x09a6f4}, // Attract PF2 blend/priority (0x62b400)
        {0x09a8ec, 0x09a8ec}, // Attract sprite priorities (0x627600)
        {0x09accc, 0x09accc}, // Attract PF2 blending on
        {0x09ad4c, 0x09ad4c}, // Attract PF2 blending off
        {0x09d684, 0x09d6a0}, // Gameboard PF2 palette add gradient
        {0x09d75e, 0x09d7aa}, // Gameboard PF2 zoom & rowscroll
        {0x09d7d6, 0x09d812}, // Gameboard PF2 column scroll
        {0x09ecde, 0x09ecee}, // PF0 wavy rowscroll (0x62a000)
    };
    for (const auto &r : ranges)
        if (pc >= r.start && pc <= r.end) return true;
    return false;
}
#endif

} // namespace

void GameLines::decode(const VideoRam &vram) {
    for (int i = 0; i < 8; ++i) {
        control_0_[i] = vram.control_u16(unsigned(i));
        control_1_[i] = vram.control_u16(unsigned(8 + i));
    }

    const uint8_t *lineram = &vram.graphics[0x20000];

    auto set_clip_upper = [](LineClip &c, int8_t left, int8_t right) {
        c.left = int16_t((c.left & 0xff) | (int16_t(left) << 8));
        c.right = int16_t((c.right & 0xff) | (int16_t(right) << 8));
    };
    auto set_clip_lower = [](LineClip &c, uint8_t left, uint8_t right) {
        c.left = int16_t((c.left & 0x100) | left);
        c.right = int16_t((c.right & 0x100) | right);
    };
    auto latched_addr = [lineram](uint8_t section, uint8_t subsection, int y) -> uint32_t {
        const uint16_t latches = read_be16(&lineram[section * 0x200 + y * 2]);
        const uint32_t base = 0x4000 + 0x1000 * section + 0x200 * subsection;
        if (latches & (1 << (subsection + 4)))
            return base + 0x800 + y * 2;
        else if (latches & (1 << subsection))
            return base + y * 2;
        return 0;
    };

    LineParams line{};
    for (int y = 0; y < 256; ++y) {
        // 4000 (column scroll & clip-plane high bits for PF 2 and 3)
        for (int i : { 2, 3 }) {
            if (const uint32_t where = latched_addr(0, uint8_t(i), y)) {
                const uint16_t colscroll = read_be16(&lineram[where]);
                line.pf[i].colscroll = colscroll & 0x1ff;
                set_clip_upper(line.clip[2 * (i - 2) + 0], (colscroll >> 12) & 1, (colscroll >> 13) & 1);
                set_clip_upper(line.clip[2 * (i - 2) + 1], (colscroll >> 14) & 1, (colscroll >> 15) & 1);
            }
        }

        // 5000 (clip planes low 8 bits)
        for (int i = 0; i < 4; ++i) {
            if (const uint32_t where = latched_addr(1, uint8_t(i), y)) {
                const uint16_t clip_lows = read_be16(&lineram[where]);
                set_clip_lower(line.clip[i], uint8_t(clip_lows & 0xff), uint8_t((clip_lows >> 8) & 0xff));
            }
        }

        // 6000 (sprite blend modes, pivot control, alpha values, mosaic, bg palette)
        if (const uint32_t where = latched_addr(2, 0, y)) {
            const uint16_t v = read_be16(&lineram[where]);
            line.pivot.blend_select_v = (v & 0x0200) != 0;
            line.pivot.pivot_control = uint8_t((v >> 8) & 0xff);
            for (int group = 0; group < 4; ++group) {
                const uint8_t blend = uint8_t((v >> (group * 2)) & 3);
                line.sp[group].mix_value = uint16_t((line.sp[group].mix_value & 0x3fff) | (uint16_t(blend) << 14));
                line.sp[group].blend_mode = blend;
            }
        }
        if (const uint32_t where = latched_addr(2, 1, y)) {
            const uint16_t blend_vals = read_be16(&lineram[where]);
            for (int idx = 0; idx < 4; ++idx) {
                const uint8_t alpha = uint8_t((blend_vals >> (4 * idx)) & 0x0f);
                line.blend[idx] = std::min<uint8_t>(8, uint8_t(0x0f - alpha));
            }
        }
        if (const uint32_t where = latched_addr(2, 2, y)) {
            const uint16_t x_mosaic = read_be16(&lineram[where]);
            line.x_sample = uint8_t(16 - ((x_mosaic >> 4) & 0x0f));
            for (int pf_num = 0; pf_num < 4; ++pf_num)
                line.pf[pf_num].x_sample_enable = (x_mosaic & (1 << pf_num)) != 0;
            for (auto &sp : line.sp)
                sp.x_sample_enable = (x_mosaic & 0x100) != 0;
            line.pivot.x_sample_enable = (x_mosaic & 0x200) != 0;
        }
        if (const uint32_t where = latched_addr(2, 3, y))
            line.bg_palette = read_be16(&lineram[where]);

        // 7000 (pivot & sprite mixing / priority)
        if (const uint32_t where = latched_addr(3, 0, y))
            line.pivot.pivot_enable = read_be16(&lineram[where]);
        if (const uint32_t where = latched_addr(3, 1, y))
            line.pivot.set_mix(read_be16(&lineram[where]));
        if (const uint32_t where = latched_addr(3, 2, y)) {
            const uint16_t sprite_mix = read_be16(&lineram[where]);
            for (int group = 0; group < 4; ++group) {
                const uint16_t v = uint16_t((line.sp[group].mix_value & 0xc00f) | ((sprite_mix & 0x3ff) << 4));
                line.sp[group].set_mix(v);
                line.sp[group].blend_select_v = ((sprite_mix >> (12 + group)) & 1) != 0;
            }
        }
        if (const uint32_t where = latched_addr(3, 3, y)) {
            const uint16_t sprite_prio = read_be16(&lineram[where]);
            for (int group = 0; group < 4; ++group) {
                const uint8_t prio = uint8_t((sprite_prio >> (group * 4)) & 0x0f);
                line.sp[group].mix_value = uint16_t((line.sp[group].mix_value & 0xfff0) | prio);
                line.sp[group].prio = prio;
            }
        }

        // 8000 (playfield zoom)
        static constexpr int FIX_Y[] = { 0, 3, 2, 1 };
        for (int i = 0; i < 4; ++i) {
            if (const uint32_t where = latched_addr(4, uint8_t(i), y)) {
                const uint16_t pf_scale = read_be16(&lineram[where]);
                line.pf[i].x_scale = 256 - ((pf_scale >> 8) & 0xff);
                line.pf[FIX_Y[i]].y_scale = (pf_scale & 0xff) << 1;
            }
        }

        // 9000 (playfield palette add)
        for (int i = 0; i < 4; ++i) {
            if (const uint32_t where = latched_addr(5, uint8_t(i), y))
                line.pf[i].pal_add = read_be16(&lineram[where]) * 16;
        }

        // A000 (playfield rowscroll)
        for (int i = 0; i < 4; ++i) {
            if (const uint32_t where = latched_addr(6, uint8_t(i), y)) {
                const int32_t rowscroll = int32_t(read_be16(&lineram[where])) << (8 - 6);
                line.pf[i].rowscroll = (rowscroll & 0xffffff00) - (rowscroll & 0x000000ff);
            }
        }

        // B000 (playfield mixing info)
        for (int i = 0; i < 4; ++i) {
            if (const uint32_t where = latched_addr(7, uint8_t(i), y))
                line.pf[i].set_mix(read_be16(&lineram[where]));
        }

        lines_[y] = line;
    }
}

#ifdef F3RT_VIDEO_WRITE_LOG
void GameLines::observe_write(uint32_t pc, uint32_t address, uint64_t frame) {
    address &= 0xffffff;
    const bool is_lineram = (address >= 0x620000 && address < 0x630000);
    const bool is_ctrl = (address >= 0x660000 && address < 0x660040);
    if (!is_lineram && !is_ctrl) return;
    if (is_covered_write(pc)) return;
    log_unknown_video_write("lines", pc, address, frame);
}
#endif

} // namespace f3rt
