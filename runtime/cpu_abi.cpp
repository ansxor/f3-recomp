#include "f3rt/cpu_abi.h"
#include "f3rt/machine.hpp"
#include <algorithm>

namespace {
f3rt::Machine &machine(f3_cpu *cpu) { return *static_cast<f3rt::Machine *>(cpu->runtime); }
f3rt::Machine &bus(f3_cpu *cpu, uint32_t address, uint32_t width) {
    auto &m = machine(cpu);
    const uint32_t start = address & 0xffffff;
    const uint32_t end = start + width;
    // Native blocks can run ahead of devices. Sound must finish preceding
    // time before a mailbox access or reset-line change becomes visible.
    // Include unaligned accesses crossing into a mapped range. Only advance
    // devices here: IRQ entry still belongs to the next instruction boundary.
    if (end > 0xc00000 && (start < 0xc00800 ||
        (start < 0xc80004 && end > 0xc80000) ||
        (start < 0xc80104 && end > 0xc80100)))
        m.advance_to(cpu->cycles);
    return m;
}
uint32_t &stack(f3_cpu *cpu, uint16_t sr) {
    return !(sr & 0x2000) ? cpu->usp : (sr & 0x1000) ? cpu->msp : cpu->ssp;
}
}
extern "C" {
uint8_t f3_read8(f3_cpu *cpu, uint32_t a) { return bus(cpu,a,1).read8(a); }
uint16_t f3_read16(f3_cpu *cpu, uint32_t a) { return bus(cpu,a,2).read16(a); }
uint32_t f3_read32(f3_cpu *cpu, uint32_t a) { return bus(cpu,a,4).read32(a); }
void f3_write8(f3_cpu *cpu, uint32_t a, uint8_t v) { bus(cpu,a,1).write8(a,v); }
void f3_write16(f3_cpu *cpu, uint32_t a, uint16_t v) { bus(cpu,a,2).write16(a,v); }
void f3_write32(f3_cpu *cpu, uint32_t a, uint32_t v) { bus(cpu,a,4).write32(a,v); }
void f3_set_sr(f3_cpu *cpu, uint16_t sr) {
    sr &= 0xf71f; // 68EC020 writable status bits.
    if ((sr & 0x0700) < (cpu->sr & 0x0700)) cpu->dispatch_deadline = 0;
    auto &old_stack = stack(cpu, cpu->sr);
    auto &new_stack = stack(cpu, sr);
    if (&old_stack != &new_stack) { old_stack = cpu->a[7]; cpu->a[7] = new_stack; }
    cpu->sr = sr;
    cpu->cc_op = 0;
}
void f3_exception(f3_cpu *cpu, unsigned vector, uint32_t return_pc) {
    if (vector > 255) { cpu->halted = 1; return; }
    const uint16_t old_sr = cpu->sr;
    const uint32_t instruction_pc = cpu->pc;
    const bool format2 = vector == 5 || vector == 6 || vector == 7 || vector == 9;
    f3_set_sr(cpu, uint16_t((old_sr | 0x2000) & ~0xc000));
    cpu->stopped = 0;
    cpu->a[7] -= format2 ? 12 : 8;
    f3_write16(cpu, cpu->a[7], old_sr);
    f3_write32(cpu, cpu->a[7] + 2, return_pc);
    f3_write16(cpu, cpu->a[7] + 6, uint16_t((format2 ? 0x2000 : 0) | (vector * 4)));
    if (format2) f3_write32(cpu, cpu->a[7] + 8, instruction_pc);
    cpu->pc = f3_read32(cpu, cpu->vbr + vector * 4);
    // Full exception charge, matching the pinned 68EC020 timing model.
    // Generated instructions must not add their normal base charge on this path.
    static constexpr uint8_t system_cycles[16] = {
        4, 4, 50, 50, 20, 38, 40, 20, 34, 25, 20, 20, 4, 4, 4, 30
    };
    cpu->cycles += vector < 16 ? system_cycles[vector]
        : vector >= 24 && vector < 32 ? 30
        : vector >= 32 && vector < 48 ? 24 : 4;
}
void f3_reset_devices(f3_cpu *cpu) {
    auto &m = machine(cpu);
    m.advance_to(cpu->cycles);
    m.reset_devices();
}
int f3_boundary(f3_cpu *cpu) { return machine(cpu).boundary(); }
int f3_register_blocks(f3_cpu *cpu, const f3_block *blocks, size_t count) {
    if (!cpu || !cpu->runtime || (count && !blocks)) return 0;
    for (size_t i = 0; i < count; ++i)
        if (!blocks[i].execute || (blocks[i].address & 1) ||
            (i && blocks[i - 1].address >= blocks[i].address)) return 0;
    auto &m = machine(cpu);
    m.blocks = blocks;
    m.block_count = count;
    return 1;
}
int f3_dispatch(f3_cpu *cpu) {
    if (!cpu || !cpu->runtime || cpu->halted) return 0;
    if (f3_boundary(cpu)) return !cpu->halted;
    auto &m = machine(cpu);
    // Trace must execute instruction by instruction; never defer T0/T1 in a block.
    if (!(cpu->sr & 0xc000) && cpu->pc < 0x200000 && m.block_count) {
        const f3_block *end = m.blocks + m.block_count;
        const auto *block = std::lower_bound(m.blocks, end, cpu->pc,
            [](const f3_block &entry, uint32_t pc) { return entry.address < pc; });
        if (block != end && block->address == cpu->pc) {
            ++m.native_blocks;
            block->execute(cpu);
            return !cpu->halted;
        }
    }
    return f3_fallback(cpu);
}
int f3_fallback(f3_cpu *cpu) { return machine(cpu).fallback(); }
}
