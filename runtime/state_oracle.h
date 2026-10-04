#ifndef F3RT_STATE_ORACLE_H
#define F3RT_STATE_ORACLE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#pragma pack(push, 1)
typedef struct f3rt_sound_oracle_state {
    uint32_t cpu_type;
    uint32_t dar[16];
    uint32_t dar_save[16];
    uint32_t ppc;
    uint32_t pc;
    uint32_t sp[7];
    uint32_t vbr;
    uint32_t sfc;
    uint32_t dfc;
    uint32_t cacr;
    uint32_t caar;
    uint32_t ir;
    uint32_t t1_flag;
    uint32_t t0_flag;
    uint32_t s_flag;
    uint32_t m_flag;
    uint32_t x_flag;
    uint32_t n_flag;
    uint32_t not_z_flag;
    uint32_t v_flag;
    uint32_t c_flag;
    uint32_t int_mask;
    uint32_t int_level;
    uint32_t stopped;
    uint32_t pref_addr;
    uint32_t pref_data;
    uint32_t address_mask;
    uint32_t sr_mask;
    uint32_t instr_mode;
    uint32_t run_mode;
    int32_t  has_pmmu;
    int32_t  pmmu_enabled;
    int32_t  fpu_just_reset;
    uint32_t reset_cycles;
    uint32_t cyc_bcc_notake_b;
    uint32_t cyc_bcc_notake_w;
    uint32_t cyc_dbcc_f_noexp;
    uint32_t cyc_dbcc_f_exp;
    uint32_t cyc_scc_r_true;
    uint32_t cyc_movem_w;
    uint32_t cyc_movem_l;
    uint32_t cyc_movem_store_w;
    uint32_t cyc_movem_store_l;
    uint32_t cyc_shift;
    uint32_t cyc_reset;
    uint32_t virq_state;
    uint32_t nmi_pending;
    uint32_t mmu_crp_aptr, mmu_crp_limit;
    uint32_t mmu_srp_aptr, mmu_srp_limit;
    uint32_t mmu_tc;
    uint16_t mmu_sr;
    uint8_t sound_needs_reset;
} f3rt_sound_oracle_state;
#pragma pack(pop)

void f3rt_sound_core_export(const void *context, f3rt_sound_oracle_state *dst);
void f3rt_sound_core_import(void *context, const f3rt_sound_oracle_state *src);

#ifdef __cplusplus
}
#endif

#endif
