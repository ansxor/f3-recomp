#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <f3rt/cpu_abi.h>
#include "recomp/cpu_ops.h"
static uint64_t wsum;
static uint32_t hash32(uint32_t value, unsigned width) {
    value = (value + width) * 2654435761u; return value ^ (value >> 15);
}
uint8_t f3_read8(f3_cpu *cpu, uint32_t a) { (void)cpu; return (uint8_t)hash32(a, 1); }
uint16_t f3_read16(f3_cpu *cpu, uint32_t a) { (void)cpu; return (uint16_t)hash32(a, 2); }
uint32_t f3_read32(f3_cpu *cpu, uint32_t a) { (void)cpu; return hash32(a, 4); }
void f3_write8(f3_cpu *cpu, uint32_t a, uint8_t v) { (void)cpu; wsum = wsum * 31 + a * 7 + v + 1; }
void f3_write16(f3_cpu *cpu, uint32_t a, uint16_t v) { (void)cpu; wsum = wsum * 37 + a * 7 + v + 2; }
void f3_write32(f3_cpu *cpu, uint32_t a, uint32_t v) { (void)cpu; wsum = wsum * 41 + a * 7 + v + 4; }
void f3_exception(f3_cpu *cpu, unsigned vector, uint32_t return_pc) {
    f3_cc_flush(cpu);                       /* the real entry flushes before stacking SR */
    cpu->usp = return_pc; cpu->pc = 0xdead0000u + vector;
}
