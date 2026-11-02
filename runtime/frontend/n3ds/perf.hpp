#pragma once
#include <atomic>
#include <cstdint>

namespace f3rt::n3ds {
// Relaxed counters shared between the emulation, composer and presenter threads. Rates are derived
// by the presenter once per second; the *_us stages are exponentials, the *_max_us entries are
// window maxima the presenter clears after each rate report, and the indexed-compositor entries are
// per-frame values from the last composed frame.
struct PerfStats {
    std::atomic<uint64_t> emulated{0}, composed{0}, presented{0};
    std::atomic<uint64_t> emu_us{0}, compose_us{0}, compose_max_us{0}, present_us{0};
    // Indexed compositor: rows the canonical kernel decided, pixels that needed a real blend, and
    // the participants that pulled at least one row chunk.
    std::atomic<uint32_t> fallback_rows{0}, special_px{0}, chunks{0};
    // Composer-thread hardware counters (zero when the performance monitor is off or refused).
    std::atomic<uint64_t> cpu_insn{0}, cpu_cycles{0}, cpu_dmiss{0};
    // Written by the presenter from the Performance overlay setting, read by the composer.
    std::atomic<bool> cpu_counters{false};
    std::atomic<uint32_t> audio_queue_frames{0};
    std::atomic<uint64_t> drops{0};
};
} // namespace f3rt::n3ds
