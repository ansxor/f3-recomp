/* SPDX-License-Identifier: MIT
 * f3rt CPU ABI v1. All addresses and register values are guest numeric values.
 * Guest memory is big endian; no host-endian RAM pointers cross this ABI.
 */
#ifndef F3RT_CPU_ABI_H
#define F3RT_CPU_ABI_H
#include <stddef.h>
#include <stdint.h>
#ifdef __cplusplus
extern "C" {
#endif
#define F3RT_ABI_VERSION 1u

typedef struct f3_cpu {
    uint32_t d[8];
    uint32_t a[8];
    uint32_t pc;
    uint32_t usp;
    uint32_t ssp;
    uint32_t vbr;
    uint16_t sr;
    uint8_t stopped;
    uint8_t reserved;
    uint64_t cycles;
    void *runtime;
} f3_cpu;

/* SR is always materialized at ABI calls. a[7] is the active stack;
 * usp/ssp hold the inactive stack (the active shadow need not be current).
 * cycles is cumulative main-CPU cycles, increased by each translated block
 * and by fallback. Blocks return to the dispatcher, never recursively call
 * another guest block. Every block sets pc to its successor before returning.
 */
typedef void (*f3_block_fn)(f3_cpu *cpu);
typedef struct f3_block {
    uint32_t address;
    f3_block_fn execute;
} f3_block;

uint8_t f3_read8(f3_cpu *cpu, uint32_t address);
uint16_t f3_read16(f3_cpu *cpu, uint32_t address);
uint32_t f3_read32(f3_cpu *cpu, uint32_t address);
void f3_write8(f3_cpu *cpu, uint32_t address, uint8_t value);
void f3_write16(f3_cpu *cpu, uint32_t address, uint16_t value);
void f3_write32(f3_cpu *cpu, uint32_t address, uint32_t value);

/* Call before each block. Nonzero: do not execute the selected block because
 * an interrupt changed pc, STOP remains active, or the host requested yield.
 * Do not clear stopped in translated code except RESET/exception semantics.
 */
int f3_boundary(f3_cpu *cpu);
/* Enter vector (number, not byte offset) using a 68020 format-0 stack frame.
 * return_pc is pushed exactly; exception entry clears STOP and trace bits.
 */
void f3_exception(f3_cpu *cpu, uint32_t vector, uint32_t return_pc);
/* Set SR with supervisor/user stack switching. */
void f3_set_sr(f3_cpu *cpu, uint16_t sr);
/* Install caller-owned address-sorted table; duplicate addresses rejected.
 * Returns 1 on success, 0 on invalid table. Table lives until replaced.
 */
int f3_register_blocks(f3_cpu *cpu, const f3_block *blocks, size_t count);
/* Execute one block at pc (or fallback), including boundary. Returns 1 if
 * progress/interrupt/STOP time advancement occurred; 0 on a fatal host error.
 */
int f3_dispatch(f3_cpu *cpu);
/* Execute exactly one instruction at pc using the runtime interpreter.
 * Returns 1 on success, 0 if no interpreter is configured or execution fails.
 * All registers/SR/pc and cycles are synchronized; never called inside a block
 * after that block has already performed part of the same instruction.
 */
int f3_fallback(f3_cpu *cpu);
#ifdef __cplusplus
}
#endif
#endif
