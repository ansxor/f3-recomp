#include "driver.h"

int main(void) {
    f3_cpu probe = fresh_cpu(), yielding = fresh_cpu(), resumed = fresh_cpu();
    probe.pc = 0x400; probe.dispatch_deadline = 0;
    uint32_t stop_pc = 0; uint64_t stop_steps = 0;
    while (probe.pc != 0x40a) {
        assert(dispatch(&probe));                  /* one instruction per call */
        if (probe.steps >= 5) { stop_pc = probe.pc; stop_steps = probe.steps; break; }
    }
    assert(stop_pc != 0);
    yielding.pc = 0x400; yielding.dispatch_deadline = 5;
    assert(dispatch(&yielding));                    /* chains until the deadline */
    assert(yielding.pc == stop_pc && yielding.steps == stop_steps);
    f3_cc_flush(&yielding);
    assert(yielding.cc_op == 0);                    /* flags materialized at flush */
    yielding.dispatch_deadline = UINT64_MAX;
    while (yielding.pc != 0x40a) assert(dispatch(&yielding));
    resumed.pc = 0x400; resumed.dispatch_deadline = 0;
    while (resumed.pc != 0x40a) assert(dispatch(&resumed));
    assert(yielding.d[0] == resumed.d[0] && yielding.d[1] == resumed.d[1]);
    assert(yielding.steps == resumed.steps);
    return 0;
}
