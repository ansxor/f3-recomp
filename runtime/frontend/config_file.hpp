#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace f3rt {
// Line-oriented "key=value" settings file: at most 32 KiB, 512 lines and 1024 characters per line,
// '#' comments. A missing file is not an error. `apply` returns false with `error` set to reject a
// line, and the file is then left unread past that line.
bool read_config(const std::filesystem::path &path,
                 const std::function<bool(std::string_view key, std::string_view value,
                                          std::string &error)> &apply,
                 std::string &error);
// Writes `text` through a same-directory temporary file and an atomic rename.
bool write_config_atomic(const std::filesystem::path &path, std::string_view text,
                         std::string &error);
} // namespace f3rt
