#pragma once
#include <cstdint>

namespace f3rt {
// Host-only diagnostics; never part of machine, snapshot or netplay state.
// `component`, `kind` and `layer` must be string literals (compared by address).

// Prints the first occurrence of each (component, kind) FDP fallback reason.
void log_unsupported_video(const char *component, const char *kind, uint64_t frame);

#ifdef F3RT_VIDEO_WRITE_LOG
// Debug builds only (-DF3RT_VIDEO_WRITE_LOG=ON): prints the first video RAM
// store from each PC outside a component's known producer list, to find game
// routines that are not yet documented.
void log_unknown_video_write(const char *layer, uint32_t pc, uint32_t address, uint64_t frame);
#endif
} // namespace f3rt
