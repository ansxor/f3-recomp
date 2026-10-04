#pragma once
#include "game_scene.hpp"
#include "f3rt/cpu_abi.h"
#include <array>
#include <cstdint>
#include <span>

namespace f3rt {
class Video;


class GameLines {
public:
    GameLines() { reset(); }

    void reset();
    void observe(GameMemory &memory, const f3_cpu &cpu);
    void prepare(bool flipped);
    const SceneRow &row(unsigned scanout_y) const { return rows_[scanout_y & 255]; }
    bool supported() const { return supported_; }
    uint32_t unsupported_pc() const { return unsupported_pc_; }
    void observe_write(uint32_t pc, uint32_t address);
    static std::span<const uint32_t> hooks();
    void compare_rows(const Video &oracle, uint64_t frame);

private:
    struct LineClip {
        int16_t left = 0;
        int16_t right = 0;
    };

    struct LinePivot {
        uint16_t mix_value = 0;
        uint8_t prio = 0;
        uint8_t blend_mode = 0;
        uint8_t clip_enable = 0;
        uint8_t clip_inv = 0;
        bool clip_inv_mode = false;
        bool blend_select_v = false;
        bool x_sample_enable = false;
        uint8_t pivot_control = 0;
        uint16_t pivot_enable = 0;

        void set_mix(uint16_t v) {
            mix_value = v;
            prio = v & 0x0f;
            blend_mode = (v >> 14) & 3;
            clip_inv = (v >> 4) & 0x0f;
            clip_enable = (v >> 8) & 0x0f;
            clip_inv_mode = (v & 0x1000) != 0;
        }
    };

    struct LineSprite {
        uint16_t mix_value = 0;
        uint8_t prio = 0;
        uint8_t blend_mode = 0;
        uint8_t clip_enable = 0;
        uint8_t clip_inv = 0;
        bool clip_inv_mode = false;
        bool blend_select_v = false;
        bool x_sample_enable = false;

        void set_mix(uint16_t v) {
            mix_value = v;
            prio = v & 0x0f;
            blend_mode = (v >> 14) & 3;
            clip_inv = (v >> 4) & 0x0f;
            clip_enable = (v >> 8) & 0x0f;
            clip_inv_mode = (v & 0x1000) != 0;
        }
    };

    struct LinePlayfield {
        uint16_t mix_value = 0;
        uint8_t prio = 0;
        uint8_t blend_mode = 0;
        uint8_t clip_enable = 0;
        uint8_t clip_inv = 0;
        bool clip_inv_mode = false;
        bool x_sample_enable = false;

        uint16_t colscroll = 0;
        int32_t x_scale = 256;
        int32_t y_scale = 0;
        uint16_t pal_add = 0;
        int32_t rowscroll = 0;

        void set_mix(uint16_t v) {
            mix_value = v;
            prio = v & 0x0f;
            blend_mode = (v >> 14) & 3;
            clip_inv = (v >> 4) & 0x0f;
            clip_enable = (v >> 8) & 0x0f;
            clip_inv_mode = (v & 0x1000) != 0;
        }
    };

    struct LineParams {
        std::array<LineClip, 4> clip{};
        std::array<uint8_t, 4> blend{8, 8, 8, 8};
        uint8_t x_sample = 16;
        uint16_t bg_palette = 0;
        LinePivot pivot{};
        std::array<LineSprite, 4> sp{};
        std::array<LinePlayfield, 4> pf{};
    };

    std::array<LineParams, 256> lines_{};
    std::array<SceneRow, 256> rows_{};

    std::array<uint16_t, 8> control_0_{};
    std::array<uint16_t, 8> control_1_{};
    uint16_t flipscreen_ = 0;
    bool supported_ = false;
    uint32_t unsupported_pc_ = 0;

    void load_default_profile(GameMemory &memory);
    void get_pf_scroll(int pf_num, int32_t &reg_sx, int32_t &reg_sy, bool flipped) const;
};

} // namespace f3rt
