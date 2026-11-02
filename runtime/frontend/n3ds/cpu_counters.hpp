#pragma once
#include "renderer/game/row_workers.hpp"
#include <cstdint>

namespace f3rt::n3ds {
// ARM11 performance-monitor samples for the composer thread: instructions retired, core cycles and
// L1 data-cache read misses. Requires the ControlPerformanceCounter permission
// (cmake/n3ds/app.rsf.in). When the kernel refuses it, open() reports unavailable once and the
// window methods become no-ops, so the compositor runs unchanged without the overlay.
class CpuCounters final : public f3rt::WorkWindow {
public:
    struct Sample {
        uint64_t instructions = 0, cycles = 0, data_misses = 0;
    };

    // Configures the three events and enables the monitor. Safe to call once per composer.
    bool open();
    void close();
    bool available() const { return m_available; }
    // Program the counters for one observed span; `begin()` resets, `end()` latches the sample.
    void begin() override;
    void end() override;
    // The most recent latched span (zero until the first end()).
    Sample sample() const { return m_sample; }

private:
    bool m_available = false, m_opened = false, m_reported = false;
    Sample m_sample{};
};
} // namespace f3rt::n3ds
