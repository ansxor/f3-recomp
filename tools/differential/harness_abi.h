#ifndef HARNESS_ABI_H
#define HARNESS_ABI_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <f3rt/cpu_abi.h>
#include "m68k.h"

#ifdef __cplusplus
extern "C" {
#endif

#define DIFF_PAGE_SIZE 65536u
#define DIFF_NUM_PAGES 256u
#define MAX_TOUCHED_PAGES 4096
#define MAX_WRITES_PER_CASE 512

typedef struct MemWrite {
    uint32_t address;
    uint32_t value;
    uint8_t size; /* 1, 2, or 4 */
} MemWrite;

typedef struct DiffEnv {
    /* Sparse 64KiB pages covering the 68EC020's 24-bit physical address bus. */
    uint8_t *pages[DIFF_NUM_PAGES];
    uint16_t touched_pages[MAX_TOUCHED_PAGES];
    size_t num_touched_pages;

    /* Log of memory writes during this instruction */
    MemWrite writes[MAX_WRITES_PER_CASE];
    size_t num_writes;
    uint64_t cycles;

    /* Exception/halt recording */
    uint8_t exception_taken;
    unsigned exception_vector;
    uint32_t exception_pc;
    uint8_t halted;
} DiffEnv;

typedef struct MemInit {
    uint32_t address;
    const uint8_t *data;
    size_t size;
} MemInit;

typedef struct TestCase {
    uint32_t id;
    const char *name;
    const char *mnemonic;
    const char *op_str;
    const uint8_t *code_bytes;
    size_t code_size;
    unsigned instruction_count;
    uint32_t initial_pc;
    uint32_t initial_d[8];
    uint32_t initial_a[8];
    uint16_t initial_sr;
    const MemInit *initial_mem;
    size_t num_mem_init;
    void (*recomp_fn)(f3_cpu *cpu);
    int is_boundary;
    const char *boundary_kind;
    uint64_t seed;
} TestCase;

typedef struct StateDelta {
    bool has_mismatch;
    char description[2048];
    uint32_t musashi_d[8], recomp_d[8];
    uint32_t musashi_a[8], recomp_a[8];
    uint32_t musashi_pc, recomp_pc;
    uint16_t musashi_sr, recomp_sr;
    size_t musashi_writes, recomp_writes;
} StateDelta;

/* Environment management */
void diff_env_init(DiffEnv *env);
void diff_env_reset(DiffEnv *env);
void diff_env_free(DiffEnv *env);
void diff_env_write_byte(DiffEnv *env, uint32_t address, uint8_t val, bool log);
void diff_env_write_bytes(DiffEnv *env, uint32_t address, const uint8_t *data, size_t size);
uint8_t diff_env_read_byte(DiffEnv *env, uint32_t address);

/* Set the currently active environment for memory read/write callbacks */
void diff_set_active_env(DiffEnv *env);
DiffEnv *diff_get_active_env(void);

/* Musashi execution harness */
void diff_musashi_setup(void);
void diff_musashi_run_case(const TestCase *tc, DiffEnv *env,
                           uint32_t out_d[8], uint32_t out_a[8],
                           uint32_t *out_pc, uint16_t *out_sr);

/* Recomp execution harness */
void diff_recomp_run_case(const TestCase *tc, DiffEnv *env, f3_cpu *out_cpu);

/* Compare outcomes and format detailed delta report if any mismatch */
bool diff_compare_states(const TestCase *tc,
                         const DiffEnv *musashi_env, const uint32_t musashi_d[8],
                         const uint32_t musashi_a[8], uint32_t musashi_pc, uint16_t musashi_sr,
                         const DiffEnv *recomp_env, const f3_cpu *recomp_cpu,
                         StateDelta *delta);

#ifdef __cplusplus
}
#endif

#endif /* HARNESS_ABI_H */
