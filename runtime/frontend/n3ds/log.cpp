#include "log.hpp"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#if defined(__3DS__)
#include <3ds.h>
#endif

namespace f3rt::n3ds {
namespace {
std::FILE *g_file = nullptr;
#if defined(__3DS__)
LightLock g_log_lock = {0};
struct LogLock {
    LogLock() { LightLock_Lock(&g_log_lock); }
    ~LogLock() { LightLock_Unlock(&g_log_lock); }
};
#endif
} // namespace
void log_open(const char *path) {
#if defined(__3DS__)
    const LogLock lock;
#endif
    if (g_file) std::fclose(g_file);
    g_file = path && *path ? std::fopen(path, "w") : nullptr;
}
void log_line(const char *format, ...) {
#if defined(__3DS__)
    const LogLock lock;
#endif
    char line[512];
    std::va_list args;
    va_start(args, format);
    const int length = std::vsnprintf(line, sizeof line - 1, format, args);
    va_end(args);
    if (length < 0) return;
    const size_t size = std::min(size_t(length), sizeof line - 2);
    line[size] = '\n';
    std::fwrite(line, 1, size + 1, stderr);
    std::fflush(stderr);
    if (!g_file) return;
    std::fwrite(line, 1, size + 1, g_file);
    std::fflush(g_file);
}
} // namespace f3rt::n3ds
