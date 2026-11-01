#include "log.hpp"
#include <cstdarg>
#include <cstdio>
#if defined(__3DS__)
#include <3ds.h>
#endif

namespace f3rt::n3ds {
namespace {
std::FILE *g_file = nullptr;
#if defined(__3DS__)
// devkitARM's newlib was built without stdio thread safety, so concurrent writes to one stream
// interleave inside its buffer. Every thread logs, so the streams are serialized here.
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
    std::va_list args;
    va_start(args, format);
    std::vfprintf(stderr, format, args);
    va_end(args);
    std::fputc('\n', stderr);
    std::fflush(stderr);
    if (!g_file) return;
    va_start(args, format);
    std::vfprintf(g_file, format, args);
    va_end(args);
    std::fputc('\n', g_file);
    std::fflush(g_file);
}
} // namespace f3rt::n3ds
