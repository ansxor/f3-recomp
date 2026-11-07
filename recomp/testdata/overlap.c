#include <assert.h>
#include <stdint.h>
#include "program.h"
static const f3_block *blocks;
static size_t block_count;
int f3_validate_main_rom(f3_cpu *cpu, size_t size, uint32_t crc) {
    (void)cpu; (void)size; (void)crc; return 1;
}
int f3_register_blocks(f3_cpu *cpu, const f3_block *table, size_t count) {
    (void)cpu; blocks = table; block_count = count; return 1;
}
int f3_register_exclusions(f3_cpu *cpu, const f3_excluded_range *ranges, size_t count) {
    (void)cpu; (void)ranges; (void)count; return 1;
}
static void dispatch(f3_cpu *cpu) {
    for (size_t i = 0; i < block_count; ++i)
        if (blocks[i].address == cpu->pc) { blocks[i].execute(cpu); return; }
    assert(0 && "missing computed entry");
}
int main(void) {
    f3_cpu cpu = {0};
    assert(f3_generated_register(&cpu));
    cpu.pc = 0x400; cpu.d[1] = 99; cpu.d[2] = 98;
    cpu.dispatch_deadline = 1;
    dispatch(&cpu);
    assert(cpu.pc == 0x406 && cpu.d[0] == 0x70017202);
    f3_cc_flush(&cpu);
    assert(cpu.d[1] == 99 && cpu.d[2] == 98 && cpu.cc_op == 0);
    cpu.dispatch_deadline = UINT64_MAX;
    while (cpu.pc != 0x40a) dispatch(&cpu);
    assert(cpu.d[1] == 99 && cpu.d[2] == 3);
    cpu.pc = 0x402; cpu.d[0] = 0; cpu.d[1] = 99;
    cpu.dispatch_deadline = cpu.cycles + 1;
    dispatch(&cpu);
    assert(cpu.pc == 0x404 && cpu.d[0] == 1 && cpu.d[1] == 99);
    f3_cc_flush(&cpu);
    assert(cpu.cc_op == 0 && (cpu.sr & 15) == 0);
    cpu.dispatch_deadline = UINT64_MAX;
    while (cpu.pc != 0x40a) dispatch(&cpu);
    assert(cpu.d[0] == 1 && cpu.d[1] == 2 && cpu.d[2] == 3);
    return 0;
}
