#include "driver.h"
#include "instrument.h"

uint64_t counts[6];
uint64_t *f3_profile_main_counts=counts;
uint64_t *f3_profile_sound_counts;
static unsigned exception;
void f3_exception(f3_cpu *cpu, unsigned vector, uint32_t pc) {
    assert(pc==cpu->pc); exception=vector;
}
int main(void) {
    f3_cpu cpu={0}; assert(f3_generated_register(&cpu));
    assert(registered_exclusion_count==1);
    assert(registered_exclusions[0].start==4 && registered_exclusions[0].end==6);
    const uint32_t expected[]={0, SLIM ? 6u : 2u, 6, 8, 10};
    assert(block_count==(SLIM ? 2u : 5u));
    for(size_t i=0;i<block_count;++i) {
        assert(blocks[i].address==expected[i]);
        /* Deadline already reached: each entry yields after one instruction,
           so block chaining (covered by tools/test_generate.py) stays out. */
        cpu.pc=blocks[i].address; cpu.dispatch_deadline=cpu.steps; exception=0;
        blocks[i].execute(&cpu);
        if(expected[i]>=6) assert(exception==(expected[i]==10 ? 11u : 10u));
        else assert(cpu.pc==expected[i]+2);
    }
    assert(counts[0]==1 && counts[3]==1 && !counts[2]);
    assert(counts[1]==(SLIM ? 0u : 1u));
    assert(counts[4]==(SLIM ? 0u : 1u) && counts[5]==(SLIM ? 0u : 1u));
    return 0;
}
