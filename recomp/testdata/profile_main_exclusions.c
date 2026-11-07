#include <assert.h>
#include "program.h"
#include <f3rt/block_profile.h>
uint64_t counts[6];
uint64_t *f3_profile_main_counts=counts;
uint64_t *f3_profile_sound_counts;
static const f3_block *blocks;
static size_t n;
static unsigned exception;
int f3_register_blocks(f3_cpu *cpu, const f3_block *table, size_t count) {
    (void)cpu; blocks=table; n=count; return 1;
}
void f3_exception(f3_cpu *cpu, unsigned vector, uint32_t pc) {
    assert(pc==cpu->pc); exception=vector;
}
int main(void) {
    f3_cpu cpu={0}; assert(f3_generated_register(&cpu));
    assert(registered_exclusion_count==1);
    assert(registered_exclusions[0].start==4 && registered_exclusions[0].end==6);
    const uint32_t expected[]={0, SLIM ? 6u : 2u, 6, 8, 10};
    assert(n==(SLIM ? 2u : 5u));
    for(size_t i=0;i<n;++i) {
        assert(blocks[i].address==expected[i]);
        /* Deadline already reached: each entry yields after one instruction,
           so block chaining (covered by tools/test_generate.py) stays out. */
        cpu.pc=blocks[i].address; cpu.dispatch_deadline=cpu.cycles; exception=0;
        blocks[i].execute(&cpu);
        if(expected[i]>=6) assert(exception==(expected[i]==10 ? 11u : 10u));
        else assert(cpu.pc==expected[i]+2);
    }
    assert(counts[0]==1 && counts[3]==1 && !counts[2]);
    assert(counts[1]==(SLIM ? 0u : 1u));
    assert(counts[4]==(SLIM ? 0u : 1u) && counts[5]==(SLIM ? 0u : 1u));
    return 0;
}
