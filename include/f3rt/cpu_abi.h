#ifndef F3RT_CPU_ABI_H
#define F3RT_CPU_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define F3RT_ABI_VERSION 4u

/* a[7] is the active stack. usp/ssp/msp store inactive user/interrupt/master
 * stacks. All addresses/registers are host integers; bus accesses are big-endian.
 * Pending flags are private to recomp lowering and MUST be flushed before any
 * runtime callback except ordinary memory reads/writes. Runtime writes to SR
 * invalidate cc_op. cycles is a monotonic scheduling clock, not exact timing. */
typedef struct f3_cpu {
    uint32_t d[8], a[8];
    uint32_t pc, usp, ssp, msp, vbr;
    uint32_t sfc, dfc, cacr, caar;
    uint16_t sr;
    uint8_t stopped, halted;
    uint32_t cc_src, cc_dst, cc_result;
    uint8_t cc_op, cc_width, cc_mask, cc_pad;
    uint64_t cycles;
    /* Conservative event threshold. Zero forces a boundary recheck.
     * Native blocks must flush SR and yield between instructions when due. */
    uint64_t dispatch_deadline;
    void *runtime;
} f3_cpu;

typedef void (*f3_block_fn)(f3_cpu *cpu);
typedef struct f3_block {
    uint32_t address;
    f3_block_fn execute;
} f3_block;

/* Immutable instruction-start exclusions; end is exclusive. ROM data reads
 * remain legal. Metadata and strings must outlive the CPU. */
typedef struct f3_excluded_range {
    uint32_t start, end;
    const char *reason, *evidence;
} f3_excluded_range;

/* Native sound-mailbox/reset accesses catch device time up to cycles before
 * taking effect, including unaligned accesses crossing into the mapped range.
 * This does not deliver main-CPU IRQs or require pending flags to be flushed. */
uint8_t f3_read8(f3_cpu *cpu, uint32_t address);
uint16_t f3_read16(f3_cpu *cpu, uint32_t address);
uint32_t f3_read32(f3_cpu *cpu, uint32_t address);
void f3_write8(f3_cpu *cpu, uint32_t address, uint8_t value);
void f3_write16(f3_cpu *cpu, uint32_t address, uint16_t value);
void f3_write32(f3_cpu *cpu, uint32_t address, uint32_t value);

/* Called before lookup at every block boundary with canonical SR. Nonzero:
 * do not execute the previously selected block (IRQ changed PC, STOP, halt).
 * Dispatch must look up the new PC at the next step, never retain stale code. */
int f3_boundary(f3_cpu *cpu);
void f3_exception(f3_cpu *cpu, unsigned vector, uint32_t return_pc);
void f3_set_sr(f3_cpu *cpu, uint16_t sr);
void f3_reset_devices(f3_cpu *cpu);

/* Table is sorted by address and remains valid for the CPU's lifetime.
 * Returns 1 on success, 0 if invalid. Duplicate addresses are rejected. */
int f3_register_blocks(f3_cpu *cpu, const f3_block *blocks, size_t count);
/* Bind generated main code to the loaded image before registering any entries. */
int f3_validate_main_rom(f3_cpu *cpu, size_t size, uint32_t crc);
/* Sorted nonoverlapping even ROM intervals, disjoint from registered blocks.
 * Excluded PCs fail before fallback, even when interpretation is enabled. */
int f3_register_exclusions(f3_cpu *cpu, const f3_excluded_range *ranges, size_t count);
/* Execute one block at pc (or fallback), including boundary. Returns 1 for
 * progress, IRQ entry, or STOP time advancement; 0 on fatal host error/halt. */
int f3_dispatch(f3_cpu *cpu);
/* Execute exactly one instruction at cpu->pc. Canonical SR on entry/exit.
 * Return nonzero on success; zero means no fallback is available. */
int f3_fallback(f3_cpu *cpu);

/* Render-only emit-unit hooks (ABI 4). Generated code calls these at the label
 * of a declared unit start/end PC, after flushing pending flags and before that
 * instruction executes; the interpreter calls them at the same instruction
 * boundary. They never change emulated state of the real CPU. `unit` is the
 * EmitUnit id from the generated sprite_units.h. f3_unit_exit returns nonzero
 * only for a sandbox replay CPU that reached its unit end: the caller must then
 * return without executing the instruction at cpu->pc. */
void f3_unit_enter(f3_cpu *cpu, uint32_t unit);
int f3_unit_exit(f3_cpu *cpu, uint32_t unit);

#ifdef __cplusplus
}
#endif
#endif
