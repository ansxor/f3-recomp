#pragma once
#include "input.hpp"
#include <cstdint>
#include <string>

namespace f3rt::n3ds {
struct Settings {
    unsigned volume = 100;
    bool fast_boot = true, perf_overlay = false;
    unsigned border = 0; // 0 = 320x232 pixel-exact, 40 = the Enhanced 400x232 widescreen border
    ControlKeys keys{};
};
Settings default_settings();
// Missing file keeps the defaults; invalid values are rejected as a whole and logged.
bool load_settings(const std::string &path, Settings &settings);
bool save_settings(const std::string &path, const Settings &settings);
} // namespace f3rt::n3ds
