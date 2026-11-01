#include "settings.hpp"
#include "frontend/config_file.hpp"
#include "log.hpp"
#include <charconv>
#include <cstdio>
#include <sstream>

namespace f3rt::n3ds {
namespace {
bool decimal(std::string_view text, unsigned &value) {
    auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool hexadecimal(std::string_view text, uint32_t &value) {
    value = 0;
    auto result = std::from_chars(text.data(), text.data() + text.size(), value, 16);
    return result.ec == std::errc{} && result.ptr == text.data() + text.size();
}
bool boolean(std::string_view text, bool &value) {
    if (text != "on" && text != "off") return false;
    value = text == "on";
    return true;
}
} // namespace
Settings default_settings() {
    Settings settings;
    settings.keys = default_keys();
    return settings;
}
bool load_settings(const std::string &path, Settings &settings) {
    auto loaded = settings;
    std::string error;
    const bool read = f3rt::read_config(
        path,
        [&](std::string_view key, std::string_view value, std::string &reject) {
            if (key == "volume") {
                if (!decimal(value, loaded.volume) || loaded.volume > 100) {
                    reject = "Invalid volume";
                    return false;
                }
            } else if (key == "fast_boot") {
                if (!boolean(value, loaded.fast_boot)) {
                    reject = "Invalid fast_boot";
                    return false;
                }
            } else if (key == "boot_cache") {
                // Obsolete: a cached snapshot loses the HLE's in-flight voices. The key is still
                // accepted so an older settings file keeps loading.
                bool ignored = true;
                if (!boolean(value, ignored)) {
                    reject = "Invalid boot_cache";
                    return false;
                }
            } else if (key == "perf_overlay") {
                if (!boolean(value, loaded.perf_overlay)) {
                    reject = "Invalid perf_overlay";
                    return false;
                }
            } else if (key == "border") {
                if (!decimal(value, loaded.border) || (loaded.border != 0 && loaded.border != 40)) {
                    reject = "Invalid border";
                    return false;
                }
            } else if (key.size() > 3 && key.substr(0, 3) == "key") {
                unsigned control = 0;
                if (!decimal(key.substr(3), control) || control >= loaded.keys.size() ||
                    !hexadecimal(value, loaded.keys[control])) {
                    reject = "Invalid key binding";
                    return false;
                }
            } else {
                reject = "Unknown config setting: " + std::string(key);
                return false;
            }
            return true;
        },
        error);
    if (!read) {
        log_line("n3ds: settings %s rejected: %s", path.c_str(), error.c_str());
        return false;
    }
    settings = loaded;
    return true;
}
bool save_settings(const std::string &path, const Settings &settings) {
    std::ostringstream text;
    text << "# f3rt New 3DS settings\nvolume=" << settings.volume
         << "\nfast_boot=" << (settings.fast_boot ? "on" : "off")
         << "\nperf_overlay=" << (settings.perf_overlay ? "on" : "off")
         << "\nborder=" << settings.border << '\n';
    for (unsigned control = 0; control < settings.keys.size(); ++control) {
        char mask[16];
        std::snprintf(mask, sizeof mask, "%x", unsigned(settings.keys[control]));
        text << "key" << control << '=' << mask << '\n';
    }
    std::string error;
    if (!f3rt::write_config_atomic(path, text.str(), error)) {
        log_line("n3ds: cannot write settings %s: %s", path.c_str(), error.c_str());
        return false;
    }
    return true;
}
} // namespace f3rt::n3ds
