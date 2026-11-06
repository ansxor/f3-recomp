#include "cpu_counters.hpp"
#include "log.hpp"
#include <3ds.h>

namespace f3rt::n3ds {
namespace {
// CP15 performance counters: PMN0 counts retired instructions, PMN1 L1 data-cache read misses and
// CCNT cycles. The virtual-counter mode makes the kernel widen them to 64 bits.
bool control(PerfCounterOperation op, u32 param1 = 0, u64 param2 = 0, u64 *out = nullptr) {
    u64 value = 0;
    return R_SUCCEEDED(svcControlPerformanceCounter(out ? out : &value, op, param1, param2));
}
// "Enable and lock": the monitor is one process-wide resource that the kernel hands out once, so the
// first thread to open a CpuCounters takes it and later ones reuse it. Nothing ever disables it
// (a DISABLE forcibly unlocks it under the other threads).
bool monitor_enabled() {
    static const bool enabled =
        control(PERFCOUNTEROP_ENABLE) && control(PERFCOUNTEROP_SET_VIRTUAL_COUNTER_ENABLED, 1);
    return enabled;
}
uint64_t value_of(PerfCounterRegister reg) {
    u64 value = 0;
    return control(PERFCOUNTEROP_GET_VALUE, u32(reg), 0, &value) ? value : 0;
}
} // namespace

bool CpuCounters::open(const char *owner) {
    if (m_opened) return m_available;
    m_opened = true;
    m_available = monitor_enabled() &&
                  control(PERFCOUNTEROP_SET_EVENT, u32(PERFCOUNTERREG_CORE_COUNT_REG_0),
                          u64(PERFCOUNTEREVT_CORE_INST_EXECUTED)) &&
                  control(PERFCOUNTEROP_SET_EVENT, u32(PERFCOUNTERREG_CORE_COUNT_REG_1),
                          u64(PERFCOUNTEREVT_CORE_DATA_CACHE_READ_MISS)) &&
                  control(PERFCOUNTEROP_SET_EVENT, u32(PERFCOUNTERREG_CORE_CYCLE_COUNTER),
                          u64(PERFCOUNTEREVT_CORE_CYCLE_COUNT));
    log_line(m_available
                 ? "n3ds: cpu counters enabled for %s (insn, cycles, L1D read misses)"
                 : "n3ds: cpu counters unavailable for %s (ControlPerformanceCounter refused)",
             owner);
    return m_available;
}

CpuSample CpuCounters::read() const {
    if (!m_available) return {};
    return {value_of(PERFCOUNTERREG_CORE_COUNT_REG_0), value_of(PERFCOUNTERREG_CORE_CYCLE_COUNTER),
            value_of(PERFCOUNTERREG_CORE_COUNT_REG_1)};
}

void CpuCounters::begin() { m_origin = read(); }

void CpuCounters::end() { m_sample = read().since(m_origin); }

void StageCounters::begin_frame() {
    m_stages.fill({});
    m_stage = f3rt::VideoStage::Idle;
    m_mark = m_counters.read();
}

void StageCounters::end_frame() { charge(); }

void StageCounters::enter(f3rt::VideoStage stage) {
    charge();
    m_stage = stage;
}

CpuSample StageCounters::total() const {
    CpuSample sum;
    for (const auto &stage : m_stages) sum += stage;
    return sum;
}

void StageCounters::charge() {
    const CpuSample now = m_counters.read();
    m_stages[unsigned(m_stage)] += now.since(m_mark);
    m_mark = now;
}
} // namespace f3rt::n3ds
