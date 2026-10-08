#include "renderer/game/video_log.hpp"
#include <cstdio>
#include <mutex>
#include <set>
#include <tuple>

namespace f3rt {
namespace {
std::mutex log_mutex;
}

void log_unsupported_video(const char *component, const char *kind, uint64_t frame) {
    static std::set<std::tuple<const char *, const char *>> seen;
    const std::lock_guard<std::mutex> lock(log_mutex);
    if (!seen.emplace(component, kind).second) return;
    std::fprintf(stderr, "game-video: unsupported %s %s frame=%llu\n",
                 component, kind, static_cast<unsigned long long>(frame));
}
} // namespace f3rt
