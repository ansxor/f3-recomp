#pragma once
#include <cstdarg>

namespace f3rt::n3ds {
// Opens the log file (truncated) and keeps stderr, which carries svcOutputDebugString.
void log_open(const char *path);
void log_line(const char *format, ...) __attribute__((format(printf, 1, 2)));
} // namespace f3rt::n3ds
