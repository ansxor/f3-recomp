#pragma once
#include "f3rt/game_video.hpp"
#include "renderer/game/row_workers.hpp"
#include <array>
#include <cstdint>

namespace f3rt::n3ds {
// ARM11 performance-monitor readings of one core: instructions retired, core cycles and L1
// data-cache read misses.
struct CpuSample {
    uint64_t instructions = 0, cycles = 0, data_misses = 0;

    // Counts accumulated since `earlier`. The hardware counters are 32 bits wide, so the difference
    // is taken modulo 2^32: exact for any span shorter than one counter period (seconds).
    CpuSample since(const CpuSample &earlier) const {
        const auto delta = [](uint64_t now, uint64_t before) {
            return uint64_t(uint32_t(now - before));
        };
        return {delta(instructions, earlier.instructions), delta(cycles, earlier.cycles),
                delta(data_misses, earlier.data_misses)};
    }
    CpuSample &operator+=(const CpuSample &other) {
        instructions += other.instructions;
        cycles += other.cycles;
        data_misses += other.data_misses;
        return *this;
    }
};

// The performance monitor of the calling thread's core. Requires the ControlPerformanceCounter
// permission (cmake/n3ds/app.rsf.in). When the kernel refuses it, open() reports unavailable and
// every reading is zero, so the code under observation runs unchanged without the overlay.
// Each observed thread owns one instance and opens it on that thread; the counters run free, so
// windows nest and never disturb each other.
class CpuCounters final : public f3rt::WorkWindow {
public:
    // Enables the process-wide monitor (once) and programs this core's three events. Call once,
    // on the thread to observe; later calls return the first outcome. `owner` names the thread in
    // the log.
    bool open(const char *owner);
    bool available() const { return m_available; }
    // The free-running counters now (zero when unavailable).
    CpuSample read() const;
    // WorkWindow: observe one span; `begin()` marks its start, `end()` latches its counts.
    void begin() override;
    void end() override;
    // The most recent latched span (zero until the first end()).
    CpuSample sample() const { return m_sample; }

private:
    bool m_available = false, m_opened = false;
    CpuSample m_origin{}, m_sample{};
};

// Attributes one thread's counters to the GameVideo stages of a frame. begin_frame() starts a frame
// in VideoStage::Idle; each enter() charges everything since the previous call to the stage just
// left; end_frame() charges the remainder. stages() then holds the frame's per-stage counts, with
// the work outside render_frame (the guest CPU, audio hand-off) in Idle.
class StageCounters final : public f3rt::VideoProbe {
public:
    using Stages = std::array<CpuSample, f3rt::VIDEO_STAGE_COUNT>;

    explicit StageCounters(const CpuCounters &counters) : m_counters(counters) {}
    void begin_frame();
    void end_frame();
    void enter(f3rt::VideoStage stage) override;
    const Stages &stages() const { return m_stages; }
    // Every stage of the frame together.
    CpuSample total() const;

private:
    void charge();
    const CpuCounters &m_counters;
    Stages m_stages{};
    CpuSample m_mark{};
    f3rt::VideoStage m_stage = f3rt::VideoStage::Idle;
};
} // namespace f3rt::n3ds
