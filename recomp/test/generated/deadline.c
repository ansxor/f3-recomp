#include "driver.h"

int main(void) {
    f3_cpu cpu = {0};
    assert(f3_generated_register(&cpu));
    cpu.pc = 0x400; cpu.sr = 0x10; cpu.d[1] = 0x12345678;
    cpu.dispatch_deadline = 1;
    assert(dispatch(&cpu));
    assert(cpu.pc == 0x402 && cpu.steps == 1);
    assert(cpu.d[0] == UINT32_MAX && cpu.d[1] == 0x12345678);
    f3_cc_flush(&cpu);
    assert(cpu.sr == 0x18 && cpu.cc_op == 0);

    cpu.dispatch_deadline = cpu.steps + 1;            /* MOVE CCR,D3 */
    assert(dispatch(&cpu));
    assert(cpu.pc == 0x404 && (cpu.d[3] & 0x1f) == 0x18);

    cpu.dispatch_deadline = cpu.steps + 1;            /* ADDQ.W #1,D0 */
    assert(dispatch(&cpu));
    assert(cpu.pc == 0x406 && cpu.d[0] == 0xffff0000);
    f3_cc_flush(&cpu);
    assert(cpu.sr == 0x15 && cpu.cc_op == 0);

    uint64_t resumed = cpu.steps;
    cpu.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&cpu));
    assert(cpu.pc == 0x40c && cpu.steps > resumed);
    assert(cpu.d[0] == 0xffff0000 && cpu.d[1] == 1);
    assert((cpu.d[4] & 0x1f) == 0x15 && (cpu.d[5] & 0x1f) == 0x10);
    f3_cc_flush(&cpu);
    assert(cpu.sr == 0x10 && cpu.cc_op == 0);
    return 0;
}
