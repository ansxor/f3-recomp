#include "driver.h"
#include "instrument.h"

uint64_t counts[0x1000];
uint64_t *f3_profile_main_counts = counts;
uint64_t *f3_profile_sound_counts;
int main(void) {
    f3_cpu cpu = fresh_cpu();
    cpu.pc=0x400; cpu.dispatch_deadline=1;
    assert(dispatch(&cpu)); assert(cpu.pc==0x402 && cpu.d[0]==1 && !cpu.d[1]);
    assert(counts[0x400/2]==1 && !counts[0x402/2]);
    cpu.dispatch_deadline=UINT64_MAX;
#if SLIM
    assert(block_count==2 && !dispatch(&cpu));
#else
    while(cpu.pc!=0x406) assert(dispatch(&cpu));
    assert(cpu.d[1]==2 && cpu.d[2]==3);
    assert(counts[0x402/2]==1 && counts[0x404/2]==1);
#endif
    cpu.pc=0x404; cpu.d[2]=99; cpu.dispatch_deadline=cpu.steps+1;
    assert(dispatch(&cpu)); f3_cc_flush(&cpu); assert(cpu.pc==0x406 && cpu.d[2]==3 && !cpu.cc_op);
    assert(counts[0x404/2]==(SLIM ? 1u : 2u));
    counts[0x404/2]=UINT64_MAX; cpu.pc=0x404;
    assert(dispatch(&cpu)); assert(counts[0x404/2]==UINT64_MAX);
    return 0;
}
