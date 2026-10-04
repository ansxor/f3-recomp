#ifndef RECOMP_CPU_OPS_H
#define RECOMP_CPU_OPS_H

#include <stdint.h>
#include <f3rt/cpu_abi.h>

#ifdef __cplusplus
extern "C" {
#endif

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

/* Evaluate a 68000/68020 4-bit condition against canonicalized CCR. */
static inline int f3_eval_cond(f3_cpu *cpu, uint8_t cond) {
    f3_cc_flush(cpu);
    uint16_t sr = cpu->sr;
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

#include "bitfield.h"

#ifdef __cplusplus
}
#endif

#endif /* RECOMP_CPU_OPS_H */
