#include <assert.h>
#include <stdint.h>
#include "program.h"
static const f3_block *blocks;
int f3_validate_main_rom(f3_cpu *cpu, size_t size, uint32_t crc) {
    (void)cpu; (void)size; (void)crc; return 1;
}
int f3_register_blocks(f3_cpu *cpu, const f3_block *table, size_t count) {
    (void)cpu; (void)count;
    blocks = table;
    return 1;
}
int f3_register_exclusions(f3_cpu *cpu, const f3_excluded_range *ranges, size_t count) {
    (void)cpu; (void)ranges; (void)count; return 1;
}
int main(void) {
    f3_cpu cpu = {0};
    assert(f3_generated_register(&cpu));
    cpu.pc = 0x400; cpu.sr = 0x10; cpu.d[1] = 0x12345678;
    cpu.dispatch_deadline = 2;
    blocks[0].execute(&cpu);
    assert(cpu.pc == 0x402 && cpu.cycles == 2);
    assert(cpu.d[0] == UINT32_MAX && cpu.d[1] == 0x12345678);
    f3_cc_flush(&cpu);
    assert(cpu.sr == 0x18 && cpu.cc_op == 0);

    cpu.dispatch_deadline = cpu.cycles + 1;            /* MOVE CCR,D3 */
    blocks[0].execute(&cpu);
    assert(cpu.pc == 0x404 && (cpu.d[3] & 0x1f) == 0x18);

    cpu.dispatch_deadline = cpu.cycles + 1;            /* ADDQ.W #1,D0 */
    blocks[0].execute(&cpu);
    assert(cpu.pc == 0x406 && cpu.d[0] == 0xffff0000);
    f3_cc_flush(&cpu);
    assert(cpu.sr == 0x15 && cpu.cc_op == 0);

    uint64_t resumed = cpu.cycles;
    cpu.dispatch_deadline = UINT64_MAX;
    blocks[0].execute(&cpu);
    assert(cpu.pc == 0x40c && cpu.cycles > resumed);
    assert(cpu.d[0] == 0xffff0000 && cpu.d[1] == 1);
    assert((cpu.d[4] & 0x1f) == 0x15 && (cpu.d[5] & 0x1f) == 0x10);
    f3_cc_flush(&cpu);
    assert(cpu.sr == 0x10 && cpu.cc_op == 0);
    return 0;
}
