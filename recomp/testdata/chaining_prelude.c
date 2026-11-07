#include <assert.h>
#include <stdint.h>
#include "program.h"
static const f3_block *blocks;
static size_t block_count;
static uint8_t memory[0x20000];
int f3_validate_main_rom(f3_cpu *cpu, size_t size, uint32_t crc) {
    (void)cpu; (void)size; (void)crc; return 1;
}
int f3_register_blocks(f3_cpu *cpu, const f3_block *table, size_t count) {
    (void)cpu; blocks = table; block_count = count; return 1;
}
int f3_register_exclusions(f3_cpu *cpu, const f3_excluded_range *ranges, size_t count) {
    (void)cpu; (void)ranges; (void)count; return 1;
}
uint8_t f3_read8(f3_cpu *cpu, uint32_t address) { (void)cpu; return memory[address & 0x1ffffu]; }
uint16_t f3_read16(f3_cpu *cpu, uint32_t address) {
    (void)cpu;
    return (uint16_t)((memory[address & 0x1ffffu] << 8) | memory[(address + 1u) & 0x1ffffu]);
}
uint32_t f3_read32(f3_cpu *cpu, uint32_t address) {
    return ((uint32_t)f3_read16(cpu, address) << 16) | f3_read16(cpu, address + 2u);
}
void f3_write8(f3_cpu *cpu, uint32_t address, uint8_t value) {
    (void)cpu; memory[address & 0x1ffffu] = value;
}
void f3_write16(f3_cpu *cpu, uint32_t address, uint16_t value) {
    (void)cpu; memory[address & 0x1ffffu] = (uint8_t)(value >> 8);
    memory[(address + 1u) & 0x1ffffu] = (uint8_t)value;
}
void f3_write32(f3_cpu *cpu, uint32_t address, uint32_t value) {
    f3_write16(cpu, address, (uint16_t)(value >> 16));
    f3_write16(cpu, address + 2u, (uint16_t)value);
}
static int dispatch(f3_cpu *cpu) {
    for (size_t i = 0; i < block_count; ++i)
        if (blocks[i].address == cpu->pc) { blocks[i].execute(cpu); return 1; }
    return 0;
}
static f3_cpu fresh_cpu(void) {
    f3_cpu cpu = {0};
    assert(f3_generated_register(&cpu));
    return cpu;
}
