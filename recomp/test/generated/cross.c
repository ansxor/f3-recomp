#include "driver.h"

int main(void) {
    f3_cpu cpu = fresh_cpu();
    cpu.pc = 0x400; cpu.a[7] = 0x10000; cpu.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&cpu));
    assert(cpu.pc == (HAS_MUSTTAIL ? 0x504 : 0x500) && cpu.d[1] == (HAS_MUSTTAIL ? 7 : 0) && cpu.a[7] == 0xfffcu);

    f3_cpu traced = fresh_cpu();
    traced.pc = 0x400; traced.sr = 0x8000; traced.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&traced));
    assert(traced.pc == 0x500 && traced.d[1] == 0);

    f3_cpu jumped = fresh_cpu();
    jumped.pc = 0x600; jumped.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&jumped));
    assert(jumped.pc == (HAS_MUSTTAIL ? 0x504 : 0x500) && jumped.d[1] == (HAS_MUSTTAIL ? 7 : 0));
    return 0;
}
