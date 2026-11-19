#include "driver.h"
#include "instrument.h"

static unsigned events;
static uint64_t counts[0x1000];
uint64_t *f3_profile_main_counts = counts;
uint64_t *f3_profile_sound_counts;

enum { ENTER_FIRST = 0x402, SHARED_HOOK = 0x406, EXIT_SECOND = 0x408 };

void f3_unit_enter(f3_cpu *cpu, uint32_t unit) {
    assert(!cpu->cc_op);
    assert(!counts[cpu->pc / 2]);
    if (events == 0) {
        assert(unit == 0 && cpu->pc == ENTER_FIRST);
    } else {
        assert(events == 2 && unit == 1 && cpu->pc == SHARED_HOOK);
    }
    ++events;
}

int f3_unit_exit(f3_cpu *cpu, uint32_t unit) {
    assert(!cpu->cc_op);
    assert(!counts[cpu->pc / 2]);
    if (events == 1) {
        assert(unit == 0 && cpu->pc == SHARED_HOOK);
    } else {
        assert(events == 3 && unit == 1 && cpu->pc == EXIT_SECOND);
    }
    ++events;
    return 0;
}

int main(void) {
    f3_cpu cpu = fresh_cpu();
    cpu.pc = 0x400;
    cpu.dispatch_deadline = UINT64_MAX;
    assert(dispatch(&cpu));
    assert(cpu.pc == 0x40a);
    assert(events == (HAS_UNITS ? 4 : 0));
    assert(counts[ENTER_FIRST / 2] == 1 && counts[SHARED_HOOK / 2] == 1);
    return 0;
}
