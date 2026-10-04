#include "harness_abi.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static DiffEnv *s_active_env = NULL;
static int s_musashi_initialized = 0;
static int s_musashi_insn_count = 0;
static void *s_pristine_context;

void diff_set_active_env(DiffEnv *env) {
    s_active_env = env;
}

DiffEnv *diff_get_active_env(void) {
    return s_active_env;
}

void diff_env_init(DiffEnv *env) {
    if (!env) return;
    memset(env->pages, 0, sizeof(env->pages));
    env->num_touched_pages = 0;
    env->num_writes = 0;
    env->exception_taken = 0;
    env->exception_vector = 0;
    env->exception_pc = 0;
    env->halted = 0;
}

void diff_env_reset(DiffEnv *env) {
    if (!env) return;
    for (size_t i = 0; i < env->num_touched_pages; i++) {
        uint16_t page_idx = env->touched_pages[i];
        if (env->pages[page_idx]) {
            memset(env->pages[page_idx], 0, DIFF_PAGE_SIZE);
        }
    }
    env->num_touched_pages = 0;
    env->num_writes = 0;
    env->exception_taken = 0;
    env->exception_vector = 0;
    env->exception_pc = 0;
    env->halted = 0;
}

void diff_env_free(DiffEnv *env) {
    if (!env) return;
    for (size_t i = 0; i < DIFF_NUM_PAGES; i++) {
        if (env->pages[i]) {
            free(env->pages[i]);
            env->pages[i] = NULL;
        }
    }
    env->num_touched_pages = 0;
    env->num_writes = 0;
}

static uint8_t *get_or_alloc_page(DiffEnv *env, uint16_t page_idx) {
    if (!env->pages[page_idx]) {
        uint8_t *page = (uint8_t *)calloc(1, DIFF_PAGE_SIZE);
        if (!page) {
            fprintf(stderr, "FATAL: out of memory allocating diff test page %u\n", page_idx);
            exit(1);
        }
        env->pages[page_idx] = page;
    }
    /* Record touched page if not already recorded */
    bool found = false;
    for (size_t i = 0; i < env->num_touched_pages; i++) {
        if (env->touched_pages[i] == page_idx) {
            found = true;
            break;
        }
    }
    if (!found && env->num_touched_pages < MAX_TOUCHED_PAGES) {
        env->touched_pages[env->num_touched_pages++] = page_idx;
    }
    return env->pages[page_idx];
}

void diff_env_write_byte(DiffEnv *env, uint32_t address, uint8_t val, bool log) {
    if (!env) return;
    address &= 0x00ffffffu;
    uint16_t page_idx = (uint16_t)(address >> 16);
    uint16_t page_off = (uint16_t)(address & 0xFFFF);
    uint8_t *page = get_or_alloc_page(env, page_idx);
    page[page_off] = val;

    if (log && env->num_writes < MAX_WRITES_PER_CASE) {
        env->writes[env->num_writes].address = address;
        env->writes[env->num_writes].value = val;
        env->writes[env->num_writes].size = 1;
        env->num_writes++;
    }
}

void diff_env_write_bytes(DiffEnv *env, uint32_t address, const uint8_t *data, size_t size) {
    for (size_t i = 0; i < size; i++) {
        diff_env_write_byte(env, address + (uint32_t)i, data[i], false);
    }
}

uint8_t diff_env_read_byte(DiffEnv *env, uint32_t address) {
    if (!env) return 0;
    address &= 0x00ffffffu;
    uint16_t page_idx = (uint16_t)(address >> 16);
    uint16_t page_off = (uint16_t)(address & 0xFFFF);
    if (!env->pages[page_idx]) return 0;
    return env->pages[page_idx][page_off];
}

/* ======================================================================== */
/* F3 Recomp ABI Implementation                                             */
/* ======================================================================== */

uint8_t f3_read8(f3_cpu *cpu, uint32_t address) {
    DiffEnv *env = cpu && cpu->runtime ? (DiffEnv *)cpu->runtime : s_active_env;
    return diff_env_read_byte(env, address);
}

uint16_t f3_read16(f3_cpu *cpu, uint32_t address) {
    uint16_t b0 = f3_read8(cpu, address);
    uint16_t b1 = f3_read8(cpu, address + 1);
    return (uint16_t)((b0 << 8) | b1);
}

uint32_t f3_read32(f3_cpu *cpu, uint32_t address) {
    uint32_t w0 = f3_read16(cpu, address);
    uint32_t w1 = f3_read16(cpu, address + 2);
    return (w0 << 16) | w1;
}

void f3_write8(f3_cpu *cpu, uint32_t address, uint8_t value) {
    DiffEnv *env = cpu && cpu->runtime ? (DiffEnv *)cpu->runtime : s_active_env;
    diff_env_write_byte(env, address, value, true);
}

void f3_write16(f3_cpu *cpu, uint32_t address, uint16_t value) {
    DiffEnv *env = cpu && cpu->runtime ? (DiffEnv *)cpu->runtime : s_active_env;
    address &= 0x00ffffffu;
    diff_env_write_byte(env, address, (uint8_t)(value >> 8), false);
    diff_env_write_byte(env, address + 1, (uint8_t)(value & 0xFF), false);
    if (env && env->num_writes < MAX_WRITES_PER_CASE) {
        env->writes[env->num_writes].address = address;
        env->writes[env->num_writes].value = value;
        env->writes[env->num_writes].size = 2;
        env->num_writes++;
    }
}

void f3_write32(f3_cpu *cpu, uint32_t address, uint32_t value) {
    DiffEnv *env = cpu && cpu->runtime ? (DiffEnv *)cpu->runtime : s_active_env;
    address &= 0x00ffffffu;
    diff_env_write_byte(env, address, (uint8_t)(value >> 24), false);
    diff_env_write_byte(env, address + 1, (uint8_t)((value >> 16) & 0xFF), false);
    diff_env_write_byte(env, address + 2, (uint8_t)((value >> 8) & 0xFF), false);
    diff_env_write_byte(env, address + 3, (uint8_t)(value & 0xFF), false);
    if (env && env->num_writes < MAX_WRITES_PER_CASE) {
        env->writes[env->num_writes].address = address;
        env->writes[env->num_writes].value = value;
        env->writes[env->num_writes].size = 4;
        env->num_writes++;
    }
}

void f3_exception(f3_cpu *cpu, unsigned vector, uint32_t return_pc) {
    DiffEnv *env = (DiffEnv *)cpu->runtime;
    env->exception_taken = 1;
    env->exception_vector = vector;
    env->exception_pc = return_pc;
    uint16_t old_sr = cpu->sr;
    f3_set_sr(cpu, (uint16_t)((old_sr | 0x2000u) & 0x3fffu));
    cpu->stopped = 0;
    unsigned format = 0;
    if (vector == 5 || vector == 6 || vector == 7 || vector == 9) {
        cpu->a[7] -= 4;
        f3_write32(cpu, cpu->a[7], cpu->pc);
        format = 0x2000;
    }
    cpu->a[7] -= 2;
    f3_write16(cpu, cpu->a[7], (uint16_t)(format | (vector * 4)));
    cpu->a[7] -= 4;
    f3_write32(cpu, cpu->a[7], return_pc);
    cpu->a[7] -= 2;
    f3_write16(cpu, cpu->a[7], old_sr);
    cpu->pc = f3_read32(cpu, cpu->vbr + vector * 4);
}

void f3_set_sr(f3_cpu *cpu, uint16_t sr) {
    uint32_t *old_stack = !(cpu->sr & 0x2000) ? &cpu->usp :
        (cpu->sr & 0x1000) ? &cpu->msp : &cpu->ssp;
    uint32_t *new_stack = !(sr & 0x2000) ? &cpu->usp :
        (sr & 0x1000) ? &cpu->msp : &cpu->ssp;
    if (old_stack != new_stack) {
        *old_stack = cpu->a[7];
        cpu->a[7] = *new_stack;
    }
    cpu->sr = sr & 0xf71fu;
    cpu->cc_op = 0;
}

void f3_reset_devices(f3_cpu *cpu) {
    (void)cpu;
    fprintf(stderr, "RESET device effects require the runtime integration harness\n");
    exit(1);
}

/* ======================================================================== */
/* Musashi Reference Memory Callbacks                                       */
/* ======================================================================== */

unsigned int m68k_read_memory_8(unsigned int address) {
    return diff_env_read_byte(s_active_env, (uint32_t)address);
}

unsigned int m68k_read_memory_16(unsigned int address) {
    unsigned int b0 = diff_env_read_byte(s_active_env, (uint32_t)address);
    unsigned int b1 = diff_env_read_byte(s_active_env, (uint32_t)(address + 1));
    return (b0 << 8) | b1;
}

unsigned int m68k_read_memory_32(unsigned int address) {
    unsigned int w0 = m68k_read_memory_16(address);
    unsigned int w1 = m68k_read_memory_16(address + 2);
    return (w0 << 16) | w1;
}

unsigned int m68k_read_immediate_16(unsigned int address) {
    return m68k_read_memory_16(address);
}

unsigned int m68k_read_immediate_32(unsigned int address) {
    return m68k_read_memory_32(address);
}

unsigned int m68k_read_pcrelative_8(unsigned int address) {
    return m68k_read_memory_8(address);
}

unsigned int m68k_read_pcrelative_16(unsigned int address) {
    return m68k_read_memory_16(address);
}

unsigned int m68k_read_pcrelative_32(unsigned int address) {
    return m68k_read_memory_32(address);
}

unsigned int m68k_read_disassembler_8(unsigned int address) {
    return m68k_read_memory_8(address);
}

unsigned int m68k_read_disassembler_16(unsigned int address) {
    return m68k_read_memory_16(address);
}

unsigned int m68k_read_disassembler_32(unsigned int address) {
    return m68k_read_memory_32(address);
}

void m68k_write_memory_8(unsigned int address, unsigned int value) {
    diff_env_write_byte(s_active_env, (uint32_t)address, (uint8_t)value, true);
}

void m68k_write_memory_16(unsigned int address, unsigned int value) {
    address &= 0x00ffffffu;
    diff_env_write_byte(s_active_env, (uint32_t)address, (uint8_t)(value >> 8), false);
    diff_env_write_byte(s_active_env, (uint32_t)(address + 1), (uint8_t)(value & 0xFF), false);
    if (s_active_env && s_active_env->num_writes < MAX_WRITES_PER_CASE) {
        s_active_env->writes[s_active_env->num_writes].address = (uint32_t)address;
        s_active_env->writes[s_active_env->num_writes].value = value & 0xFFFF;
        s_active_env->writes[s_active_env->num_writes].size = 2;
        s_active_env->num_writes++;
    }
}

void m68k_write_memory_32(unsigned int address, unsigned int value) {
    address &= 0x00ffffffu;
    diff_env_write_byte(s_active_env, (uint32_t)address, (uint8_t)(value >> 24), false);
    diff_env_write_byte(s_active_env, (uint32_t)(address + 1), (uint8_t)((value >> 16) & 0xFF), false);
    diff_env_write_byte(s_active_env, (uint32_t)(address + 2), (uint8_t)((value >> 8) & 0xFF), false);
    diff_env_write_byte(s_active_env, (uint32_t)(address + 3), (uint8_t)(value & 0xFF), false);
    if (s_active_env && s_active_env->num_writes < MAX_WRITES_PER_CASE) {
        s_active_env->writes[s_active_env->num_writes].address = (uint32_t)address;
        s_active_env->writes[s_active_env->num_writes].value = value;
        s_active_env->writes[s_active_env->num_writes].size = 4;
        s_active_env->num_writes++;
    }
}

void m68k_write_memory_32_pd(unsigned int address, unsigned int value) {
    m68k_write_memory_32(address, value);
}

/* ======================================================================== */
/* Musashi Execution Setup & Execution                                      */
/* ======================================================================== */

static void musashi_instr_hook(unsigned int pc) {
    (void)pc;
    if (s_musashi_insn_count > 0) {
        m68k_end_timeslice();
    }
    s_musashi_insn_count++;
}

void diff_musashi_setup(void) {
    if (!s_musashi_initialized) {
        m68k_init();
        m68k_set_cpu_type(M68K_CPU_TYPE_68EC020);
        m68k_set_instr_hook_callback(musashi_instr_hook);

        /* Drain reset cycle debt safely with dummy reset vectors */
        DiffEnv setup_env;
        diff_env_init(&setup_env);
        diff_set_active_env(&setup_env);

        /* Vectors: SP=0x00028000 at 0, PC=0x00010000 at 4 */
        uint8_t vectors[8] = { 0x00, 0x02, 0x80, 0x00, 0x00, 0x01, 0x00, 0x00 };
        diff_env_write_bytes(&setup_env, 0, vectors, 8);
        /* Put NOP (0x4E71) at 0x00010000 */
        uint8_t nop[2] = { 0x4E, 0x71 };
        diff_env_write_bytes(&setup_env, 0x00010000, nop, 2);

        m68k_pulse_reset();
        /* Drain all reset cycles without running code */
        m68k_execute(1);

        /* Smoke verification: run exactly 1 NOP and assert PC advances to 0x00010002 */
        m68k_set_reg(M68K_REG_PC, 0x00010000);
        s_musashi_insn_count = 0;
        m68k_execute(1);
        uint32_t smoke_pc = m68k_get_reg(NULL, M68K_REG_PC);
        if (s_musashi_insn_count != 1 || smoke_pc != 0x00010002) {
            fprintf(stderr, "FATAL: Musashi smoke test failed! insn_count=%d, pc=0x%08X (expected 1, 0x00010002)\n",
                    s_musashi_insn_count, smoke_pc);
            exit(1);
        }

        diff_env_free(&setup_env);
        s_pristine_context = malloc(m68k_context_size());
        if (!s_pristine_context) abort();
        m68k_get_context(s_pristine_context);
        s_musashi_initialized = 1;
    }
}

void diff_musashi_run_case(const TestCase *tc, DiffEnv *env,
                           uint32_t out_d[8], uint32_t out_a[8],
                           uint32_t *out_pc, uint16_t *out_sr) {
    diff_set_active_env(env);
    diff_env_reset(env);

    /* Write test initial memory */
    for (size_t i = 0; i < tc->num_mem_init; i++) {
        diff_env_write_bytes(env, tc->initial_mem[i].address,
                             tc->initial_mem[i].data, tc->initial_mem[i].size);
    }
    /* Write instruction opcode bytes at test_pc */
    diff_env_write_bytes(env, tc->initial_pc, tc->code_bytes, tc->code_size);

    /* Set up Musashi registers */
    m68k_set_context(s_pristine_context);
    m68k_set_reg(M68K_REG_SR, tc->initial_sr);
    m68k_set_reg(M68K_REG_USP, 0);
    m68k_set_reg(M68K_REG_ISP, 0);
    m68k_set_reg(M68K_REG_MSP, 0);
    for (int i = 0; i < 8; i++) {
        m68k_set_reg(M68K_REG_D0 + i, tc->initial_d[i]);
        m68k_set_reg(M68K_REG_A0 + i, tc->initial_a[i]);
    }
    m68k_set_reg(M68K_REG_PC, tc->initial_pc);

    /* Each positive one-cycle slice executes one complete instruction. */
    for (unsigned step = 0; step < tc->instruction_count; ++step) {
        s_musashi_insn_count = 0;
        m68k_execute(1);
        if (s_musashi_insn_count != 1) {
            fprintf(stderr, "FATAL: Musashi did not step one instruction in '%s': count=%d\n",
                    tc->name, s_musashi_insn_count);
            exit(1);
        }
    }

    /* Harvest results */
    for (int i = 0; i < 8; i++) {
        out_d[i] = m68k_get_reg(NULL, M68K_REG_D0 + i);
        out_a[i] = m68k_get_reg(NULL, M68K_REG_A0 + i);
    }
    *out_pc = m68k_get_reg(NULL, M68K_REG_PC);
    *out_sr = (uint16_t)m68k_get_reg(NULL, M68K_REG_SR);
}

/* ======================================================================== */
/* Recomp Execution                                                         */
/* ======================================================================== */

void diff_recomp_run_case(const TestCase *tc, DiffEnv *env, f3_cpu *out_cpu) {
    diff_set_active_env(env);
    diff_env_reset(env);

    /* Write test initial memory */
    for (size_t i = 0; i < tc->num_mem_init; i++) {
        diff_env_write_bytes(env, tc->initial_mem[i].address,
                             tc->initial_mem[i].data, tc->initial_mem[i].size);
    }
    /* Write instruction opcode bytes at test_pc */
    diff_env_write_bytes(env, tc->initial_pc, tc->code_bytes, tc->code_size);

    /* Set up Recomp CPU */
    memset(out_cpu, 0, sizeof(*out_cpu));
    memcpy(out_cpu->d, tc->initial_d, sizeof(out_cpu->d));
    memcpy(out_cpu->a, tc->initial_a, sizeof(out_cpu->a));
    out_cpu->pc = tc->initial_pc;
    out_cpu->sr = tc->initial_sr;
    out_cpu->runtime = env;

    /* Execute the lowered C statements */
    if (tc->recomp_fn) {
        tc->recomp_fn(out_cpu);
    }
}

/* ======================================================================== */
/* State Comparison & Delta Reporting                                       */
/* ======================================================================== */

static void format_sr_flags(char *buf, size_t maxlen, uint16_t sr) {
    snprintf(buf, maxlen, "0x%04X [X=%d N=%d Z=%d V=%d C=%d]",
             sr,
             (sr >> 4) & 1,
             (sr >> 3) & 1,
             (sr >> 2) & 1,
             (sr >> 1) & 1,
             sr & 1);
}

bool diff_compare_states(const TestCase *tc,
                         const DiffEnv *musashi_env, const uint32_t musashi_d[8],
                         const uint32_t musashi_a[8], uint32_t musashi_pc, uint16_t musashi_sr,
                         const DiffEnv *recomp_env, const f3_cpu *recomp_cpu,
                         StateDelta *delta) {
    (void)tc;
    memset(delta, 0, sizeof(*delta));
    memcpy(delta->musashi_d, musashi_d, sizeof(delta->musashi_d));
    memcpy(delta->musashi_a, musashi_a, sizeof(delta->musashi_a));
    delta->musashi_pc = musashi_pc;
    delta->musashi_sr = musashi_sr;
    delta->musashi_writes = musashi_env->num_writes;

    memcpy(delta->recomp_d, recomp_cpu->d, sizeof(delta->recomp_d));
    memcpy(delta->recomp_a, recomp_cpu->a, sizeof(delta->recomp_a));
    delta->recomp_pc = recomp_cpu->pc;
    delta->recomp_sr = recomp_cpu->sr;
    delta->recomp_writes = recomp_env->num_writes;

    char *desc = delta->description;
    size_t rem = sizeof(delta->description);
    int written = 0;

    /* 1. Compare Data Registers D0-D7 */
    for (int i = 0; i < 8; i++) {
        if (musashi_d[i] != recomp_cpu->d[i]) {
            delta->has_mismatch = true;
            written = snprintf(desc, rem,
                               "  D%d: expected 0x%08X (Musashi), got 0x%08X (Recomp) [diff 0x%08X]\n",
                               i, musashi_d[i], recomp_cpu->d[i], musashi_d[i] ^ recomp_cpu->d[i]);
            if (written > 0 && (size_t)written < rem) { desc += written; rem -= (size_t)written; }
        }
    }

    /* 2. Compare Address Registers A0-A7 */
    for (int i = 0; i < 8; i++) {
        if (musashi_a[i] != recomp_cpu->a[i]) {
            delta->has_mismatch = true;
            written = snprintf(desc, rem,
                               "  A%d: expected 0x%08X (Musashi), got 0x%08X (Recomp) [diff 0x%08X]\n",
                               i, musashi_a[i], recomp_cpu->a[i], musashi_a[i] ^ recomp_cpu->a[i]);
            if (written > 0 && (size_t)written < rem) { desc += written; rem -= (size_t)written; }
        }
    }

    /* 3. Compare Program Counter */
    if (musashi_pc != recomp_cpu->pc) {
        delta->has_mismatch = true;
        written = snprintf(desc, rem,
                           "  PC: expected 0x%08X (Musashi), got 0x%08X (Recomp)\n",
                           musashi_pc, recomp_cpu->pc);
        if (written > 0 && (size_t)written < rem) { desc += written; rem -= (size_t)written; }
    }

    /* 4. Compare Canonical Status Register (SR) */
    /* 68020: T1, T0, S, M, interrupt mask, and all five condition codes. */
    const uint16_t compare_mask = 0xf71fu;

    if ((musashi_sr & compare_mask) != (recomp_cpu->sr & compare_mask)) {
        delta->has_mismatch = true;
        char m_str[64], r_str[64];
        format_sr_flags(m_str, sizeof(m_str), musashi_sr & compare_mask);
        format_sr_flags(r_str, sizeof(r_str), recomp_cpu->sr & compare_mask);
        written = snprintf(desc, rem,
                           "  SR: expected %s (Musashi)\n"
                           "      got      %s (Recomp)\n",
                           m_str, r_str);
        if (written > 0 && (size_t)written < rem) { desc += written; rem -= (size_t)written; }

        /* Detail individual flag differences */
        static const char *flag_names[5] = { "C", "V", "Z", "N", "X" };
        for (int bit = 4; bit >= 0; bit--) {
            int m_bit = (musashi_sr >> bit) & 1;
            int r_bit = (recomp_cpu->sr >> bit) & 1;
            if (m_bit != r_bit) {
                written = snprintf(desc, rem,
                                   "      flag %s mismatch: Musashi=%d, Recomp=%d\n",
                                   flag_names[bit], m_bit, r_bit);
                if (written > 0 && (size_t)written < rem) { desc += written; rem -= (size_t)written; }
            }
        }
    }

    /* 5. Compare Memory Writes */
    if (musashi_env->num_writes != recomp_env->num_writes) {
        delta->has_mismatch = true;
        written = snprintf(desc, rem,
                           "  Write count mismatch: Musashi wrote %zu times, Recomp wrote %zu times\n",
                           musashi_env->num_writes, recomp_env->num_writes);
        if (written > 0 && (size_t)written < rem) { desc += written; rem -= (size_t)written; }
    } else {
        for (size_t w = 0; w < musashi_env->num_writes; w++) {
            const MemWrite *mw = &musashi_env->writes[w];
            const MemWrite *rw = &recomp_env->writes[w];
            if (mw->address != rw->address || mw->size != rw->size || mw->value != rw->value) {
                delta->has_mismatch = true;
                written = snprintf(desc, rem,
                                   "  Write #%zu mismatch: Musashi wrote (0x%08X, sz=%u, val=0x%X) vs Recomp (0x%08X, sz=%u, val=0x%X)\n",
                                   w, mw->address, mw->size, mw->value, rw->address, rw->size, rw->value);
                if (written > 0 && (size_t)written < rem) { desc += written; rem -= (size_t)written; }
                break;
            }
        }
    }

    /* Compare the union of touched pages, including one-sided allocations. */
    for (unsigned p = 0; p < DIFF_NUM_PAGES; ++p) {
        const uint8_t *mp = musashi_env->pages[p];
        const uint8_t *rp = recomp_env->pages[p];
        if (!mp && !rp) continue;
        if (mp && rp && memcmp(mp, rp, DIFF_PAGE_SIZE) == 0) continue;
        for (size_t off = 0; off < DIFF_PAGE_SIZE; ++off) {
            uint8_t mb = mp ? mp[off] : 0;
            uint8_t rb = rp ? rp[off] : 0;
            if (mb != rb) {
                delta->has_mismatch = true;
                written = snprintf(desc, rem,
                    "  Memory byte mismatch at 0x%08X: Musashi=0x%02X, Recomp=0x%02X\n",
                    (p << 16) | (uint32_t)off, mb, rb);
                if (written > 0 && (size_t)written < rem) { desc += written; rem -= (size_t)written; }
                break;
            }
        }
    }

    return !delta->has_mismatch;
}
