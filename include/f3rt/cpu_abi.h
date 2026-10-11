#ifndef F3RT_CPU_ABI_H
#define F3RT_CPU_ABI_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define F3RT_ABI_VERSION 5u

/* a[7] is the active stack. usp/ssp/msp store inactive user/interrupt/master
 * stacks. All addresses/registers are host integers; bus accesses are big-endian.
 * cc_* is part of CPU state; any code outside generated blocks that reads CCR
 * bits of sr must call f3_cc_flush first. Generated code does not flush at block
 * exits or chains. Runtime writes to SR invalidate cc_op. cycles is a monotonic
 * scheduling clock, not exact timing. */
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
     * Native blocks yield between instructions when due. */
    uint64_t dispatch_deadline;
    void *runtime;
} f3_cpu;

/* Condition Code Operations for Lazy Evaluation */
enum {
    F3_CC_OP_NONE  = 0,
    F3_CC_OP_LOGIC = 1,
    F3_CC_OP_ADD   = 2,
    F3_CC_OP_SUB   = 3,
    F3_CC_OP_CMP   = 4,
};

/* CCR Flags bit positions in SR */
#define F3_CCR_C 0x01u
#define F3_CCR_V 0x02u
#define F3_CCR_Z 0x04u
#define F3_CCR_N 0x08u
#define F3_CCR_X 0x10u

/* Materialize pending condition codes into canonical cpu->sr. */
static inline void f3_cc_flush(f3_cpu *cpu) {
    uint8_t op = cpu->cc_op;
    if (op == F3_CC_OP_NONE) {
        return;
    }
    uint32_t res = cpu->cc_result;
    uint8_t w = cpu->cc_width;
    uint32_t mask = (w == 1) ? 0xffu : ((w == 2) ? 0xffffu : 0xffffffffu);
    uint32_t sign = (w == 1) ? 0x80u : ((w == 2) ? 0x8000u : 0x80000000u);
    uint16_t ccr = 0;

    if (op == F3_CC_OP_LOGIC) {
        ccr = (cpu->sr & F3_CCR_X); /* X preserved */
        if ((res & mask) == 0) ccr |= F3_CCR_Z;
        if (res & sign) ccr |= F3_CCR_N;
        /* V = 0, C = 0 */
    } else if (op == F3_CC_OP_ADD) {
        uint32_t src = cpu->cc_src;
        uint32_t dst = cpu->cc_dst;
        int c = (res & mask) < (src & mask);
        int v = ((src ^ res) & (dst ^ res) & sign) != 0;
        int z = (res & mask) == 0;
        int n = (res & sign) != 0;
        if (c) ccr |= (F3_CCR_X | F3_CCR_C);
        if (n) ccr |= F3_CCR_N;
        if (z) ccr |= F3_CCR_Z;
        if (v) ccr |= F3_CCR_V;
    } else if (op == F3_CC_OP_SUB) {
        uint32_t src = cpu->cc_src;
        uint32_t dst = cpu->cc_dst;
        int c = (dst & mask) < (src & mask);
        int v = ((dst ^ src) & (dst ^ res) & sign) != 0;
        int z = (res & mask) == 0;
        int n = (res & sign) != 0;
        if (c) ccr |= (F3_CCR_X | F3_CCR_C);
        if (n) ccr |= F3_CCR_N;
        if (z) ccr |= F3_CCR_Z;
        if (v) ccr |= F3_CCR_V;
    } else if (op == F3_CC_OP_CMP) {
        uint32_t src = cpu->cc_src;
        uint32_t dst = cpu->cc_dst;
        ccr = (cpu->sr & F3_CCR_X); /* X preserved */
        int c = (dst & mask) < (src & mask);
        int v = ((dst ^ src) & (dst ^ res) & sign) != 0;
        int z = (res & mask) == 0;
        int n = (res & sign) != 0;
        if (c) ccr |= F3_CCR_C;
        if (n) ccr |= F3_CCR_N;
        if (z) ccr |= F3_CCR_Z;
        if (v) ccr |= F3_CCR_V;
    }

    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    cpu->cc_op = F3_CC_OP_NONE;
}

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

/* Called before lookup at block boundaries. Nonzero: do not execute the
 * previously selected block (IRQ changed PC, STOP, halt). Any caller reading
 * CCR bits of sr must flush pending flags via f3_cc_flush first. Dispatch must
 * look up the new PC at the next step, never retain stale code. */
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
/* Execute exactly one instruction at cpu->pc through the interpreter, which
 * flushes pending flags before reading CCR; canonical SR on exit. Return
 * nonzero on success; zero means no fallback is available. */
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
