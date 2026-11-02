#include "cpu_counters.hpp"
#include "log.hpp"
#include <3ds.h>

namespace f3rt::n3ds {
namespace {
// CP15 performance counters: PMN0 counts retired instructions, PMN1 L1 data-cache read misses and
// CCNT cycles. The virtual-counter mode makes the kernel widen them to 64 bits.
constexpr u32 REGISTERS = (1u << PERFCOUNTERREG_CORE_COUNT_REG_0) |
                          (1u << PERFCOUNTERREG_CORE_COUNT_REG_1) |
                          (1u << PERFCOUNTERREG_CORE_CYCLE_COUNTER);
bool control(PerfCounterOperation op, u32 param1 = 0, u64 param2 = 0, u64 *out = nullptr) {
    u64 value = 0;
    return R_SUCCEEDED(svcControlPerformanceCounter(out ? out : &value, op, param1, param2));
}
void write_value(u64 &slot, PerfCounterRegister reg) {
    u64 value = 0;
    slot = control(PERFCOUNTEROP_GET_VALUE, u32(reg), 0, &value) ? value : 0;
}
} // namespace

bool CpuCounters::open() {
    if (m_opened) return m_available;
    m_opened = true;
    bool ok = control(PERFCOUNTEROP_ENABLE) &&
              control(PERFCOUNTEROP_SET_VIRTUAL_COUNTER_ENABLED, 1) &&
              control(PERFCOUNTEROP_SET_EVENT, u32(PERFCOUNTERREG_CORE_COUNT_REG_0),
                      u64(PERFCOUNTEREVT_CORE_INST_EXECUTED)) &&
              control(PERFCOUNTEROP_SET_EVENT, u32(PERFCOUNTERREG_CORE_COUNT_REG_1),
                      u64(PERFCOUNTEREVT_CORE_DATA_CACHE_READ_MISS)) &&
              control(PERFCOUNTEROP_SET_EVENT, u32(PERFCOUNTERREG_CORE_CYCLE_COUNTER),
                      u64(PERFCOUNTEREVT_CORE_CYCLE_COUNT));
    if (!ok) {
        control(PERFCOUNTEROP_DISABLE);
        if (!m_reported)
            log_line("n3ds: cpu counters unavailable (ControlPerformanceCounter refused)");
    } else {
        log_line("n3ds: cpu counters enabled (insn, cycles, L1D read misses)");
    }
    m_reported = true;
    m_available = ok;
    return m_available;
}

void CpuCounters::close() {
    if (!m_available) return;
    control(PERFCOUNTEROP_DISABLE);
    m_available = false;
}

void CpuCounters::begin() {
    if (!m_available) return;
    control(PERFCOUNTEROP_RESET, REGISTERS, REGISTERS);
}

void CpuCounters::end() {
    if (!m_available) return;
    Sample sample;
    write_value(sample.instructions, PERFCOUNTERREG_CORE_COUNT_REG_0);
    write_value(sample.data_misses, PERFCOUNTERREG_CORE_COUNT_REG_1);
    write_value(sample.cycles, PERFCOUNTERREG_CORE_CYCLE_COUNTER);
    m_sample = sample;
}
} // namespace f3rt::n3ds
