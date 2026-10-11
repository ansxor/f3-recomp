#include "f3rt/cpu_abi.h"
#include "f3rt/machine.hpp"
#include "f3rt/block_profile.h"
#include "sprites/units.hpp"
#include <algorithm>
#include <cstdio>
#include <stdexcept>

namespace {
f3rt::Machine &machine(f3_cpu *cpu) { return *static_cast<f3rt::Machine *>(cpu->runtime); }
// Emit-unit replay clone of the main CPU, or null for every other CPU (including the real one).
// Sandbox CPUs never reach devices: every entry point below diverts them before side effects.
f3rt::SpriteUnits *sandbox(f3_cpu *cpu) {
    auto *units = machine(cpu).sprite_units.get();
    return units && units->is_sandbox(cpu) ? units : nullptr;
}
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
uint8_t f3_read8(f3_cpu *cpu, uint32_t a) {
    if (auto *s = sandbox(cpu)) return uint8_t(s->sandbox_read(a, 1));
    return bus(cpu,a,1).read8(a);
}
uint16_t f3_read16(f3_cpu *cpu, uint32_t a) {
    if (auto *s = sandbox(cpu)) return uint16_t(s->sandbox_read(a, 2));
    return bus(cpu,a,2).read16(a);
}
uint32_t f3_read32(f3_cpu *cpu, uint32_t a) {
    if (auto *s = sandbox(cpu)) return s->sandbox_read(a, 4);
    return bus(cpu,a,4).read32(a);
}
void f3_write8(f3_cpu *cpu, uint32_t a, uint8_t v) {
    if (auto *s = sandbox(cpu)) { s->sandbox_write(a, v, 1); return; }
    bus(cpu,a,1).write8(a,v);
}
void f3_write16(f3_cpu *cpu, uint32_t a, uint16_t v) {
    if (auto *s = sandbox(cpu)) { s->sandbox_write(a, v, 2); return; }
    bus(cpu,a,2).write16(a,v);
}
void f3_write32(f3_cpu *cpu, uint32_t a, uint32_t v) {
    if (auto *s = sandbox(cpu)) { s->sandbox_write(a, v, 4); return; }
    bus(cpu,a,4).write32(a,v);
}
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
    if (auto *s = sandbox(cpu)) { s->sandbox_abort(f3rt::SpriteUnits::Abort::Exception); return; }
    if (vector > 255) { cpu->halted = 1; return; }
    f3_cc_flush(cpu);
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
    if (auto *s = sandbox(cpu)) { s->sandbox_abort(f3rt::SpriteUnits::Abort::ResetDevices); return; }
    auto &m = machine(cpu);
    m.advance_to(cpu->cycles);
    m.reset_devices();
}
int f3_boundary(f3_cpu *cpu) {
    if (auto *s = sandbox(cpu)) { s->sandbox_abort(f3rt::SpriteUnits::Abort::Stopped); return 1; }
    return machine(cpu).boundary();
}
int f3_validate_main_rom(f3_cpu *cpu, size_t size, uint32_t crc) {
    if (!cpu || !cpu->runtime) return 0;
    const auto &rom = machine(cpu).roms.main;
    const uint32_t actual = f3rt::crc32(rom.data(), rom.size());
    if (rom.size() != size || actual != crc) {
        char message[160];
        std::snprintf(message, sizeof(message),
                      "Generated main ROM mismatch: loaded %zu bytes / %08x, expected %zu / %08x",
                      rom.size(), actual, size, crc);
        throw std::runtime_error(message);
    }
    return 1;
}
int f3_register_blocks(f3_cpu *cpu, const f3_block *blocks, size_t count) {
    if (!cpu || !cpu->runtime || (count && !blocks)) return 0;
    for (size_t i = 0; i < count; ++i)
        if (!blocks[i].execute || (blocks[i].address & 1) ||
            (i && blocks[i - 1].address >= blocks[i].address)) return 0;
    auto &m = machine(cpu);
    for (const auto &range : m.excluded_code) {
        const auto *entry = count ? std::lower_bound(blocks, blocks + count, range.start,
            [](const f3_block &block, uint32_t pc) { return block.address < pc; }) : nullptr;
        if (entry && entry != blocks + count && entry->address < range.end) return 0;
    }
    // Commit only after all validation succeeds: failed re-registration must
    // leave both the original table and its derived lookup unchanged.
    size_t index = 0;
    for (size_t page = 0; page < m.native_pages.size(); ++page) {
        const size_t first = index;
        const uint32_t end = uint32_t((page + 1) * 0x1000);
        while (index < count && blocks[index].address < end) ++index;
        auto &lookup = m.native_pages[page];
        lookup.first = count ? blocks + first : nullptr;
        lookup.count = uint16_t(index - first);
        lookup.address = index != first ? blocks[first].address : 0;
        lookup.dense = index != first &&
            blocks[index - 1].address - lookup.address == 2 * (index - first - 1);
    }
    m.blocks = blocks;
    m.block_count = count;
    return 1;
}
int f3_register_exclusions(f3_cpu *cpu, const f3_excluded_range *ranges, size_t count) {
    if (!cpu || !cpu->runtime || (count && !ranges)) return 0;
    auto &m = machine(cpu);
    for (size_t i = 0; i < count; ++i) {
        const auto &range = ranges[i];
        if ((range.start & 1) || (range.end & 1) || range.start >= range.end ||
            range.end > 0x200000 || !range.reason || !*range.reason ||
            !range.evidence || !*range.evidence ||
            (i && ranges[i - 1].end > range.start)) return 0;
        if (m.block_count) {
            const auto *entry = std::lower_bound(m.blocks, m.blocks + m.block_count, range.start,
                [](const f3_block &block, uint32_t pc) { return block.address < pc; });
            if (entry != m.blocks + m.block_count && entry->address < range.end) return 0;
        }
    }
    m.excluded_code = std::span(ranges, count);
    return 1;
}
int f3_dispatch(f3_cpu *cpu) {
    if (!cpu || !cpu->runtime || cpu->halted) return 0;
    if (sandbox(cpu)) { f3_boundary(cpu); return 0; } // replays run blocks directly, never via dispatch
    if (f3_boundary(cpu)) return !cpu->halted;
    auto &m = machine(cpu);
    // Trace must execute instruction by instruction; never defer T0/T1 in a block.
    if (!(cpu->sr & 0xc000) && cpu->pc < 0x200000 && m.block_count) {
        const auto &page = m.native_pages[cpu->pc >> 12];
        if (page.count) {
            const f3_block *block = nullptr;
            if (page.dense) {
                const uint32_t offset = cpu->pc - page.address;
                if (!(offset & 1) && offset < 2u * page.count)
                    block = page.first + (offset >> 1);
            } else {
                const f3_block *end = page.first + page.count;
                const auto *entry = std::lower_bound(page.first, end, cpu->pc,
                    [](const f3_block &entry, uint32_t pc) { return entry.address < pc; });
                if (entry != end && entry->address == cpu->pc) block = entry;
            }
            if (block) {
                ++m.native_blocks;
                block->execute(cpu);
                return !cpu->halted;
            }
        }
    }
#ifdef F3_PROFILE_SLIM_ENABLED
    // ABI 3 exclusions take precedence over opt-in slim misses, including
    // odd PCs and 24-bit aliases; fallback rejects them before interpretation.
    const uint32_t physical_pc = cpu->pc & 0xffffffu;
    for (const auto &range : m.excluded_code) {
        if (physical_pc < range.start) break;
        if (physical_pc < range.end) return f3_fallback(cpu);
    }
    f3_profile_cold_abort(F3_PROFILE_MAIN,
        f3rt::crc32(m.roms.main.data(), m.roms.main.size()), cpu->pc);
#endif
    return f3_fallback(cpu);
}
int f3_fallback(f3_cpu *cpu) {
    // Untranslated or excluded instruction inside a replay: abandon it, never interpret.
    if (auto *s = sandbox(cpu)) { s->sandbox_abort(f3rt::SpriteUnits::Abort::Untranslated); return 0; }
    return machine(cpu).fallback();
}
void f3_unit_enter(f3_cpu *cpu, uint32_t unit) {
    if (auto *s = machine(cpu).sprite_units.get()) s->unit_enter(cpu, unit);
}
int f3_unit_exit(f3_cpu *cpu, uint32_t unit) {
    auto *s = machine(cpu).sprite_units.get();
    return s ? s->unit_exit(cpu, unit) : 0;
}
}
