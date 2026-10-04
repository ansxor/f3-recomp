/* Musashi state bridge. Core is vendored under its MIT-style license. */
#include "f3rt/cpu_abi.h"
#include "m68kcpu.h"

void f3rt_core_import(const f3_cpu *cpu) {
    unsigned i;
    m68k_set_reg(M68K_REG_SR, cpu->sr);
    m68k_set_reg(M68K_REG_USP, cpu->usp);
    m68k_set_reg(M68K_REG_ISP, cpu->ssp);
    m68k_set_reg(M68K_REG_MSP, cpu->msp);
    for (i = 0; i < 8; ++i) {
        m68k_set_reg((m68k_register_t)(M68K_REG_D0 + i), cpu->d[i]);
        m68k_set_reg((m68k_register_t)(M68K_REG_A0 + i), cpu->a[i]);
    }
    m68k_set_reg(M68K_REG_PC, cpu->pc);
    m68k_set_reg(M68K_REG_VBR, cpu->vbr);
    m68k_set_reg(M68K_REG_SFC, cpu->sfc);
    m68k_set_reg(M68K_REG_DFC, cpu->dfc);
    m68k_set_reg(M68K_REG_CACR, cpu->cacr);
    m68k_set_reg(M68K_REG_CAAR, cpu->caar);
    m68ki_cpu.stopped = (cpu->stopped ? STOP_LEVEL_STOP : 0) | (cpu->halted ? STOP_LEVEL_HALT : 0);
    /* A fallback is one instruction, not an outstanding power-on reset delay. */
    m68ki_cpu.reset_cycles = 0;
}
void f3rt_core_export(f3_cpu *cpu) {
    unsigned i;
    const uint16_t sr = (uint16_t)m68k_get_reg(0, M68K_REG_SR);
    if ((sr & 0x0700) < (cpu->sr & 0x0700)) cpu->dispatch_deadline = 0;
    for (i = 0; i < 8; ++i) {
        cpu->d[i] = m68k_get_reg(0, (m68k_register_t)(M68K_REG_D0 + i));
        cpu->a[i] = m68k_get_reg(0, (m68k_register_t)(M68K_REG_A0 + i));
    }
    cpu->pc = m68k_get_reg(0, M68K_REG_PC);
    cpu->sr = sr;
    cpu->usp = m68k_get_reg(0, M68K_REG_USP);
    cpu->ssp = m68k_get_reg(0, M68K_REG_ISP);
    cpu->msp = m68k_get_reg(0, M68K_REG_MSP);
    cpu->vbr = m68k_get_reg(0, M68K_REG_VBR);
    cpu->sfc = m68k_get_reg(0, M68K_REG_SFC);
    cpu->dfc = m68k_get_reg(0, M68K_REG_DFC);
    cpu->cacr = m68k_get_reg(0, M68K_REG_CACR);
    cpu->caar = m68k_get_reg(0, M68K_REG_CAAR);
    cpu->stopped = (m68ki_cpu.stopped & STOP_LEVEL_STOP) != 0;
    cpu->halted = (m68ki_cpu.stopped & STOP_LEVEL_HALT) != 0;
    cpu->cc_op = 0;
}
