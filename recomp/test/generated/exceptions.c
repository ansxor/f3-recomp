#include "driver.h"
#include "instrument.h"

uint64_t counts[3];
uint64_t *f3_profile_main_counts=counts;
uint64_t *f3_profile_sound_counts;
static unsigned exception;
void f3_exception(f3_cpu *cpu, unsigned vector, uint32_t pc) {
    assert(pc==cpu->pc); exception=vector;
}
int main(void) {
    f3_cpu cpu = fresh_cpu();
    for(size_t i=0;i<block_count;++i) {
        cpu.pc=blocks[i].address; exception=0;
        blocks[i].execute(&cpu);
        assert(exception==(cpu.pc==0 ? 10u : cpu.pc==2 ? 11u : 4u));
    }
    assert(counts[0]==1 && counts[1]==(SLIM ? 0u : 1u) && counts[2]==1);
    return 0;
}
