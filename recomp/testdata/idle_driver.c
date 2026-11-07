#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>
#include "program.h"
#include "recomp/cpu_ops.h"

uint64_t *f3_profile_main_counts;
uint64_t *f3_profile_sound_counts;
int f3_profile_indirect_enabled;
void f3_profile_indirect(uint32_t site, uint32_t target) { (void)site; (void)target; }
void f3_profile_cold_abort(unsigned region, uint32_t rom_crc, uint32_t address) {
    (void)region; (void)rom_crc; (void)address; assert(0);
}

#define RAM_BASE 0x400000u
static uint8_t ram[0x1000];
static uint32_t offset(uint32_t a) { assert(a - RAM_BASE < sizeof ram); return a - RAM_BASE; }
static uint64_t reads;
uint8_t f3_read8(f3_cpu *cpu, uint32_t a) { (void)cpu; ++reads; return ram[offset(a)]; }
uint16_t f3_read16(f3_cpu *cpu, uint32_t a) { return (uint16_t)(f3_read8(cpu, a) << 8 | f3_read8(cpu, a + 1)); }
uint32_t f3_read32(f3_cpu *cpu, uint32_t a) { return (uint32_t)f3_read16(cpu, a) << 16 | f3_read16(cpu, a + 2); }
void f3_write8(f3_cpu *cpu, uint32_t a, uint8_t v) { (void)cpu; ram[offset(a)] = v; }
void f3_write16(f3_cpu *cpu, uint32_t a, uint16_t v) { f3_write8(cpu, a, (uint8_t)(v >> 8)); f3_write8(cpu, a + 1, (uint8_t)v); }
void f3_write32(f3_cpu *cpu, uint32_t a, uint32_t v) { f3_write16(cpu, a, (uint16_t)(v >> 16)); f3_write16(cpu, a + 2, (uint16_t)v); }
uint8_t *f3_ram(f3_cpu *cpu, uint32_t a, uint32_t size) {
    (void)cpu;
    a -= RAM_BASE;
    return a < sizeof ram && size <= sizeof ram - a ? ram + a : NULL;
}
int f3_validate_main_rom(f3_cpu *cpu, size_t size, uint32_t crc) { (void)cpu; (void)size; (void)crc; return 1; }
static const f3_block *blocks;
static size_t block_count;
int f3_register_blocks(f3_cpu *cpu, const f3_block *table, size_t count) {
    (void)cpu; blocks = table; block_count = count; return 1;
}
int f3_register_exclusions(f3_cpu *cpu, const f3_excluded_range *ranges, size_t count) {
    (void)cpu; (void)ranges; (void)count; return 1;
}
static int dispatch(f3_cpu *cpu) {
    for (size_t i = 0; i < block_count; ++i)
        if (blocks[i].address == cpu->pc) { blocks[i].execute(cpu); return 1; }
    return 0;
}
static uint64_t state = 0x9e3779b97f4a7c15ull;
static uint32_t random32(void) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return (uint32_t)(state >> 16);
}
static uint32_t crc(const void *data, size_t size) {
    const uint8_t *p = data;
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < size; ++i) h = (h ^ p[i]) * 16777619u;
    return h;
}

int main(void) {
    f3_cpu base = {0};
    assert(f3_generated_register(&base));
    for (uint32_t n = 0; n < 300; ++n) {
        uint32_t x = 0x12345678u + n, expected = x;
        for (uint32_t i = 0; i < n; ++i) expected = 7u * expected + 5u;
        assert(f3_affine_power(x, 7u, 5u, n) == expected);
    }
    static const uint32_t heads[] = {0x400, 0x410, 0x420, 0x440};
    for (unsigned round = 0; round < 6000; ++round) {
        f3_cpu cpu = base;
        for (unsigned i = 0; i < sizeof ram; ++i) ram[i] = (uint8_t)random32();
        const uint32_t head = heads[round % 4];
        cpu.a[5] = RAM_BASE + 0x100;
        cpu.a[7] = RAM_BASE + 0x800 + 2 * (random32() & 0x3f);
        cpu.pc = head;
        cpu.d[0] = random32() & 3 ? random32() & 0x1ff : random32();
        for (int i = 1; i < 8; ++i) cpu.d[i] = random32();
        cpu.sr = (uint16_t)(0x2000 | (random32() & 0x1f));
        cpu.cc_op = (uint8_t)(random32() % 5);
        cpu.cc_src = random32(); cpu.cc_dst = random32(); cpu.cc_result = random32();
        cpu.cc_width = (uint8_t)(1u << (random32() % 3)); cpu.cc_mask = (uint8_t)random32();
        cpu.cycles = random32() & 0xffff;
        if (random32() & 3) { ram[0x110] = 0; ram[0x120] = ram[0x121] = 0; }
        if (random32() % 7 == 0) ram[0x110] = (uint8_t)random32();
        const uint64_t budget = random32() % (round % 3 == 0 ? 150 : 4000) + 1;
        cpu.dispatch_deadline = cpu.cycles + budget;
        while (cpu.cycles < cpu.dispatch_deadline && dispatch(&cpu)) {}
        printf("%u pc=%" PRIx32 " cyc=%" PRIu64 " sr=%" PRIx16 " op=%u width=%u src=%" PRIx32 " dst=%" PRIx32
               " res=%" PRIx32 " mask=%u a7=%" PRIx32 " d=%" PRIx32 ",%" PRIx32 ",%" PRIx32 " ram=%" PRIx32 "\n",
               round, cpu.pc, cpu.cycles, cpu.sr, cpu.cc_op, cpu.cc_width, cpu.cc_src, cpu.cc_dst, cpu.cc_result,
               cpu.cc_mask, cpu.a[7], cpu.d[0], cpu.d[1], cpu.d[2], crc(ram, sizeof ram));
    }
    fprintf(stderr, "%" PRIu64 "\n", reads);
    return 0;
}
