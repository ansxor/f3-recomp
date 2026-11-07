#include <assert.h>
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "recomp/cpu_ops.h"

typedef struct {
    uint8_t ram[0x20000] __attribute__((aligned(8)));
    uint32_t writer[0x10000];
    uint8_t rom[0x1000];
    uint64_t device_hash;
    int track_writers;
} bus_t;
static bus_t buses[2];
static f3_cpu cpus[2];
#define ROM_SIZE 0xff0u

static bus_t *bus_of(f3_cpu *cpu) { return &buses[cpu - cpus]; }
static int ram_address(uint32_t a) { return (a & 0xfc0000u) == 0x400000u; }
static uint8_t rd8(f3_cpu *cpu, uint32_t a) {
    bus_t *bus = bus_of(cpu);
    a &= 0xffffffu;
    if (ram_address(a)) return bus->ram[a & 0x1ffffu];
    if (a < ROM_SIZE) return bus->rom[a];
    bus->device_hash = bus->device_hash * 31 + a;
    return (uint8_t)(a * 7 + 3);
}
static void wr8(f3_cpu *cpu, uint32_t a, uint8_t v) {
    bus_t *bus = bus_of(cpu);
    a &= 0xffffffu;
    if (ram_address(a)) {
        bus->ram[a & 0x1ffffu] = v;
        if (bus->track_writers) bus->writer[(a & 0x1ffffu) >> 1] = cpu->pc;
        return;
    }
    bus->device_hash = (bus->device_hash * 31 + a) * 31 + v;
}
uint8_t f3_read8(f3_cpu *cpu, uint32_t a) { return rd8(cpu, a); }
uint16_t f3_read16(f3_cpu *cpu, uint32_t a) { return (uint16_t)(rd8(cpu, a) << 8 | rd8(cpu, a + 1)); }
uint32_t f3_read32(f3_cpu *cpu, uint32_t a) { return (uint32_t)f3_read16(cpu, a) << 16 | f3_read16(cpu, a + 2); }
void f3_write8(f3_cpu *cpu, uint32_t a, uint8_t v) { wr8(cpu, a, v); }
void f3_write16(f3_cpu *cpu, uint32_t a, uint16_t v) { wr8(cpu, a, (uint8_t)(v >> 8)); wr8(cpu, a + 1, (uint8_t)v); }
void f3_write32(f3_cpu *cpu, uint32_t a, uint32_t v) { f3_write16(cpu, a, (uint16_t)(v >> 16)); f3_write16(cpu, a + 2, (uint16_t)v); }

uint8_t *f3_ram(f3_cpu *cpu, uint32_t a, uint32_t size) { (void)cpu; (void)a; (void)size; return NULL; }

static uint64_t state = 0x2545f4914f6cdd1dull;
static uint32_t random32(void) {
    state ^= state << 13; state ^= state >> 7; state ^= state << 17;
    return (uint32_t)(state >> 20);
}
static uint32_t random_address(void) {
    static const uint32_t bases[] = {0x400000u, 0x420000u, 0x43fff0u, 0x43ff00u, 0x43fffcu, 0x440000u, 0x3ffff0u,
                                     0x0u, 0xff0u - 8u, 0x1000u, 0x4a0000u, 0x600000u, 0xffffffu};
    uint32_t a = bases[random32() % (sizeof bases / sizeof *bases)] + (random32() % 24);
    if (random32() & 1) a = 0x400000u + (random32() & 0x7ffffu);
    if (random32() % 5 == 0) a = random32() & 0x1fffu;
    if (random32() % 7 == 0) a |= (random32() & 0xffu) << 24;       /* the bus ignores the top byte */
    return a;
}

int main(void) {
    for (int i = 0; i < 2; ++i) {
        for (size_t j = 0; j < sizeof buses[i].rom; ++j) buses[i].rom[j] = (uint8_t)(j * 13 + 5);
        buses[i].track_writers = 1;
    }
    cpus[0].ram = buses[0].ram;
    cpus[0].ram_writer = buses[0].writer;
    cpus[0].rom = buses[0].rom;
    cpus[0].rom_size = ROM_SIZE;
    for (unsigned round = 0; round < 400000; ++round) {
        const uint32_t a = random_address(), v = random32() * 2654435761u;
        const uint32_t pc = random32() & 0xfffffeu;
        cpus[0].pc = cpus[1].pc = pc;
        if (round == 200000) { /* attribution off: the stores must stay silent in the table */
            buses[0].track_writers = buses[1].track_writers = 0;
            cpus[0].ram_writer = NULL;
        }
        switch (random32() % 6) {
        case 0: assert(f3_ld8(&cpus[0], a) == f3_read8(&cpus[1], a)); break;
        case 1: assert(f3_ld16(&cpus[0], a) == f3_read16(&cpus[1], a)); break;
        case 2: assert(f3_ld32(&cpus[0], a) == f3_read32(&cpus[1], a)); break;
        case 3: f3_st8(&cpus[0], a, (uint8_t)v); f3_write8(&cpus[1], a, (uint8_t)v); break;
        case 4: f3_st16(&cpus[0], a, (uint16_t)v); f3_write16(&cpus[1], a, (uint16_t)v); break;
        default: f3_st32(&cpus[0], a, v); f3_write32(&cpus[1], a, v); break;
        }
    }
    assert(memcmp(buses[0].ram, buses[1].ram, sizeof buses[0].ram) == 0);
    assert(memcmp(buses[0].writer, buses[1].writer, sizeof buses[0].writer) == 0);
    assert(buses[0].device_hash == buses[1].device_hash);
    f3_cpu bare = {0};
    cpus[0] = bare;
    assert(f3_ld32(&cpus[0], 0x400010u) == f3_read32(&cpus[1], 0x400010u));
    printf("ok\n");
    return 0;
}
