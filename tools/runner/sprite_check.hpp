#pragma once

#include <cstdint>
#include <filesystem>
#include <ostream>
#include <string>
#include <vector>

namespace f3rt::runner {

struct SpriteCheckArgs {
    std::filesystem::path rom_dir;
    std::string set = "commandw";
    uint64_t frames = 6000;
    uint64_t compare_every = 60;
    uint64_t seed = 12345;
    bool inputs = true;
    bool flicker = true;
    uint64_t shadow_trace = 0;
    std::vector<std::string> behaviours;
};

struct SpriteCheckResult {
    bool passed = false;
    uint64_t frames = 0;
    uint64_t cycles = 0;
    uint64_t native_blocks = 0;
    uint64_t state_compares = 0;
    uint64_t invocations = 0;
    uint64_t unaccounted_writes = 0;
    uint64_t mismatches = 0;
    uint64_t aborted = 0;
    uint64_t violations = 0;
    std::string first_violation;
    std::string error;
};

SpriteCheckResult run_sprite_check(const SpriteCheckArgs &args, std::ostream *out = nullptr);

} // namespace f3rt::runner
