#pragma once
#include "cpu_counters.hpp"
#include <array>
#include <atomic>
#include <cstdint>

namespace f3rt::n3ds {
// Relaxed mirror of a CpuSample: the observed thread stores it, the presenter reads it.
struct CpuStats {
    std::atomic<uint64_t> instructions{0}, cycles{0}, data_misses{0};

    void store(const CpuSample &sample) {
        instructions.store(sample.instructions, std::memory_order_relaxed);
        cycles.store(sample.cycles, std::memory_order_relaxed);
        data_misses.store(sample.data_misses, std::memory_order_relaxed);
    }
    CpuSample load() const {
        return {instructions.load(std::memory_order_relaxed),
                cycles.load(std::memory_order_relaxed),
                data_misses.load(std::memory_order_relaxed)};
    }
};

// Relaxed counters shared between the emulation, composer and presenter threads. Rates are derived
// by the presenter once per second; the *_us stages are exponentials, the *_max_us entries are
// window maxima the presenter clears after each rate report, and the indexed-compositor entries are
// per-frame values from the last composed frame.
struct PerfStats {
    std::atomic<uint64_t> emulated{0}, composed{0}, presented{0};
    std::atomic<uint64_t> emu_us{0}, compose_us{0}, compose_max_us{0}, present_us{0};
    // Indexed compositor: rows the canonical kernel decided, pixels that needed a real blend, and
    // the row chunks each participant of the last frame pulled (the composer, then each helper).
    std::atomic<uint32_t> fallback_rows{0}, special_px{0};
    std::array<std::atomic<uint32_t>, std::tuple_size_v<f3rt::ChunkCounts>> chunks{};
    // Hardware counters of the last observed frame (zero when the performance monitor is off or
    // refused): the composer's own compose span, and the emulation thread's frame split by GameVideo
    // stage (Idle is everything else in Machine::run_frame: the guest CPU above all).
    CpuStats composer_cpu;
    std::array<CpuStats, f3rt::VIDEO_STAGE_COUNT> emulation_cpu;
    // Written by the presenter from the Performance overlay setting, read by the composer and the
    // emulation thread.
    std::atomic<bool> cpu_counters{false};
    std::atomic<uint32_t> audio_queue_frames{0};
    std::atomic<uint64_t> drops{0};
};
} // namespace f3rt::n3ds
