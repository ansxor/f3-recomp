#pragma once
#include "game_scene.hpp"
#include "f3rt/cpu_abi.h"
#include "f3rt/game_video.hpp"
#include <array>
#include <cstdint>
#include <span>

namespace f3rt {

class GameSprites {
public:
    GameSprites() { reset(); }

    void reset();
    void observe(GameMemory &memory, const f3_cpu &cpu);
    void latch();
    void raster(std::span<const uint8_t> assets, std::span<uint16_t> output,
                GameVideoOptions options = {}) const;
    std::span<const SceneSprite> sprites() const;
    bool flipped() const;
    uint8_t pen_mask() const;
    bool trails() const;
    bool supported() const;
    uint32_t unsupported_pc() const;
    void observe_write(uint32_t pc, uint32_t address);

private:
    static constexpr size_t kMaxSprites = 1024;

    std::array<SceneSprite, kMaxSprites> staging_sprites_{};
    size_t staging_count_ = 0;

    std::array<SceneSprite, kMaxSprites> submitted_sprites_{};
    size_t submitted_count_ = 0;

    std::array<SceneSprite, kMaxSprites> current_sprites_{};
    size_t current_count_ = 0;
    bool current_flipped_ = false;
    uint8_t current_pen_mask_ = 15;
    bool current_trails_ = false;

    int16_t reg_scroll_x_ = 0;
    int16_t reg_scroll_y_ = 0;
    bool reg_flipped_ = false;
    uint8_t reg_pen_mask_ = 15;
    bool reg_trails_ = false;

    bool supported_ = false;
    uint32_t unsupported_pc_ = 0;

    void parse_single(GameMemory &memory, const f3_cpu &cpu);
    void parse_grid(GameMemory &memory, const f3_cpu &cpu);
    void parse_obj_grid(GameMemory &memory, const f3_cpu &cpu);
    void parse_obj_3tile(GameMemory &memory, const f3_cpu &cpu);
    void parse_obj_4tile(GameMemory &memory, const f3_cpu &cpu);
    void parse_obj_scaled(GameMemory &memory, const f3_cpu &cpu);
    void submit(GameMemory &memory, const f3_cpu &cpu);

    void emit_sprite(uint32_t tile, int32_t x_24_8, int32_t y_24_8,
                     uint16_t scale_x, uint16_t scale_y,
                     uint8_t palette, bool flip_x, bool flip_y,
                     uint32_t caller_pc);

    static bool is_covered_write(uint32_t pc);
};

} // namespace f3rt
