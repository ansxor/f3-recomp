#pragma once
#include "cpu_counters.hpp"
#include <array>
#include <atomic>
#include <cstdint>

namespace f3rt::n3ds {
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

struct PerfStats {
    static void smooth(std::atomic<uint64_t> &slot, uint64_t sample) {
        const uint64_t previous = slot.load(std::memory_order_relaxed);
        const int64_t value = previous ? int64_t(previous) + (int64_t(sample) - int64_t(previous)) / 16
                                       : int64_t(sample);
        slot.store(uint64_t(value), std::memory_order_relaxed);
    }
    std::atomic<uint64_t> emulated{0}, composed{0}, presented{0};
    std::atomic<uint64_t> emu_us{0}, compose_us{0}, compose_max_us{0}, present_us{0};
    std::atomic<uint64_t> emu_start_ns{0}, emu_end_ns{0}, emu_frame_us{0}, synth_frame_us{0};
    std::atomic<uint32_t> special_px{0};
    std::array<std::atomic<uint32_t>, std::tuple_size_v<f3rt::ChunkCounts>> chunks{};
    CpuStats composer_cpu;
    std::array<CpuStats, f3rt::VIDEO_STAGE_COUNT> emulation_cpu;
    std::atomic<bool> cpu_counters{false};
    std::atomic<uint32_t> audio_queue_frames{0};
    std::atomic<uint64_t> drops{0};
};
} // namespace f3rt::n3ds
