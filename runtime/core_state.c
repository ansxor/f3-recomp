/* Musashi state bridge. Core is vendored under its MIT-style license. */
#include "f3rt/cpu_abi.h"
#include "m68kcpu.h"
#include "m68kops.h"
#include "state_oracle.h"

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

void f3rt_sound_core_export(const void *context, f3rt_sound_oracle_state *dst) {
    const m68ki_cpu_core *core = (const m68ki_cpu_core *)context;
    int i;
    dst->cpu_type = core->cpu_type;
    for (i = 0; i < 16; ++i) {
        dst->dar[i] = core->dar[i];
        dst->dar_save[i] = core->dar_save[i];
    }
    dst->ppc = core->ppc;
    dst->pc = core->pc;
    for (i = 0; i < 7; ++i) dst->sp[i] = core->sp[i];
    dst->vbr = core->vbr;
    dst->sfc = core->sfc;
    dst->dfc = core->dfc;
    dst->cacr = core->cacr;
    dst->caar = core->caar;
    dst->ir = core->ir;
    dst->t1_flag = core->t1_flag;
    dst->t0_flag = core->t0_flag;
    dst->s_flag = core->s_flag;
    dst->m_flag = core->m_flag;
    dst->x_flag = core->x_flag;
    dst->n_flag = core->n_flag;
    dst->not_z_flag = core->not_z_flag;
    dst->v_flag = core->v_flag;
    dst->c_flag = core->c_flag;
    dst->int_mask = core->int_mask;
    dst->int_level = core->int_level;
    dst->stopped = core->stopped;
    dst->pref_addr = core->pref_addr;
    dst->pref_data = core->pref_data;
    dst->address_mask = core->address_mask;
    dst->sr_mask = core->sr_mask;
    dst->instr_mode = core->instr_mode;
    dst->run_mode = core->run_mode;
    dst->has_pmmu = core->has_pmmu;
    dst->pmmu_enabled = core->pmmu_enabled;
    dst->fpu_just_reset = core->fpu_just_reset;
    dst->reset_cycles = core->reset_cycles;
    dst->cyc_bcc_notake_b = core->cyc_bcc_notake_b;
    dst->cyc_bcc_notake_w = core->cyc_bcc_notake_w;
    dst->cyc_dbcc_f_noexp = core->cyc_dbcc_f_noexp;
    dst->cyc_dbcc_f_exp = core->cyc_dbcc_f_exp;
    dst->cyc_scc_r_true = core->cyc_scc_r_true;
    dst->cyc_movem_w = core->cyc_movem_w;
    dst->cyc_movem_l = core->cyc_movem_l;
    dst->cyc_movem_store_w = core->cyc_movem_store_w;
    dst->cyc_movem_store_l = core->cyc_movem_store_l;
    dst->cyc_shift = core->cyc_shift;
    dst->cyc_reset = core->cyc_reset;
    dst->virq_state = core->virq_state;
    dst->nmi_pending = core->nmi_pending;
    dst->mmu_crp_aptr = core->mmu_crp_aptr;
    dst->mmu_crp_limit = core->mmu_crp_limit;
    dst->mmu_srp_aptr = core->mmu_srp_aptr;
    dst->mmu_srp_limit = core->mmu_srp_limit;
    dst->mmu_tc = core->mmu_tc;
    dst->mmu_sr = core->mmu_sr;
}

void f3rt_sound_core_import(void *context, const f3rt_sound_oracle_state *src) {
    m68ki_cpu_core *core = (m68ki_cpu_core *)context;
    int i;
    core->cpu_type = src->cpu_type;
    for (i = 0; i < 16; ++i) {
        core->dar[i] = src->dar[i];
        core->dar_save[i] = src->dar_save[i];
    }
    core->ppc = src->ppc;
    core->pc = src->pc;
    for (i = 0; i < 7; ++i) core->sp[i] = src->sp[i];
    core->vbr = src->vbr;
    core->sfc = src->sfc;
    core->dfc = src->dfc;
    core->cacr = src->cacr;
    core->caar = src->caar;
    core->ir = src->ir;
    core->t1_flag = src->t1_flag;
    core->t0_flag = src->t0_flag;
    core->s_flag = src->s_flag;
    core->m_flag = src->m_flag;
    core->x_flag = src->x_flag;
    core->n_flag = src->n_flag;
    core->not_z_flag = src->not_z_flag;
    core->v_flag = src->v_flag;
    core->c_flag = src->c_flag;
    core->int_mask = src->int_mask;
    core->int_level = src->int_level;
    core->stopped = src->stopped;
    core->pref_addr = src->pref_addr;
    core->pref_data = src->pref_data;
    core->address_mask = src->address_mask;
    core->sr_mask = src->sr_mask;
    core->instr_mode = src->instr_mode;
    core->run_mode = src->run_mode;
    core->has_pmmu = src->has_pmmu;
    core->pmmu_enabled = src->pmmu_enabled;
    core->fpu_just_reset = src->fpu_just_reset;
    core->reset_cycles = src->reset_cycles;
    core->cyc_bcc_notake_b = src->cyc_bcc_notake_b;
    core->cyc_bcc_notake_w = src->cyc_bcc_notake_w;
    core->cyc_dbcc_f_noexp = src->cyc_dbcc_f_noexp;
    core->cyc_dbcc_f_exp = src->cyc_dbcc_f_exp;
    core->cyc_scc_r_true = src->cyc_scc_r_true;
    core->cyc_movem_w = src->cyc_movem_w;
    core->cyc_movem_l = src->cyc_movem_l;
    core->cyc_movem_store_w = src->cyc_movem_store_w;
    core->cyc_movem_store_l = src->cyc_movem_store_l;
    core->cyc_shift = src->cyc_shift;
    core->cyc_reset = src->cyc_reset;
    core->virq_state = src->virq_state;
    core->nmi_pending = src->nmi_pending;
    core->mmu_crp_aptr = src->mmu_crp_aptr;
    core->mmu_crp_limit = src->mmu_crp_limit;
    core->mmu_srp_aptr = src->mmu_srp_aptr;
    core->mmu_srp_limit = src->mmu_srp_limit;
    core->mmu_tc = src->mmu_tc;
    core->mmu_sr = src->mmu_sr;

    if (src->cpu_type != 0 || !src->sound_needs_reset) {
        core->cyc_instruction = m68ki_cycles[0];
        core->cyc_exception = m68ki_exception_cycle_table[0];
    }
}
