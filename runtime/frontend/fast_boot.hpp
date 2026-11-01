#pragma once
#include "f3rt/game_video.hpp"
#include <cstdint>
#include <filesystem>
#include <functional>
#include <span>

namespace f3rt {
class Machine;
struct GameBoot;

// Host-side boot shortcut: run the power-on frames unthrottled with no presentation and discarded
// audio, from a locally generated initialised EEPROM and optionally a cached post-boot state. The
// emulated machine executes the same frames a user would watch.
struct FastBootRequest {
    std::filesystem::path eeprom_init; // empty: no initialised EEPROM is loaded or generated
    std::filesystem::path boot_cache;  // empty: no cached state is loaded or written
    GameVideoOptions snapshot{};       // geometry the cache was saved with
    bool translated = false;
    uint64_t frame_budget = 0; // total frames to execute; 0 = unlimited
    std::function<void(std::span<const int16_t>)> audio; // discarded audio, counted by the caller
    std::function<bool()> cancelled;                     // polled every 30 frames; true stops the boot
};
struct FastBootResult {
    uint64_t frames = 0; // frames the turbo boot executed
    double ms = 0;
    bool cache_loaded = false, complete = false;
    const char *source = "turbo";
    const char *eeprom = "none";
};
FastBootResult run_fast_boot(Machine &machine, const GameBoot &boot,
                             const FastBootRequest &request);
} // namespace f3rt
