#pragma once
#include <cstdint>

namespace f3rt {
// Host-only diagnostics; never part of machine, snapshot or netplay state.
// `component`, `kind` and `layer` must be string literals (compared by address).

// Prints the first occurrence of each (component, kind) FDP fallback reason.
void log_unsupported_video(const char *component, const char *kind, uint64_t frame);
} // namespace f3rt
