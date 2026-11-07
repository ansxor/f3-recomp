#include <assert.h>
#include "program.h"
#include <f3rt/block_profile.h>
uint64_t counts[0x1000];
uint64_t *f3_profile_main_counts = counts;
uint64_t *f3_profile_sound_counts;
static const f3_block *blocks;
static size_t n;
int f3_register_blocks(f3_cpu *cpu, const f3_block *table, size_t count) {
    (void)cpu; blocks=table; n=count; return 1;
}
static int dispatch(f3_cpu *cpu) {
    for(size_t i=0;i<n;++i) if(blocks[i].address==cpu->pc) { blocks[i].execute(cpu); return 1; }
    return 0;
}
int main(void) {
    f3_cpu cpu={0}; f3_generated_register(&cpu);
    cpu.pc=0x400; cpu.dispatch_deadline=1;
    assert(dispatch(&cpu)); assert(cpu.pc==0x402 && cpu.d[0]==1 && !cpu.d[1]);
    assert(counts[0x400/2]==1 && !counts[0x402/2]);
    cpu.dispatch_deadline=UINT64_MAX;
#if SLIM
    assert(n==2 && !dispatch(&cpu));
#else
    while(cpu.pc!=0x406) assert(dispatch(&cpu));
    assert(cpu.d[1]==2 && cpu.d[2]==3);
    assert(counts[0x402/2]==1 && counts[0x404/2]==1);
#endif
    cpu.pc=0x404; cpu.d[2]=99; cpu.dispatch_deadline=cpu.cycles+1;
    assert(dispatch(&cpu)); f3_cc_flush(&cpu); assert(cpu.pc==0x406 && cpu.d[2]==3 && !cpu.cc_op);
    assert(counts[0x404/2]==(SLIM ? 1u : 2u));
    counts[0x404/2]=UINT64_MAX; cpu.pc=0x404;
    assert(dispatch(&cpu)); assert(counts[0x404/2]==UINT64_MAX);
    return 0;
}
