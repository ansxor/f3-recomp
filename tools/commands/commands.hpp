#pragma once

#include <cstdint>
#include <cstdio>
#include <iostream>
#include <string>
#include <string_view>

namespace f3rt::tool {

// First frame at which comparing the game renderer with the FDP oracle is meaningful (earlier frames are boot).
inline constexpr uint64_t VIDEO_DIFF_MIN_FRAME = 600;

int cmd_gameplay(int argc, char **argv);
int cmd_motion(int argc, char **argv);
int cmd_sprite_check(int argc, char **argv);
int cmd_netplay(int argc, char **argv);
int cmd_netplay_replay(int argc, char **argv);
int cmd_mcp(int argc, char **argv);

inline std::string json_escape(const std::string &s) {
    std::string out;
    for (const unsigned char c : s) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += char(c);
            }
        }
    }
    return out;
}

inline bool has_flag(int argc, char **argv, std::string_view flag) {
    for (int i = 0; i < argc; ++i)
        if (flag == argv[i]) return true;
    return false;
}

// Failure result for --json runs: one JSON object on stdout, details on stderr as usual.
inline void print_json_error(const char *status, const std::string &message) {
    std::cout << "{\"status\":\"" << status << "\",\"error\":\"" << json_escape(message)
              << "\"}\n";
}

} // namespace f3rt::tool
