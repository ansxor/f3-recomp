#ifndef F3RT_SOUND_NATIVE_OPS_H
#define F3RT_SOUND_NATIVE_OPS_H

#include <stddef.h>
#include <stdint.h>
#include <f3rt/cpu_abi.h>
#include "recomp/cpu_ops.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Sound-specific memory bus operations (dispatched via SoundNative) */
uint8_t f3_sound_read8(f3_cpu *cpu, uint32_t address);
uint16_t f3_sound_read16(f3_cpu *cpu, uint32_t address);
uint32_t f3_sound_read32(f3_cpu *cpu, uint32_t address);
void f3_sound_write8(f3_cpu *cpu, uint32_t address, uint8_t value);
void f3_sound_write16(f3_cpu *cpu, uint32_t address, uint16_t value);
void f3_sound_write32(f3_cpu *cpu, uint32_t address, uint32_t value);

/* Sound CPU control and exception operations */
void f3_sound_set_sr(f3_cpu *cpu, uint16_t sr);
void f3_sound_exception(f3_cpu *cpu, unsigned vector, uint32_t return_pc);
void f3_sound_rte(f3_cpu *cpu);
void f3_sound_stop(f3_cpu *cpu, uint16_t new_sr, uint32_t next_pc);
void f3_sound_unsupported_pc(f3_cpu *cpu, uint32_t pc);

/* Mathematical condition code aliases */
#define f3_sound_cc_flush f3_cc_flush
#define f3_sound_eval_cond f3_eval_cond
#define f3_sound_lsl f3_lsl
#define f3_sound_lsr f3_lsr
#define f3_sound_asl f3_asl
#define f3_sound_asr f3_asr
#define f3_sound_rol f3_rol
#define f3_sound_ror f3_ror
#define f3_sound_roxl f3_roxl
#define f3_sound_roxr f3_roxr
#define f3_sound_btst f3_btst
#define f3_sound_bset f3_bset
#define f3_sound_bclr f3_bclr
#define f3_sound_bchg f3_bchg

#define f3_sound_mulu_w f3_mulu_w
#define f3_sound_muls_w f3_muls_w

static inline uint32_t f3_sound_divs_w(f3_cpu *cpu, int16_t src, uint32_t dst, uint32_t pc, int *ex) {
    if (src == 0) {
        f3_sound_cc_flush(cpu);
        f3_sound_exception(cpu, 5, pc);
        *ex = 1;
        return dst;
    }
    *ex = 0;
    f3_sound_cc_flush(cpu);
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

static inline uint32_t f3_sound_divu_w(f3_cpu *cpu, uint16_t src, uint32_t dst, uint32_t pc, int *ex) {
    if (src == 0) {
        f3_sound_cc_flush(cpu);
        f3_sound_exception(cpu, 5, pc);
        *ex = 1;
        return dst;
    }
    *ex = 0;
    f3_sound_cc_flush(cpu);
    uint32_t dividend = dst;
    uint32_t divisor = (uint32_t)src;
    uint32_t quot = dividend / divisor;
    uint32_t rem = dividend % divisor;
    if (quot > 0xffffu) {
        cpu->sr = (cpu->sr & ~F3_CCR_C) | F3_CCR_V;
        return dst;
    }
    uint16_t ccr = (cpu->sr & F3_CCR_X);
    uint16_t q16 = (uint16_t)quot;
    if (q16 == 0) ccr |= F3_CCR_Z;
    if (q16 & 0x8000u) ccr |= F3_CCR_N;
    cpu->sr = (cpu->sr & ~0x1fu) | ccr;
    return (rem << 16) | (uint32_t)q16;
}
/* 68000 dynamic cycle cost helpers */
static inline uint32_t f3_sound_mulu_cycles(uint16_t src) {
    return 2u * (uint32_t)__builtin_popcount((unsigned int)src);
}

static inline uint32_t f3_sound_muls_cycles(uint16_t src) {
    uint32_t c = (src && !(src & 0x8000u)) ? 2u : 0u;
    for (uint32_t y = src, f = 0u; y; y >>= 1) {
        if ((y & 1u) != f) {
            c += 2u;
            f = 1u - f;
        }
    }
    return c;
}

static inline uint32_t f3_sound_divu_cycles(uint32_t dividend, uint16_t divisor) {
    if (divisor == 0) return 0;
    uint32_t cycles = 76u;
    uint32_t shifted = ((uint32_t)divisor) << 16;
    if ((dividend >> 16) >= divisor) return 10u;
    for (uint32_t bit = 0; bit < 15; bit++) {
        uint32_t carry = dividend & 0x80000000u;
        dividend <<= 1;
        if (carry) {
            dividend -= shifted;
        } else if (dividend >= shifted) {
            dividend -= shifted;
            cycles += 2u;
        } else {
            cycles += 4u;
        }
    }
    return cycles;
}

#ifdef __cplusplus
}
#endif

#endif /* F3RT_SOUND_NATIVE_OPS_H */
