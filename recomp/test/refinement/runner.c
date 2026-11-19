#include <inttypes.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <f3rt/cpu_abi.h>
#include "cpu_ops.h"

typedef struct {
    uint32_t address, value;
    unsigned size;
} Write;

typedef struct {
    Write writes[256];
    size_t count;
    uint32_t seed;
    unsigned resets;
    bool raised;
} Test_bus;

static uint32_t random_word(uint32_t *state) {
    *state = *state * 1664525u + 1013904223u;
    return *state ^ (*state >> 16);
}

uint8_t f3_read8(f3_cpu *cpu, uint32_t address) {
    Test_bus *bus = cpu->runtime;
    for (size_t i = bus->count; i > 0; --i) {
        const Write *write = &bus->writes[i - 1];
        uint32_t offset = address - write->address;
        if (offset < write->size) {
            return (uint8_t)(write->value >> (8 * (write->size - 1 - offset)));
        }
    }
    uint32_t seed = bus->seed ^ address;
    return (uint8_t)random_word(&seed);
}

uint16_t f3_read16(f3_cpu *cpu, uint32_t address) {
    return (uint16_t)((uint32_t)f3_read8(cpu, address) << 8 | f3_read8(cpu, address + 1));
}

uint32_t f3_read32(f3_cpu *cpu, uint32_t address) {
    return (uint32_t)f3_read16(cpu, address) << 16 | f3_read16(cpu, address + 2);
}

static void write_bus(f3_cpu *cpu, uint32_t address, uint32_t value, unsigned size) {
    Test_bus *bus = cpu->runtime;
    if (bus->count == sizeof(bus->writes) / sizeof(bus->writes[0])) {
        fputs("refinement write log exhausted\n", stderr);
        exit(2);
    }
    bus->writes[bus->count++] = (Write){address, value, size};
}

void f3_write8(f3_cpu *cpu, uint32_t address, uint8_t value) {
    write_bus(cpu, address, value, 1);
}

void f3_write16(f3_cpu *cpu, uint32_t address, uint16_t value) {
    write_bus(cpu, address, value, 2);
}

void f3_write32(f3_cpu *cpu, uint32_t address, uint32_t value) {
    write_bus(cpu, address, value, 4);
}

static uint32_t *stack(f3_cpu *cpu, uint16_t sr) {
    if (!(sr & 0x2000u)) return &cpu->usp;
    return sr & 0x1000u ? &cpu->msp : &cpu->ssp;
}

void f3_set_sr(f3_cpu *cpu, uint16_t sr) {
    uint32_t *previous = stack(cpu, cpu->sr);
    uint32_t *next = stack(cpu, sr);
    if (previous != next) {
        *previous = cpu->a[7];
        cpu->a[7] = *next;
    }
    cpu->sr = sr & 0xf71fu;
    cpu->cc_op = F3_CC_OP_NONE;
}

/* Record the exception inputs and canonical SR on the same ordered test bus. */
void f3_exception(f3_cpu *cpu, unsigned vector, uint32_t return_pc) {
    Test_bus *bus = cpu->runtime;
    bus->raised = true;
    cpu->a[7] -= 8;
    f3_write16(cpu, cpu->a[7], cpu->sr);
    f3_write32(cpu, cpu->a[7] + 2, return_pc);
    f3_write16(cpu, cpu->a[7] + 6, (uint16_t)(vector * 4));
    cpu->pc = f3_read32(cpu, cpu->vbr + vector * 4);
}

void f3_reset_devices(f3_cpu *cpu) {
    Test_bus *bus = cpu->runtime;
    ++bus->resets;
}

static f3_cpu initial_cpu(uint32_t *rng, uint32_t pc, unsigned sample) {
    f3_cpu cpu = {0};
    cpu.pc = pc;
    for (size_t reg = 0; reg < 8; ++reg) {
        cpu.d[reg] = random_word(rng);
        cpu.a[reg] = random_word(rng) & ~1u;
    }
    cpu.usp = random_word(rng) & ~1u;
    cpu.ssp = random_word(rng) & ~1u;
    cpu.msp = random_word(rng) & ~1u;
    cpu.sr = (uint16_t)(random_word(rng) & 0x371fu);
    cpu.cc_op = sample % (F3_CC_OP_CMP + 1);
    cpu.cc_width = 1u << ((sample / (F3_CC_OP_CMP + 1)) % 3);
    uint32_t mask = UINT32_MAX >> (32 - 8 * cpu.cc_width);
    cpu.cc_src = random_word(rng) & mask;
    cpu.cc_dst = random_word(rng) & mask;
    if (cpu.cc_op == F3_CC_OP_ADD) {
        cpu.cc_result = (cpu.cc_dst + cpu.cc_src) & mask;
    } else if (cpu.cc_op == F3_CC_OP_LOGIC) {
        cpu.cc_result = random_word(rng) & mask;
    } else {
        cpu.cc_result = (cpu.cc_dst - cpu.cc_src) & mask;
    }
    if (cpu.cc_op == F3_CC_OP_ADD || cpu.cc_op == F3_CC_OP_SUB) {
        /* Copy the real pending X while leaving the other SR bits arbitrary. */
        f3_cpu materialised = cpu;
        f3_cc_flush(&materialised);
        cpu.sr = (cpu.sr & ~F3_CCR_X) | (materialised.sr & F3_CCR_X);
    }
    return cpu;
}

static bool same_writes(const Test_bus *eager, const Test_bus *lazy) {
    if (eager->count != lazy->count) return false;
    for (size_t i = 0; i < eager->count; ++i) {
        const Write *a = &eager->writes[i];
        const Write *b = &lazy->writes[i];
        if (a->address != b->address || a->value != b->value || a->size != b->size) {
            printf("write %zu differs: eager %u:%08" PRIx32 "=%08" PRIx32
                   ", lazy %u:%08" PRIx32 "=%08" PRIx32 "\n",
                   i, a->size, a->address, a->value, b->size, b->address, b->value);
            return false;
        }
    }
    return true;
}

static int compare(void (*eager)(f3_cpu *), void (*lazy)(f3_cpu *), uint32_t *rng,
                   const uint32_t *entries, size_t count, unsigned samples) {
    for (size_t i = 0; i < count; ++i) {
        for (unsigned sample = 0; sample < samples; ++sample) {
            uint32_t seed = *rng;
            f3_cpu a = initial_cpu(rng, entries[i], sample), b = a;
            Test_bus bus_a = {.seed = random_word(rng)}, bus_b = bus_a;
            a.runtime = &bus_a;
            b.runtime = &bus_b;
            eager(&a);
            lazy(&b);
            f3_cc_flush(&a);
            f3_cc_flush(&b);
            if (memcmp(a.d, b.d, sizeof(a.d)) || memcmp(a.a, b.a, sizeof(a.a)) ||
                a.pc != b.pc || a.sr != b.sr || a.usp != b.usp || a.ssp != b.ssp ||
                a.msp != b.msp || a.steps != b.steps || a.stopped != b.stopped ||
                a.halted != b.halted || a.vbr != b.vbr || a.sfc != b.sfc ||
                a.dfc != b.dfc || a.cacr != b.cacr || a.caar != b.caar ||
                bus_a.raised != bus_b.raised || bus_a.resets != bus_b.resets ||
                !same_writes(&bus_a, &bus_b)) {
                printf("entry %#" PRIx32 ", sample %u, RNG %#" PRIx32
                       ": eager PC=%#" PRIx32 " SR=%#x writes=%zu"
                       ", lazy PC=%#" PRIx32 " SR=%#x writes=%zu\n",
                       entries[i], sample, seed, a.pc, a.sr, bus_a.count, b.pc, b.sr, bus_b.count);
                for (size_t reg = 0; reg < 8; ++reg) {
                    if (a.d[reg] != b.d[reg]) {
                        printf("D%zu: eager=%#" PRIx32 ", lazy=%#" PRIx32 "\n", reg, a.d[reg], b.d[reg]);
                    }
                    if (a.a[reg] != b.a[reg]) {
                        printf("A%zu: eager=%#" PRIx32 ", lazy=%#" PRIx32 "\n", reg, a.a[reg], b.a[reg]);
                    }
                }
                return 1;
            }
        }
    }
    return 0;
}
