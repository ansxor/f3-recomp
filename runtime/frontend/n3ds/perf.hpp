#pragma once
#include <atomic>
#include <cstdint>

namespace f3rt::n3ds {
// Relaxed counters shared between the emulation, composer and presenter threads. Rates are derived
// by the presenter once per second; emu_us/compose_us are per-stage exponentials in microseconds.
struct PerfStats {
    std::atomic<uint64_t> emulated{0}, composed{0}, presented{0};
    std::atomic<uint64_t> emu_us{0}, compose_us{0}, raster_us{0}, scene_us{0}, present_us{0};
    std::atomic<uint32_t> audio_queue_frames{0};
    std::atomic<uint64_t> drops{0};
};
} // namespace f3rt::n3ds
