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
    std::vector<std::string> behaviours;
};

struct SpriteCheckResult {
    bool passed = false;
    uint64_t frames = 0;
    uint64_t cycles = 0;
    uint64_t native_blocks = 0;
    uint64_t state_compares = 0;
    uint64_t groups = 0;              // distinct groups presented in the last frame
    uint64_t group_frames = 0;        // frames with at least one group
    uint64_t group_entries = 0;       // decoded entries carrying a group, summed over frames
    uint64_t unresolved_entries = 0;  // decoded entries without a group, summed over frames
    uint64_t max_groups_frame = 0;
    uint64_t invocations = 0;   // behaviour-anchor unit invocations
    uint64_t spliced = 0;       // behaviour splices produced
    uint64_t aborted = 0;
    std::string error;
};

// Runs the game with render-only sprite groups and checks that emulated state (cycles,
// native block count, state CRC) is identical with groups off; reports group coverage.
SpriteCheckResult run_sprite_check(const SpriteCheckArgs &args, std::ostream *out = nullptr);

} // namespace f3rt::runner
