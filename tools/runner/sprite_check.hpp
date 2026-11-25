#pragma once

#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

namespace f3rt::runner {

struct SpriteCheckArgs {
    std::filesystem::path rom_dir;
    std::string set = F3RT_DEFAULT_SET;
    uint64_t frames = 6000;
    uint64_t compare_every = 60;
    uint64_t seed = 12345;
    bool inputs = true;
    std::vector<std::string> enhancements;
};

struct SpriteCheckResult {
    bool passed = false;
    uint64_t frames = 0;
    uint64_t steps = 0;
    uint64_t native_blocks = 0;
    uint64_t state_compares = 0;
    uint64_t groups = 0;
    uint64_t group_frames = 0;
    uint64_t group_entries = 0;
    uint64_t unresolved_entries = 0;
    uint64_t max_groups_frame = 0;
    uint64_t presented_entries = 0;
    uint32_t state_crc = 0;
    std::string error;
};

// Runs the game with render-only sprite groups and presentation enhancements and checks that
// emulated state (steps, native block count, state CRC) is identical with groups off.
SpriteCheckResult run_sprite_check(const SpriteCheckArgs &args, std::ostream *out = nullptr);

} // namespace f3rt::runner
