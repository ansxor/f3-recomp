#ifndef RECOMP_CPU_OPS_H
#define RECOMP_CPU_OPS_H

#include <stdint.h>
#include <string.h>
#include <f3rt/cpu_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The expansion must stay a tail call: the block functions are mutually
 * recursive, so a plain (non-tail) call would grow the host stack without
 * bound. __attribute__((musttail)) guarantees it where available; otherwise
 * the transfer degrades to a plain return, which re-enters the dispatcher and
 * is merely slower.
 */
#if defined(__has_attribute)
#  if __has_attribute(musttail)
#    define F3_CHAIN(target, cpu) __attribute__((musttail)) return target(cpu)
#  endif
#endif
#ifndef F3_CHAIN
#  define F3_CHAIN(target, cpu) return
#endif

/* One entry per 2-byte ROM address: the block function registered for that pc,
 * NULL where there is none. Defined and filled by the generated program
 * (f3_generated_register), so main-CPU code only. */
#define F3_ENTRY_WORDS 0x100000u
extern f3_block_fn f3_entry_by_word[F3_ENTRY_WORDS];

/* Transfer to cpu->pc (set by an rts or computed jump) without returning to the
 * dispatcher under the chain guard: no trace bits and
 * cpu->cycles < dispatch_deadline, so boundary() would only republish the same
 * deadline. Anything else, including a pc with no block, falls through to the
 * caller's return. Expands to a tail call exactly like F3_CHAIN. */
#define F3_DISPATCH_PC(cpu)                                                                        \
    do {                                                                                           \
        if ((cpu)->pc < 2u * F3_ENTRY_WORDS && !((cpu)->pc & 1u) && !((cpu)->sr & 0xc000u) &&     \
            (cpu)->cycles < (cpu)->dispatch_deadline) {                                            \
            f3_block_fn f3_next = f3_entry_by_word[(cpu)->pc >> 1];                                \
            if (f3_next) { F3_CHAIN(f3_next, cpu); }                                               \
        }                                                                                          \
    } while (0)

/* Guest memory access of generated main-CPU code. Naturally aligned accesses to
 * main work RAM (any of the four 128 KiB mirrors in 0x400000..0x43ffff) and
 * aligned program-ROM reads go straight to the host window the runtime publishes
 * in the cpu; everything else, including all of it while a window is unset
 * (replay sandbox), takes the f3_read and f3_write callbacks. A RAM store also
 * records the writing pc in the halfword attribution table when sprite units
 * track it, exactly as Machine::write* does. The 24-bit bus ignores the top byte. */
#define F3_RAM_WINDOW 0x00fc0000u
#define F3_RAM_BASE 0x00400000u
#define F3_RAM_MASK 0x0001ffffu

static inline int f3_in_ram(const f3_cpu *cpu, uint32_t a, uint32_t align_mask) {
    return cpu->ram && (a & (F3_RAM_WINDOW | align_mask)) == F3_RAM_BASE;
}
static inline int f3_in_rom(const f3_cpu *cpu, uint32_t a, uint32_t width) {
    return (a & 0xffffffu) + width <= cpu->rom_size && !(a & (width - 1u));
}

static inline uint8_t f3_ld8(f3_cpu *cpu, uint32_t a) {
    if (f3_in_ram(cpu, a, 0u)) return cpu->ram[a & F3_RAM_MASK];
    if (f3_in_rom(cpu, a, 1u)) return cpu->rom[a & 0xffffffu];
    return f3_read8(cpu, a);
}
static inline uint16_t f3_ld16(f3_cpu *cpu, uint32_t a) {
    const uint8_t *p;
    if (f3_in_ram(cpu, a, 1u)) p = cpu->ram + (a & F3_RAM_MASK);
    else if (f3_in_rom(cpu, a, 2u)) p = cpu->rom + (a & 0xffffffu);
    else return f3_read16(cpu, a);
    uint16_t v;
    memcpy(&v, __builtin_assume_aligned(p, 2), 2);
    return __builtin_bswap16(v);
}
static inline uint32_t f3_ld32(f3_cpu *cpu, uint32_t a) {
    const uint8_t *p;
    if (f3_in_ram(cpu, a, 3u)) p = cpu->ram + (a & F3_RAM_MASK);
    else if (f3_in_rom(cpu, a, 4u)) p = cpu->rom + (a & 0xffffffu);
    else return f3_read32(cpu, a);
    uint32_t v;
    memcpy(&v, __builtin_assume_aligned(p, 4), 4);
    return __builtin_bswap32(v);
}
static inline void f3_st8(f3_cpu *cpu, uint32_t a, uint8_t v) {
    if (!f3_in_ram(cpu, a, 0u)) return f3_write8(cpu, a, v);
    cpu->ram[a & F3_RAM_MASK] = v;
    if (cpu->ram_writer) cpu->ram_writer[(a & F3_RAM_MASK) >> 1] = cpu->pc;
}
static inline void f3_st16(f3_cpu *cpu, uint32_t a, uint16_t v) {
    if (!f3_in_ram(cpu, a, 1u)) return f3_write16(cpu, a, v);
    const uint32_t offset = a & F3_RAM_MASK;
    const uint16_t big = __builtin_bswap16(v);
    memcpy(__builtin_assume_aligned(cpu->ram + offset, 2), &big, 2);
    if (cpu->ram_writer) cpu->ram_writer[offset >> 1] = cpu->pc;
}
static inline void f3_st32(f3_cpu *cpu, uint32_t a, uint32_t v) {
    if (!f3_in_ram(cpu, a, 3u)) return f3_write32(cpu, a, v);
    const uint32_t offset = a & F3_RAM_MASK;
    const uint32_t big = __builtin_bswap32(v);
    memcpy(__builtin_assume_aligned(cpu->ram + offset, 4), &big, 4);
    if (cpu->ram_writer) {
        cpu->ram_writer[offset >> 1] = cpu->pc;
        cpu->ram_writer[(offset >> 1) + 1u] = cpu->pc;
    }
}

/* Emit-unit hooks planted at declared start/end PCs (ABI 4). Flags are flushed so
 * a sandbox replay observes canonical sr; EXIT returns before the instruction at
 * cpu->pc runs when a replay CPU reached its unit end. */
#define F3_UNIT_ENTER(cpu, id) do { f3_cc_flush(cpu); f3_unit_enter((cpu), (id)); } while (0)
#define F3_UNIT_EXIT(cpu, id) do { f3_cc_flush(cpu); if (f3_unit_exit((cpu), (id))) return; } while (0)

static inline int f3_cond_ccr(uint16_t sr, uint8_t cond) {
    int c = sr & F3_CCR_C;
    int v = (sr & F3_CCR_V) != 0;
    int z = (sr & F3_CCR_Z) != 0;
    int n = (sr & F3_CCR_N) != 0;
    switch (cond & 0x0fu) {
        case 0: return 1;                   /* T  */
        case 1: return 0;                   /* F  */
        case 2: return !c && !z;            /* HI */
        case 3: return c || z;              /* LS */
        case 4: return !c;                  /* CC */
        case 5: return c;                   /* CS */
        case 6: return !z;                  /* NE */
        case 7: return z;                   /* EQ */
        case 8: return !v;                  /* VC */
        case 9: return v;                   /* VS */
        case 10: return !n;                 /* PL */
        case 11: return n;                  /* MI */
        case 12: return !(n ^ v);           /* GE */
        case 13: return n ^ v;              /* LT */
        case 14: return !(n ^ v) && !z;     /* GT */
        case 15: return (n ^ v) || z;       /* LE */
    }
    return 0;
}

static inline int f3_eval_cond(f3_cpu *cpu, uint8_t cond) {
    f3_cc_flush(cpu);
    return f3_cond_ccr(cpu->sr, cond);
}

/* Logical Shift Left */
static inline uint32_t f3_lsl(f3_cpu *cpu, uint32_t val, uint32_t count, uint8_t width) {
    f3_cc_flush(cpu);
    count &= 63u;
    uint32_t mask = (width == 1) ? 0xffu : ((width == 2) ? 0xffffu : 0xffffffffu);
    uint32_t sign = (width == 1) ? 0x80u : ((width == 2) ? 0x8000u : 0x80000000u);
    val &= mask;
    if (count == 0) {
        uint16_t ccr = (cpu->sr & F3_CCR_X);
        if (val == 0) ccr |= F3_CCR_Z;
        if (val & sign) ccr |= F3_CCR_N;
        cpu->sr = (cpu->sr & ~0x1fu) | ccr;
        return val;
    }
    uint32_t bits = width * 8u;
    uint32_t carry = 0;
    uint32_t res = 0;
    if (count <= bits) {
        carry = (val >> (bits - count)) & 1u;
        res = (count == bits) ? 0 : ((val << count) & mask);
    }
    uint16_t ccr = 0;
    if (carry) ccr |= (F3_CCR_X | F3_CCR_C);
    if (res == 0) ccr |= F3_CCR_Z;
    if (res & sign) ccr |= F3_CCR_N;
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return res;
}

/* Logical Shift Right */
static inline uint32_t f3_lsr(f3_cpu *cpu, uint32_t val, uint32_t count, uint8_t width) {
    f3_cc_flush(cpu);
    count &= 63u;
    uint32_t mask = (width == 1) ? 0xffu : ((width == 2) ? 0xffffu : 0xffffffffu);
    uint32_t sign = (width == 1) ? 0x80u : ((width == 2) ? 0x8000u : 0x80000000u);
    val &= mask;
    if (count == 0) {
        uint16_t ccr = (cpu->sr & F3_CCR_X);
        if (val == 0) ccr |= F3_CCR_Z;
        if (val & sign) ccr |= F3_CCR_N;
        cpu->sr = (cpu->sr & ~0x1fu) | ccr;
        return val;
    }
    uint32_t bits = width * 8u;
    uint32_t carry = 0;
    uint32_t res = 0;
    if (count <= bits) {
        carry = (val >> (count - 1u)) & 1u;
        res = (count == bits) ? 0 : ((val >> count) & mask);
    }
    uint16_t ccr = 0;
    if (carry) ccr |= (F3_CCR_X | F3_CCR_C);
    if (res == 0) ccr |= F3_CCR_Z;
    if (res & sign) ccr |= F3_CCR_N;
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return res;
}

/* Arithmetic Shift Right */
static inline uint32_t f3_asr(f3_cpu *cpu, uint32_t val, uint32_t count, uint8_t width) {
    f3_cc_flush(cpu);
    count &= 63u;
    uint32_t mask = (width == 1) ? 0xffu : ((width == 2) ? 0xffffu : 0xffffffffu);
    uint32_t sign = (width == 1) ? 0x80u : ((width == 2) ? 0x8000u : 0x80000000u);
    val &= mask;
    if (count == 0) {
        uint16_t ccr = (cpu->sr & F3_CCR_X);
        if (val == 0) ccr |= F3_CCR_Z;
        if (val & sign) ccr |= F3_CCR_N;
        cpu->sr = (cpu->sr & ~0x1fu) | ccr;
        return val;
    }
    uint32_t bits = width * 8u;
    uint32_t carry = 0;
    uint32_t res = 0;
    int is_neg = (val & sign) != 0;
    if (count >= bits) {
        carry = is_neg ? 1u : 0u;
        res = is_neg ? mask : 0u;
    } else {
        carry = (val >> (count - 1u)) & 1u;
        if (is_neg) {
            uint32_t top = mask << (bits - count);
            res = ((val >> count) | top) & mask;
        } else {
            res = (val >> count) & mask;
        }
    }
    uint16_t ccr = 0;
    if (carry) ccr |= (F3_CCR_X | F3_CCR_C);
    if (res == 0) ccr |= F3_CCR_Z;
    if (res & sign) ccr |= F3_CCR_N;
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return res;
}

/* Arithmetic Shift Left */
static inline uint32_t f3_asl(f3_cpu *cpu, uint32_t val, uint32_t count, uint8_t width) {
    f3_cc_flush(cpu);
    count &= 63u;
    uint32_t mask = (width == 1) ? 0xffu : ((width == 2) ? 0xffffu : 0xffffffffu);
    uint32_t sign = (width == 1) ? 0x80u : ((width == 2) ? 0x8000u : 0x80000000u);
    val &= mask;
    if (count == 0) {
        uint16_t ccr = (cpu->sr & F3_CCR_X);
        if (val == 0) ccr |= F3_CCR_Z;
        if (val & sign) ccr |= F3_CCR_N;
        cpu->sr = (cpu->sr & ~0x1fu) | ccr;
        return val;
    }
    uint32_t bits = width * 8u;
    uint32_t carry = 0;
    uint32_t res = 0;
    uint32_t v = 0;
    if (count <= bits) {
        carry = (val >> (bits - count)) & 1u;
        res = (count == bits) ? 0 : ((val << count) & mask);
        uint32_t top_shift = bits - (count + 1u);
        uint32_t top_mask = (count + 1u >= bits) ? mask : (~((1u << top_shift) - 1u) & mask);
        uint32_t top_bits = val & top_mask;
        if (top_bits != 0 && top_bits != top_mask) {
            v = 1;
        }
        if (count == bits) v = val != 0;
    } else {
        carry = 0;
        res = 0;
        if (val != 0) {
            v = 1;
        }
    }
    uint16_t ccr = 0;
    if (carry) ccr |= (F3_CCR_X | F3_CCR_C);
    if (v) ccr |= F3_CCR_V;
    if (res == 0) ccr |= F3_CCR_Z;
    if (res & sign) ccr |= F3_CCR_N;
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return res;
}

/* Rotate Left */
static inline uint32_t f3_rol(f3_cpu *cpu, uint32_t val, uint32_t count, uint8_t width) {
    f3_cc_flush(cpu);
    count &= 63u;
    uint32_t mask = (width == 1) ? 0xffu : ((width == 2) ? 0xffffu : 0xffffffffu);
    uint32_t sign = (width == 1) ? 0x80u : ((width == 2) ? 0x8000u : 0x80000000u);
    val &= mask;
    uint16_t ccr = (cpu->sr & F3_CCR_X);
    if (count == 0) {
        if (val == 0) ccr |= F3_CCR_Z;
        if (val & sign) ccr |= F3_CCR_N;
        cpu->sr = (cpu->sr & ~0x1fu) | ccr;
        return val;
    }
    uint32_t bits = width * 8u;
    count %= bits;
    uint32_t res = (count == 0) ? val : (((val << count) | (val >> (bits - count))) & mask);
    uint32_t carry = res & 1u;
    if (carry) ccr |= F3_CCR_C;
    if (res == 0) ccr |= F3_CCR_Z;
    if (res & sign) ccr |= F3_CCR_N;
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return res;
}

/* Rotate Right */
static inline uint32_t f3_ror(f3_cpu *cpu, uint32_t val, uint32_t count, uint8_t width) {
    f3_cc_flush(cpu);
    count &= 63u;
    uint32_t mask = (width == 1) ? 0xffu : ((width == 2) ? 0xffffu : 0xffffffffu);
    uint32_t sign = (width == 1) ? 0x80u : ((width == 2) ? 0x8000u : 0x80000000u);
    val &= mask;
    uint16_t ccr = (cpu->sr & F3_CCR_X);
    if (count == 0) {
        if (val == 0) ccr |= F3_CCR_Z;
        if (val & sign) ccr |= F3_CCR_N;
        cpu->sr = (cpu->sr & ~0x1fu) | ccr;
        return val;
    }
    uint32_t bits = width * 8u;
    count %= bits;
    uint32_t res = (count == 0) ? val : (((val >> count) | (val << (bits - count))) & mask);
    uint32_t carry = (res & sign) ? 1u : 0u;
    if (carry) ccr |= F3_CCR_C;
    if (res == 0) ccr |= F3_CCR_Z;
    if (res & sign) ccr |= F3_CCR_N;
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return res;
}

/* Rotate with Extend Left */
static inline uint32_t f3_roxl(f3_cpu *cpu, uint32_t val, uint32_t count, uint8_t width) {
    f3_cc_flush(cpu);
    count &= 63u;
    uint32_t mask = (width == 1) ? 0xffu : ((width == 2) ? 0xffffu : 0xffffffffu);
    uint32_t sign = (width == 1) ? 0x80u : ((width == 2) ? 0x8000u : 0x80000000u);
    val &= mask;
    uint32_t x = (cpu->sr & F3_CCR_X) ? 1u : 0u;
    if (count == 0) {
        uint16_t ccr = (cpu->sr & F3_CCR_X);
        if (x) ccr |= F3_CCR_C;
        if (val == 0) ccr |= F3_CCR_Z;
        if (val & sign) ccr |= F3_CCR_N;
        cpu->sr = (cpu->sr & ~0x1fu) | ccr;
        return val;
    }
    uint32_t ring_size = (width * 8u) + 1u;
    count %= ring_size;
    uint64_t ring = ((uint64_t)val) | (((uint64_t)x) << (ring_size - 1u));
    if (count > 0) {
        ring = ((ring << count) | (ring >> (ring_size - count))) & ((1ULL << ring_size) - 1ULL);
    }
    uint32_t new_x = (uint32_t)((ring >> (ring_size - 1u)) & 1u);
    uint32_t res = (uint32_t)ring & mask;
    uint16_t ccr = 0;
    if (new_x) ccr |= (F3_CCR_X | F3_CCR_C);
    if (res == 0) ccr |= F3_CCR_Z;
    if (res & sign) ccr |= F3_CCR_N;
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return res;
}

/* Rotate with Extend Right */
static inline uint32_t f3_roxr(f3_cpu *cpu, uint32_t val, uint32_t count, uint8_t width) {
    f3_cc_flush(cpu);
    count &= 63u;
    uint32_t mask = (width == 1) ? 0xffu : ((width == 2) ? 0xffffu : 0xffffffffu);
    uint32_t sign = (width == 1) ? 0x80u : ((width == 2) ? 0x8000u : 0x80000000u);
    val &= mask;
    uint32_t x = (cpu->sr & F3_CCR_X) ? 1u : 0u;
    if (count == 0) {
        uint16_t ccr = (cpu->sr & F3_CCR_X);
        if (x) ccr |= F3_CCR_C;
        if (val == 0) ccr |= F3_CCR_Z;
        if (val & sign) ccr |= F3_CCR_N;
        cpu->sr = (cpu->sr & ~0x1fu) | ccr;
        return val;
    }
    uint32_t ring_size = (width * 8u) + 1u;
    count %= ring_size;
    uint64_t ring = (((uint64_t)val) << 1) | (uint64_t)x;
    if (count > 0) {
        ring = ((ring >> count) | (ring << (ring_size - count))) & ((1ULL << ring_size) - 1ULL);
    }
    uint32_t new_x = (uint32_t)(ring & 1u);
    uint32_t res = (uint32_t)(ring >> 1) & mask;
    uint16_t ccr = 0;
    if (new_x) ccr |= (F3_CCR_X | F3_CCR_C);
    if (res == 0) ccr |= F3_CCR_Z;
    if (res & sign) ccr |= F3_CCR_N;
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return res;
}

/* Signed Multiply 16x16 -> 32 */
static inline uint32_t f3_muls_w(f3_cpu *cpu, int16_t src, int16_t dst) {
    f3_cc_flush(cpu);
    int32_t res = (int32_t)src * (int32_t)dst;
    uint16_t ccr = (cpu->sr & F3_CCR_X);
    if (res == 0) ccr |= F3_CCR_Z;
    if (res < 0) ccr |= F3_CCR_N;
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return (uint32_t)res;
}

/* Unsigned Multiply 16x16 -> 32 */
static inline uint32_t f3_mulu_w(f3_cpu *cpu, uint16_t src, uint16_t dst) {
    f3_cc_flush(cpu);
    uint32_t res = (uint32_t)src * (uint32_t)dst;
    uint16_t ccr = (cpu->sr & F3_CCR_X);
    if (res == 0) ccr |= F3_CCR_Z;
    if ((int32_t)res < 0) ccr |= F3_CCR_N;
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return res;
}

/* Signed Multiply 32x32 -> 32 or 64 */
static inline uint32_t f3_muls_l(f3_cpu *cpu, int32_t src, int32_t dst, uint32_t *hi_out) {
    f3_cc_flush(cpu);
    int64_t prod = (int64_t)src * (int64_t)dst;
    uint16_t ccr = (cpu->sr & F3_CCR_X);
    if (hi_out != NULL) {
        *hi_out = (uint32_t)((uint64_t)prod >> 32);
        if (prod == 0) ccr |= F3_CCR_Z;
        if (prod < 0) ccr |= F3_CCR_N;
    } else {
        int32_t lo = (int32_t)prod;
        if (lo == 0) ccr |= F3_CCR_Z;
        if (lo < 0) ccr |= F3_CCR_N;
        if (prod < -2147483648LL || prod > 2147483647LL) ccr |= F3_CCR_V;
    }
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return (uint32_t)prod;
}

/* Unsigned Multiply 32x32 -> 32 or 64 */
static inline uint32_t f3_mulu_l(f3_cpu *cpu, uint32_t src, uint32_t dst, uint32_t *hi_out) {
    f3_cc_flush(cpu);
    uint64_t prod = (uint64_t)src * (uint64_t)dst;
    uint16_t ccr = (cpu->sr & F3_CCR_X);
    if (hi_out != NULL) {
        *hi_out = (uint32_t)(prod >> 32);
        if (prod == 0) ccr |= F3_CCR_Z;
        if (prod & (1ULL << 63)) ccr |= F3_CCR_N;
    } else {
        uint32_t lo = (uint32_t)prod;
        if (lo == 0) ccr |= F3_CCR_Z;
        if (lo & 0x80000000u) ccr |= F3_CCR_N;
        if (prod > 0xffffffffULL) ccr |= F3_CCR_V;
    }
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return (uint32_t)prod;
}

/* Signed Divide 32 / 16 -> 16 quot, 16 rem */
static inline uint32_t f3_divs_w(f3_cpu *cpu, int16_t src, uint32_t dst, uint32_t pc, int *ex) {
    if (src == 0) {
        f3_cc_flush(cpu);
        f3_exception(cpu, 5, pc);
        *ex = 1;
        return dst;
    }
    *ex = 0;
    f3_cc_flush(cpu);
    /* Matches MAME's defined handling of the host INT32_MIN / -1 edge. */
    if (dst == 0x80000000u && src == -1) {
        cpu->sr = (cpu->sr & ~0x1fu) | (cpu->sr & F3_CCR_X) | F3_CCR_Z;
        return 0;
    }
    int32_t dividend = (int32_t)dst;
    int32_t divisor = (int32_t)src;
    int32_t quot = dividend / divisor;
    int32_t rem = dividend % divisor;
    if (quot < -32768 || quot > 32767) {
        cpu->sr = (cpu->sr & ~F3_CCR_C) | F3_CCR_V;
        return dst;
    }
    uint16_t ccr = (cpu->sr & F3_CCR_X);
    int16_t q16 = (int16_t)quot;
    if (q16 == 0) ccr |= F3_CCR_Z;
    if (q16 < 0) ccr |= F3_CCR_N;
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return (((uint32_t)(uint16_t)rem) << 16) | ((uint32_t)(uint16_t)q16);
}

/* Unsigned Divide 32 / 16 -> 16 quot, 16 rem */
static inline uint32_t f3_divu_w(f3_cpu *cpu, uint16_t src, uint32_t dst, uint32_t pc, int *ex) {
    if (src == 0) {
        f3_cc_flush(cpu);
        f3_exception(cpu, 5, pc);
        *ex = 1;
        return dst;
    }
    *ex = 0;
    f3_cc_flush(cpu);
    uint32_t dividend = dst;
    uint32_t divisor = (uint32_t)src;
    uint32_t quot = dividend / divisor;
    uint32_t rem = dividend % divisor;
    if (quot > 0xffffu) {
        cpu->sr = (cpu->sr & ~F3_CCR_C) | F3_CCR_V;
        return dst;
    }
    uint16_t ccr = (cpu->sr & F3_CCR_X);
    int16_t q16 = (int16_t)(uint16_t)quot;
    if (quot == 0) ccr |= F3_CCR_Z;
    if (q16 < 0) ccr |= F3_CCR_N;
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return (((uint32_t)(uint16_t)rem) << 16) | ((uint32_t)(uint16_t)quot);
}

/* Unsigned Divide 32/32 or 64/32 -> 32 */
static inline uint32_t f3_divu_l(f3_cpu *cpu, uint32_t src, uint32_t dst_lo, uint32_t dst_hi, int is_64, uint32_t pc, uint32_t *rem_out, int *ex) {
    if (src == 0) {
        f3_cc_flush(cpu);
        f3_exception(cpu, 5, pc);
        *ex = 1;
        return dst_lo;
    }
    *ex = 0;
    f3_cc_flush(cpu);
    uint64_t dividend = is_64 ? (((uint64_t)dst_hi << 32) | (uint64_t)dst_lo) : (uint64_t)dst_lo;
    uint64_t quot = dividend / (uint64_t)src;
    uint64_t rem = dividend % (uint64_t)src;
    if (quot > 0xffffffffULL) {
        cpu->sr |= F3_CCR_V;
        return dst_lo;
    }
    uint32_t q32 = (uint32_t)quot;
    if (rem_out != NULL) *rem_out = (uint32_t)rem;
    uint16_t ccr = (cpu->sr & F3_CCR_X);
    if (q32 == 0) ccr |= F3_CCR_Z;
    if (q32 & 0x80000000u) ccr |= F3_CCR_N;
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return q32;
}

/* Signed Divide 32/32 or 64/32 -> 32 */
static inline uint32_t f3_divs_l(f3_cpu *cpu, int32_t src, uint32_t dst_lo, uint32_t dst_hi, int is_64, uint32_t pc, uint32_t *rem_out, int *ex) {
    if (src == 0) {
        f3_cc_flush(cpu);
        f3_exception(cpu, 5, pc);
        *ex = 1;
        return dst_lo;
    }
    *ex = 0;
    f3_cc_flush(cpu);
    int64_t dividend = is_64 ? (int64_t)(((uint64_t)dst_hi << 32) | (uint64_t)dst_lo) : (int64_t)(int32_t)dst_lo;
    int64_t divisor = (int64_t)src;
    if (dividend == (-9223372036854775807LL - 1LL) && divisor == -1) {
        cpu->sr |= F3_CCR_V;
        return dst_lo;
    }
    int64_t quot = dividend / divisor;
    int64_t rem = dividend % divisor;
    if (is_64 && (quot < -2147483648LL || quot > 2147483647LL)) {
        cpu->sr |= F3_CCR_V;
        return dst_lo;
    }
    int32_t q32 = (int32_t)quot;
    if (rem_out != NULL) *rem_out = (uint32_t)(int32_t)rem;
    uint16_t ccr = (cpu->sr & F3_CCR_X);
    if (q32 == 0) ccr |= F3_CCR_Z;
    if (q32 < 0) ccr |= F3_CCR_N;
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return (uint32_t)q32;
}

/* Bit test helper */
static inline void f3_btst(f3_cpu *cpu, uint32_t val, uint32_t bit, uint8_t width) {
    f3_cc_flush(cpu);
    uint32_t mask = (width == 1) ? 7u : 31u;
    bit &= mask;
    int z = ((val >> bit) & 1u) == 0;
    cpu->sr = (cpu->sr & ~F3_CCR_Z) | (z ? F3_CCR_Z : 0);
}

/* Bit test and set */
static inline uint32_t f3_bset(f3_cpu *cpu, uint32_t val, uint32_t bit, uint8_t width) {
    f3_btst(cpu, val, bit, width);
    uint32_t mask = (width == 1) ? 7u : 31u;
    return val | (1u << (bit & mask));
}

/* Bit test and clear */
static inline uint32_t f3_bclr(f3_cpu *cpu, uint32_t val, uint32_t bit, uint8_t width) {
    f3_btst(cpu, val, bit, width);
    uint32_t mask = (width == 1) ? 7u : 31u;
    return val & ~(1u << (bit & mask));
}

/* Bit test and change */
static inline uint32_t f3_bchg(f3_cpu *cpu, uint32_t val, uint32_t bit, uint8_t width) {
    f3_btst(cpu, val, bit, width);
    uint32_t mask = (width == 1) ? 7u : 31u;
    return val ^ (1u << (bit & mask));
}

static inline uint64_t f3_idle_iterations(const f3_cpu *cpu, uint32_t cycles) {
    if (cpu->stopped || cpu->halted || (cpu->sr & 0xc000u) || cpu->cycles >= cpu->dispatch_deadline) return 0;
    return (cpu->dispatch_deadline - 1u - cpu->cycles) / cycles;
}

static inline void f3_idle_spin(f3_cpu *cpu, uint32_t cycles) {
    cpu->cycles += f3_idle_iterations(cpu, cycles) * cycles;
}

static inline uint64_t f3_idle_skippable(const f3_cpu *cpu, uint32_t cycles) {
    const uint64_t iterations = f3_idle_iterations(cpu, cycles);
    return iterations < 2 ? 0 : iterations - 1;
}

static inline uint16_t f3_tst_ccr(const uint8_t *bytes, unsigned size) {
    uint32_t value = 0;
    for (unsigned i = 0; i < size; ++i) value = (value << 8) | bytes[i];
    const uint32_t sign = 0x80u << (8 * (size - 1));
    return (value ? 0u : F3_CCR_Z) | ((value & sign) ? F3_CCR_N : 0u);
}

static inline void f3_idle_poll_bcc(f3_cpu *cpu, uint32_t address, unsigned size, unsigned cond, uint32_t cycles) {
    const uint8_t *p = f3_ram(cpu, address, size);
    if (!p) return;
    if (!f3_cond_ccr(f3_tst_ccr(p, size), (uint8_t)cond)) return;
    cpu->cycles += f3_idle_skippable(cpu, cycles) * cycles;
}

static inline void f3_idle_poll_dbcc(f3_cpu *cpu, uint32_t address, unsigned size, unsigned cond,
                                     unsigned counter, uint32_t cycles) {
    const uint8_t *p = f3_ram(cpu, address, size);
    if (!p) return;
    if (f3_cond_ccr(f3_tst_ccr(p, size), (uint8_t)cond)) return;
    uint64_t whole = f3_idle_skippable(cpu, cycles);
    if (!whole) return;
    const uint32_t count = cpu->d[counter] & 0xffffu;
    if (whole > count) whole = count;
    cpu->d[counter] = (cpu->d[counter] & 0xffff0000u) | (count - (uint32_t)whole);
    cpu->cycles += whole * cycles;
}

static inline uint32_t f3_affine_power(uint32_t x, uint32_t multiplier, uint32_t addend, uint64_t n) {
    uint32_t result_multiplier = 1u, result_addend = 0u;
    for (; n; n >>= 1) {
        if (n & 1u) {
            result_addend = multiplier * result_addend + addend;
            result_multiplier *= multiplier;
        }
        addend = multiplier * addend + addend;
        multiplier *= multiplier;
    }
    return result_multiplier * x + result_addend;
}

static inline void f3_idle_affine_call(f3_cpu *cpu, uint32_t address, uint32_t stack, uint32_t multiplier,
                                       uint32_t addend, uint32_t cycles) {
    uint8_t *word = f3_ram(cpu, address, 4);
    const uint8_t *frame = f3_ram(cpu, cpu->a[7] - stack, stack);
    if (!word || !frame || (word < frame + stack && frame < word + 4)) return;
    uint64_t whole = f3_idle_skippable(cpu, cycles);
    if (!whole) return;
    uint32_t x = (uint32_t)word[0] << 24 | (uint32_t)word[1] << 16 | (uint32_t)word[2] << 8 | word[3];
    x = f3_affine_power(x, multiplier, addend, whole);
    word[0] = (uint8_t)(x >> 24);
    word[1] = (uint8_t)(x >> 16);
    word[2] = (uint8_t)(x >> 8);
    word[3] = (uint8_t)x;
    cpu->cycles += whole * cycles;
}
#if defined(F3_PROFILE_INSTRUMENT) && F3_PROFILE_INSTRUMENT
#  define F3_IDLE_SPIN(cpu, cycles) ((void)0)
#  define F3_IDLE_POLL_BCC(cpu, address, size, cond, cycles) ((void)0)
#  define F3_IDLE_POLL_DBCC(cpu, address, size, cond, counter, cycles) ((void)0)
#  define F3_IDLE_AFFINE_CALL(cpu, address, stack, multiplier, addend, cycles) ((void)0)
#else
#  define F3_IDLE_SPIN(cpu, cycles) f3_idle_spin((cpu), (cycles))
#  define F3_IDLE_POLL_BCC(cpu, address, size, cond, cycles) \
      f3_idle_poll_bcc((cpu), (address), (size), (cond), (cycles))
#  define F3_IDLE_POLL_DBCC(cpu, address, size, cond, counter, cycles) \
      f3_idle_poll_dbcc((cpu), (address), (size), (cond), (counter), (cycles))
#  define F3_IDLE_AFFINE_CALL(cpu, address, stack, multiplier, addend, cycles) \
      f3_idle_affine_call((cpu), (address), (stack), (multiplier), (addend), (cycles))
#endif

#include "bitfield.h"

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_CPU_OPS_H */
