#include <inttypes.h>
#include <stdio.h>
#include "driver.h"
#include "instrument.h"
#include "recomp/cpu_ops.h"

uint64_t *f3_profile_main_counts;
uint64_t *f3_profile_sound_counts;

#define RAM_BASE 0x400000u
#define RAM_SIZE 0x1000u

uint8_t *f3_ram(f3_cpu *cpu, uint32_t address, uint32_t size) {
    (void)cpu;
    uint32_t offset = address - RAM_BASE;
    return offset < RAM_SIZE && size <= RAM_SIZE - offset ? memory + offset : NULL;
}

static int idle;
void f3_idle(f3_cpu *cpu) {
    idle = 1;
    cpu->dispatch_deadline = 0;
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

/* With hooks (YIELD=1) a round runs until its budget or an idle yield and prints
 * the step count it stopped at; without hooks a round runs to exactly that count,
 * read back from stdin. Both print the same state line. */
int main(void) {
    f3_cpu base = {0};
    assert(f3_generated_register(&base));
    static const uint32_t heads[] = {0x400, 0x410, 0x420, 0x440};
    unsigned yields = 0;
    for (unsigned round = 0; round < 6000; ++round) {
        f3_cpu cpu = base;
        for (unsigned i = 0; i < RAM_SIZE; ++i) memory[i] = (uint8_t)random32();
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
        cpu.steps = random32() & 0xffff;
        if (random32() & 3) { memory[0x110] = 0; memory[0x120] = memory[0x121] = 0; }
        if (random32() % 7 == 0) memory[0x110] = (uint8_t)random32();
        const uint64_t start = cpu.steps;
        (void)start;
        uint64_t deadline = start + random32() % (round % 3 == 0 ? 40 : 1000) + 1;
#if YIELD
        idle = 0;
        cpu.dispatch_deadline = deadline;
        while (!idle && cpu.steps < cpu.dispatch_deadline && dispatch(&cpu)) {}
        if (idle) ++yields;
        if (head == 0x400) assert(idle && cpu.steps - start <= 1); /* bra * waits at once */
        printf("%" PRIu64 " ", cpu.steps);
#else
        assert(scanf("%" SCNu64, &deadline) == 1);
        cpu.dispatch_deadline = deadline;
        while (cpu.steps < cpu.dispatch_deadline && dispatch(&cpu)) {}
        printf("%" PRIu64 " ", cpu.steps);
#endif
        printf("%u pc=%" PRIx32 " sr=%" PRIx16 " op=%u width=%u src=%" PRIx32 " dst=%" PRIx32
               " res=%" PRIx32 " mask=%u a7=%" PRIx32 " d=%" PRIx32 ",%" PRIx32 ",%" PRIx32 " ram=%" PRIx32 "\n",
               round, cpu.pc, cpu.sr, cpu.cc_op, cpu.cc_width, cpu.cc_src, cpu.cc_dst, cpu.cc_result,
               cpu.cc_mask, cpu.a[7], cpu.d[0], cpu.d[1], cpu.d[2], crc(memory, RAM_SIZE));
    }
#if YIELD
    assert(yields > 6000 / 4);
#else
    (void)yields;
#endif
    return 0;
}
