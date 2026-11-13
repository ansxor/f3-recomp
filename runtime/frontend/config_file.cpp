#include "frontend/config_file.hpp"
#include <chrono>
#include <fstream>

namespace f3rt {
bool read_config(const std::filesystem::path &path,
                 const std::function<bool(std::string_view key, std::string_view value,
                                          std::string &error)> &apply,
                 std::string &error) {
    error.clear();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) error = ec.message();
        return !ec;
    }
    if (std::filesystem::file_size(path, ec) > 32768 || ec) {
        error = "Config exceeds 32 KiB or is unreadable";
        return false;
    }
    std::ifstream file(path);
    if (!file) {
        error = "Cannot read config";
        return false;
    }
    std::string line;
    unsigned count = 0;
    while (std::getline(file, line)) {
        if (++count > 512 || line.size() > 1024) {
            error = "Config limits exceeded";
            return false;
        }
        if (line.empty() || line[0] == '#') continue;
        const auto split = line.find('=');
        if (split == std::string::npos) {
            error = "Invalid config line";
            return false;
        }
        const std::string_view text(line);
        if (!apply(text.substr(0, split), text.substr(split + 1), error)) return false;
    }
    if (file.bad()) {
        error = "Invalid config values";
        return false;
    }
    return true;
}
bool write_config_atomic(const std::filesystem::path &path, std::string_view text,
                         std::string &error) {
    error.clear();
    if (path.empty()) {
        error = "Invalid config path";
        return false;
    }
    std::error_code ec;
    if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    // Same-directory temporary file keeps replacement atomic.
    std::string tmp = path.string() + ".tmp." +
                      std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Cannot create config temporary file";
        return false;
    }
    out << text;
    out.close();
    if (!out) {
        error = "Cannot write config";
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, path, ec);
    if (ec) {
        error = ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}
} // namespace f3rt
