#include "driver.h"

int main(void) {
    f3_cpu cpu = {0};
    assert(f3_generated_register(&cpu));
    cpu.pc = 0x400; cpu.d[1] = 99; cpu.d[2] = 98;
    cpu.dispatch_deadline = 1;
    assert(dispatch(&cpu));
    assert(cpu.pc == 0x406 && cpu.d[0] == 0x70017202);
    f3_cc_flush(&cpu);
    assert(cpu.d[1] == 99 && cpu.d[2] == 98 && cpu.cc_op == 0);
    cpu.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&cpu));
    assert(cpu.pc == 0x40a);
    assert(cpu.d[1] == 99 && cpu.d[2] == 3);
    cpu.pc = 0x402; cpu.d[0] = 0; cpu.d[1] = 99;
    cpu.dispatch_deadline = cpu.steps + 1;
    assert(dispatch(&cpu));
    assert(cpu.pc == 0x404 && cpu.d[0] == 1 && cpu.d[1] == 99);
    f3_cc_flush(&cpu);
    assert(cpu.cc_op == 0 && (cpu.sr & 15) == 0);
    cpu.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&cpu));
    assert(cpu.pc == 0x40a);
    assert(cpu.d[0] == 1 && cpu.d[1] == 2 && cpu.d[2] == 3);
    return 0;
}
