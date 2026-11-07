#include <assert.h>
#include <stdint.h>
#include <string.h>
#include "program.h"
static const f3_block *blocks;
static size_t block_count, reads;
static uint8_t memory[0x4000];
static uint32_t addresses[32];
static uint8_t widths[32];
int f3_validate_main_rom(f3_cpu *cpu, size_t size, uint32_t crc) {
    (void)cpu; (void)size; (void)crc; return 1;
}
int f3_register_blocks(f3_cpu *cpu, const f3_block *table, size_t count) {
    (void)cpu; blocks = table; block_count = count; return 1;
}
int f3_register_exclusions(f3_cpu *cpu, const f3_excluded_range *ranges, size_t count) {
    (void)cpu; (void)ranges; (void)count; return 1;
}
static uint32_t read_memory(uint32_t address, unsigned width) {
    assert(reads < 32 && address + width <= sizeof(memory));
    addresses[reads] = address; widths[reads++] = width;
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i) value = (value << 8) | memory[address + i];
    return value;
}
uint8_t f3_read8(f3_cpu *cpu, uint32_t address) {
    (void)cpu; return (uint8_t)read_memory(address, 1);
}
uint16_t f3_read16(f3_cpu *cpu, uint32_t address) {
    (void)cpu; return (uint16_t)read_memory(address, 2);
}
uint32_t f3_read32(f3_cpu *cpu, uint32_t address) {
    (void)cpu; return read_memory(address, 4);
}
static void dispatch(f3_cpu *cpu) {
    for (size_t i = 0; i < block_count; ++i)
        if (blocks[i].address == cpu->pc) { blocks[i].execute(cpu); return; }
    assert(0 && "missing CMPM entry");
}
int main(void) {
    f3_cpu cpu = {0};
    assert(f3_generated_register(&cpu));
    memcpy(memory + 0x1000, "TAITO", 5);
    memcpy(memory + 0x2000, "TAITO", 5);
    cpu.pc = 0x400; cpu.sr = 0x201b; cpu.d[0] = 4;
    cpu.a[0] = 0x1000; cpu.a[2] = 0x2000;
    while (cpu.pc != 0x406) dispatch(&cpu);
    assert(cpu.a[0] == 0x1005 && cpu.a[2] == 0x2005);
    f3_cc_flush(&cpu);
    assert(cpu.d[0] == 0xffff && cpu.sr == 0x2014 && reads == 10);
    for (unsigned i = 0; i < 5; ++i) {
        assert(addresses[i * 2] == 0x2000 + i && addresses[i * 2 + 1] == 0x1000 + i);
        assert(widths[i * 2] == 1 && widths[i * 2 + 1] == 1);
    }
    /* A changed EEPROM signature must stop at the first differing byte. */
    memory[0x2001] = 'X'; reads = 0;
    cpu.pc = 0x400; cpu.d[0] = 4; cpu.a[0] = 0x1000; cpu.a[2] = 0x2000;
    while (cpu.pc != 0x406) dispatch(&cpu);
    assert(cpu.a[0] == 0x1002 && cpu.a[2] == 0x2002 && cpu.d[0] == 3 && reads == 4);
    f3_cc_flush(&cpu);
    assert(cpu.sr == 0x2019); /* 'A' - 'X': N/C, preserved X. */

    reads = 0; cpu.pc = 0x406; cpu.sr = 0x2010;
    cpu.a[0] = 0x1000; cpu.a[1] = 0x2000;
    memory[0x1000] = 0x80; memory[0x1001] = 0;
    memory[0x2000] = 0x7f; memory[0x2001] = 0xff;
    dispatch(&cpu);
    f3_cc_flush(&cpu);
    assert(cpu.a[0] == 0x1002 && cpu.a[1] == 0x2002 && cpu.sr == 0x2012);
    assert(reads == 2 && addresses[0] == 0x2000 && addresses[1] == 0x1000);
    assert(widths[0] == 2 && widths[1] == 2);

    reads = 0; cpu.pc = 0x40a; cpu.a[0] = 0x1000;
    memcpy(memory + 0x1000, "\0\0\0\1\0\0\0\2", 8);
    dispatch(&cpu);
    f3_cc_flush(&cpu);
    assert(cpu.a[0] == 0x1008 && cpu.sr == 0x2010 && reads == 2);
    assert(addresses[0] == 0x1000 && addresses[1] == 0x1004 && widths[0] == 4 && widths[1] == 4);

    reads = 0; cpu.pc = 0x40e; cpu.a[7] = 0x3000;
    memory[0x3000] = 0x80; memory[0x3002] = 0x7f;
    dispatch(&cpu);
    f3_cc_flush(&cpu);
    assert(cpu.a[7] == 0x3004 && cpu.sr == 0x201b && reads == 2);
    assert(addresses[0] == 0x3000 && addresses[1] == 0x3002 && widths[0] == 1 && widths[1] == 1);
    return 0;
}
