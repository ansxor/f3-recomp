#pragma once
#include "f3rt/game_video.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>

namespace f3rt {
class Machine;
struct GameBoot;

struct FastBootRequest {
    std::filesystem::path eeprom_init;
    std::filesystem::path boot_cache;
    GameVideoOptions snapshot{};
    bool translated = false;
    uint64_t frame_budget = 0;
    std::function<void(std::span<const int16_t>)> audio;
    std::function<bool()> cancelled;
};
struct FastBootResult {
    uint64_t frames = 0;
    double ms = 0;
    bool cache_loaded = false, complete = false;
    const char *source = "turbo";
    const char *eeprom = "none";
};
FastBootResult run_fast_boot(Machine &machine, const GameBoot &boot,
                             const FastBootRequest &request);
} // namespace f3rt
