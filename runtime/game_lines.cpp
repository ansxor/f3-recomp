#include "game_lines.hpp"
#include "f3rt/video.hpp"
#include "state_io.hpp"
#include <algorithm>
#include <sstream>
#include <stdexcept>

namespace f3rt {
namespace {

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

void GameLines::reset() {
    control_0_.fill(0);
    control_1_.fill(0);
    for (auto &line : lines_) line = {};
    for (auto &row : rows_) row = {};
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

static void pack_layer(const SceneLayer &s, CanonicalSceneLayer &d) {
    d.priority = s.priority;
    d.blend_mode = s.blend_mode;
    d.clip_enabled = s.clip_enabled;
    d.clip_inverted = s.clip_inverted;
    d.clip_inverse = s.clip_inverse ? 1 : 0;
    d.enabled = s.enabled ? 1 : 0;
    d.blend_select = s.blend_select ? 1 : 0;
    d.mosaic = s.mosaic ? 1 : 0;
}

static void unpack_layer(const CanonicalSceneLayer &s, SceneLayer &d) {
    d.priority = s.priority;
    d.blend_mode = s.blend_mode;
    d.clip_enabled = s.clip_enabled;
    d.clip_inverted = s.clip_inverted;
    d.clip_inverse = s.clip_inverse != 0;
    d.enabled = s.enabled != 0;
    d.blend_select = s.blend_select != 0;
    d.mosaic = s.mosaic != 0;
}

size_t GameLines::state_size() const {
    return sizeof(CanonicalLineParams) * 256 +
           sizeof(CanonicalSceneRow) * 256;
}

void GameLines::save_state(StateWriter &writer) const {
    for (size_t i = 0; i < 256; ++i) {
        const auto &s = lines_[i];
        CanonicalLineParams d{};
        for (int c = 0; c < 4; ++c) {
            d.clip[c].left = s.clip[c].left;
            d.clip[c].right = s.clip[c].right;
        }
        std::copy_n(s.blend.data(), 4, d.blend);
        d.x_sample = s.x_sample;
        d.bg_palette = s.bg_palette;
        d.pivot.mix_value = s.pivot.mix_value;
        d.pivot.prio = s.pivot.prio;
        d.pivot.blend_mode = s.pivot.blend_mode;
        d.pivot.clip_enable = s.pivot.clip_enable;
        d.pivot.clip_inv = s.pivot.clip_inv;
        d.pivot.clip_inv_mode = s.pivot.clip_inv_mode ? 1 : 0;
        d.pivot.blend_select_v = s.pivot.blend_select_v ? 1 : 0;
        d.pivot.x_sample_enable = s.pivot.x_sample_enable ? 1 : 0;
        d.pivot.pivot_control = s.pivot.pivot_control;
        d.pivot.pivot_enable = s.pivot.pivot_enable;
        for (int p = 0; p < 4; ++p) {
            d.sp[p].mix_value = s.sp[p].mix_value;
            d.sp[p].prio = s.sp[p].prio;
            d.sp[p].blend_mode = s.sp[p].blend_mode;
            d.sp[p].clip_enable = s.sp[p].clip_enable;
            d.sp[p].clip_inv = s.sp[p].clip_inv;
            d.sp[p].clip_inv_mode = s.sp[p].clip_inv_mode ? 1 : 0;
            d.sp[p].blend_select_v = s.sp[p].blend_select_v ? 1 : 0;
            d.sp[p].x_sample_enable = s.sp[p].x_sample_enable ? 1 : 0;

            d.pf[p].mix_value = s.pf[p].mix_value;
            d.pf[p].prio = s.pf[p].prio;
            d.pf[p].blend_mode = s.pf[p].blend_mode;
            d.pf[p].clip_enable = s.pf[p].clip_enable;
            d.pf[p].clip_inv = s.pf[p].clip_inv;
            d.pf[p].clip_inv_mode = s.pf[p].clip_inv_mode ? 1 : 0;
            d.pf[p].x_sample_enable = s.pf[p].x_sample_enable ? 1 : 0;
            d.pf[p].colscroll = s.pf[p].colscroll;
            d.pf[p].x_scale = s.pf[p].x_scale;
            d.pf[p].y_scale = s.pf[p].y_scale;
            d.pf[p].pal_add = s.pf[p].pal_add;
            d.pf[p].rowscroll = s.pf[p].rowscroll;
        }
        writer.write(d);
    }
    for (size_t i = 0; i < 256; ++i) {
        const auto &s = rows_[i];
        CanonicalSceneRow d{};
        for (int p = 0; p < 4; ++p) {
            pack_layer(s.playfields[p].layer, d.playfields[p].layer);
            d.playfields[p].source_x = s.playfields[p].source_x;
            d.playfields[p].source_y = s.playfields[p].source_y;
            d.playfields[p].x_step = s.playfields[p].x_step;
            d.playfields[p].y_step = s.playfields[p].y_step;
            d.playfields[p].y_fraction = s.playfields[p].y_fraction;
            d.playfields[p].palette_add = s.playfields[p].palette_add;
        }
        for (int sp = 0; sp < 4; ++sp) pack_layer(s.sprites[sp], d.sprites[sp]);
        pack_layer(s.text, d.text);
        for (int c = 0; c < 4; ++c) {
            d.clips[c].left = s.clips[c].left;
            d.clips[c].right = s.clips[c].right;
        }
        std::copy_n(s.blend.data(), 4, d.blend);
        d.background = s.background;
        d.text_x = s.text_x;
        d.text_y = s.text_y;
        d.mosaic_period = s.mosaic_period;
        d.bitmap = s.bitmap ? 1 : 0;
        writer.write(d);
    }
}

void GameLines::load_state(StateReader &reader) {
    for (size_t i = 0; i < 256; ++i) {
        CanonicalLineParams s;
        reader.read(s);
        auto &d = lines_[i];
        for (int c = 0; c < 4; ++c) {
            d.clip[c].left = s.clip[c].left;
            d.clip[c].right = s.clip[c].right;
        }
        std::copy_n(s.blend, 4, d.blend.data());
        d.x_sample = s.x_sample;
        d.bg_palette = s.bg_palette;
        d.pivot.mix_value = s.pivot.mix_value;
        d.pivot.prio = s.pivot.prio;
        d.pivot.blend_mode = s.pivot.blend_mode;
        d.pivot.clip_enable = s.pivot.clip_enable;
        d.pivot.clip_inv = s.pivot.clip_inv;
        d.pivot.clip_inv_mode = s.pivot.clip_inv_mode != 0;
        d.pivot.blend_select_v = s.pivot.blend_select_v != 0;
        d.pivot.x_sample_enable = s.pivot.x_sample_enable != 0;
        d.pivot.pivot_control = s.pivot.pivot_control;
        d.pivot.pivot_enable = s.pivot.pivot_enable;
        for (int p = 0; p < 4; ++p) {
            d.sp[p].mix_value = s.sp[p].mix_value;
            d.sp[p].prio = s.sp[p].prio;
            d.sp[p].blend_mode = s.sp[p].blend_mode;
            d.sp[p].clip_enable = s.sp[p].clip_enable;
            d.sp[p].clip_inv = s.sp[p].clip_inv;
            d.sp[p].clip_inv_mode = s.sp[p].clip_inv_mode != 0;
            d.sp[p].blend_select_v = s.sp[p].blend_select_v != 0;
            d.sp[p].x_sample_enable = s.sp[p].x_sample_enable != 0;

            d.pf[p].mix_value = s.pf[p].mix_value;
            d.pf[p].prio = s.pf[p].prio;
            d.pf[p].blend_mode = s.pf[p].blend_mode;
            d.pf[p].clip_enable = s.pf[p].clip_enable;
            d.pf[p].clip_inv = s.pf[p].clip_inv;
            d.pf[p].clip_inv_mode = s.pf[p].clip_inv_mode != 0;
            d.pf[p].x_sample_enable = s.pf[p].x_sample_enable != 0;
            d.pf[p].colscroll = s.pf[p].colscroll;
            d.pf[p].x_scale = s.pf[p].x_scale;
            d.pf[p].y_scale = s.pf[p].y_scale;
            d.pf[p].pal_add = s.pf[p].pal_add;
            d.pf[p].rowscroll = s.pf[p].rowscroll;
        }
    }
    for (size_t i = 0; i < 256; ++i) {
        CanonicalSceneRow s;
        reader.read(s);
        auto &d = rows_[i];
        for (int p = 0; p < 4; ++p) {
            unpack_layer(s.playfields[p].layer, d.playfields[p].layer);
            d.playfields[p].source_x = s.playfields[p].source_x;
            d.playfields[p].source_y = s.playfields[p].source_y;
            d.playfields[p].x_step = s.playfields[p].x_step;
            d.playfields[p].y_step = s.playfields[p].y_step;
            d.playfields[p].y_fraction = s.playfields[p].y_fraction;
            d.playfields[p].palette_add = s.playfields[p].palette_add;
        }
        for (int sp = 0; sp < 4; ++sp) unpack_layer(s.sprites[sp], d.sprites[sp]);
        unpack_layer(s.text, d.text);
        for (int c = 0; c < 4; ++c) {
            d.clips[c].left = s.clips[c].left;
            d.clips[c].right = s.clips[c].right;
        }
        std::copy_n(s.blend, 4, d.blend.data());
        d.background = s.background;
        d.text_x = s.text_x;
        d.text_y = s.text_y;
        d.mosaic_period = s.mosaic_period;
        d.bitmap = s.bitmap != 0;
    }
}

} // namespace f3rt
