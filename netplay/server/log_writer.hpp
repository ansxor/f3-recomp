#pragma once

#include "state.hpp"
#include <filesystem>
#include <string>

namespace f3rt::netplay {

class LogWriter {
public:
    explicit LogWriter(std::filesystem::path log_dir = "./netplay-logs");

    // Serializes snapshot to JSON string
    static std::string serialize_snapshot_json(const SessionSnapshot &snap);

    // Atomically writes log file to <log_dir>/<session_id>.json via .tmp rename
    bool write_log(const SessionSnapshot &snap) const;

    const std::filesystem::path &log_dir() const { return log_dir_; }

private:
    std::filesystem::path log_dir_;
};

} // namespace f3rt::netplay
