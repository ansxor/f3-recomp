#include <assert.h>
#include "program.h"
#include <f3rt/block_profile.h>
uint64_t counts[3];
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
    f3_cpu cpu={0}; f3_generated_register(&cpu);
    for(size_t i=0;i<n;++i) {
        cpu.pc=blocks[i].address; exception=0;
        blocks[i].execute(&cpu);
        assert(exception==(cpu.pc==0 ? 10u : cpu.pc==2 ? 11u : 4u));
    }
    assert(counts[0]==1 && counts[1]==(SLIM ? 0u : 1u) && counts[2]==1);
    return 0;
}
