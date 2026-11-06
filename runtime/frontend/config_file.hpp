#pragma once
#include <filesystem>
#include <functional>
#include <string>
#include <string_view>

namespace f3rt {
bool read_config(const std::filesystem::path &path,
                 const std::function<bool(std::string_view key, std::string_view value,
                                          std::string &error)> &apply,
                 std::string &error);
bool write_config_atomic(const std::filesystem::path &path, std::string_view text,
                         std::string &error);
} // namespace f3rt
