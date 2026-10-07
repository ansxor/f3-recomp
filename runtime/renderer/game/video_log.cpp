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

#ifdef F3RT_VIDEO_WRITE_LOG
void log_unknown_video_write(const char *layer, uint32_t pc, uint32_t address, uint64_t frame) {
    static std::set<std::tuple<const char *, uint32_t>> seen;
    const std::lock_guard<std::mutex> lock(log_mutex);
    if (!seen.emplace(layer, pc).second) return;
    std::fprintf(stderr, "game-video: unknown %s write pc=0x%06x address=0x%06x frame=%llu\n",
                 layer, unsigned(pc), unsigned(address), static_cast<unsigned long long>(frame));
}
#endif
} // namespace f3rt
