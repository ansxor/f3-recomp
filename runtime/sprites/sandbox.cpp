#include "sprites/sandbox.hpp"
#include "f3rt/machine.hpp"
#include "sprites/presentation.hpp"
#include <algorithm>
#include <cstring>

namespace f3rt {
namespace {
constexpr uint64_t REPLAY_BUDGET_CYCLES = 1000000;
constexpr uint32_t REPLAY_BUDGET_BLOCKS = 500000;
constexpr uint32_t BANK_BYTES = SPRITE_BANK_STRIDE;
}

const char *Sandbox::abort_name(Abort reason) {
    switch (reason) {
    case Abort::Device:
        return "device-access";
    case Abort::ForbiddenWrite:
        return "forbidden-write";
    case Abort::Untranslated:
        return "untranslated-pc";
    case Abort::Exception:
        return "exception";
    case Abort::ResetDevices:
        return "reset-devices";
    case Abort::Budget:
        return "budget";
    case Abort::Stopped:
        return "stopped";
    case Abort::NoNative:
        return "no-native-blocks";
    case Abort::Count:
        break;
    }
    return "none";
}

void Sandbox::Result::clear() {
    ok = false;
    reason = Abort::Count;
    written.clear();
    dense.clear();
    first = last = 0;
    bank = false;
}

Sandbox::Sandbox(Machine &machine) : m_machine(machine) {
    m_cpu.runtime = &machine;
    m_page_slot.fill(-1);
    m_capture_slot.fill(-1);
}

const f3_block *Sandbox::find_block(uint32_t pc) const {
    if (pc >= ROM_END || (pc & 1)) return nullptr;
    const auto &page = m_machine.native_pages[pc >> 12];
    if (!page.count) return nullptr;
    if (page.dense) {
        const uint32_t offset = pc - page.address;
        return !(offset & 1) && offset < 2u * page.count ? page.first + (offset >> 1) : nullptr;
    }
    const f3_block *end = page.first + page.count;
    const auto *entry =
        std::lower_bound(page.first, end, pc, [](const f3_block &block, uint32_t value) {
            return block.address < value;
        });
    return entry != end && entry->address == pc ? entry : nullptr;
}

uint8_t Sandbox::ram_get(uint32_t offset) const {
    const int16_t slot = m_page_slot[offset / OVERLAY_PAGE];
    return slot >= 0 ? m_page_pool[size_t(slot)][offset % OVERLAY_PAGE] : m_machine.ram[offset];
}

void Sandbox::ram_put(uint32_t offset, uint8_t value) {
    int16_t &slot = m_page_slot[offset / OVERLAY_PAGE];
    if (slot < 0) {
        slot = int16_t(m_touched_pages.size());
        if (m_page_pool.size() <= size_t(slot)) m_page_pool.emplace_back();
        const uint32_t base = offset - offset % OVERLAY_PAGE;
        std::memcpy(m_page_pool[size_t(slot)].data(), m_machine.ram.data() + base, OVERLAY_PAGE);
        m_touched_pages.push_back(uint16_t(offset / OVERLAY_PAGE));
    }
    m_page_pool[size_t(slot)][offset % OVERLAY_PAGE] = value;
}

void Sandbox::abort(Abort reason) {
    if (m_abort != Abort::Count) return;
    m_abort = reason;
    m_cpu.halted = 1;
}

uint8_t Sandbox::read8(uint32_t address) {
    const uint32_t a = address & 0xffffff;
    if (a < ROM_END) return a < m_machine.roms.main.size() ? m_machine.roms.main[a] : 0xff;
    if (a - RAM_BASE < RAM_END - RAM_BASE) return ram_get(a & RAM_MIRROR_MASK);
    if (a - PALETTE_BASE < PALETTE_RAM_SIZE) return m_machine.palette[a - PALETTE_BASE];
    if (a - GRAPHICS_BASE < GRAPHICS_RAM_SIZE) {
        const uint32_t offset = a - GRAPHICS_BASE;
        if (offset < SPRITE_RAM_SIZE && sprite_slot_listed(offset)) {
            const int16_t slot = m_capture_slot[sprite_slot_for(offset)];
            if (slot >= 0) return m_capture[size_t(slot)].bytes[offset & 15];
        }
        return m_machine.graphics[offset];
    }
    abort(Abort::Device);
    return 0;
}

void Sandbox::write8(uint32_t address, uint8_t value) {
    const uint32_t a = address & 0xffffff;
    if (a - RAM_BASE < RAM_END - RAM_BASE) {
        ram_put(a & RAM_MIRROR_MASK, value);
        return;
    }
    if (a - GRAPHICS_BASE < SPRITE_RAM_SIZE) {
        const uint32_t offset = a - GRAPHICS_BASE;
        if (!sprite_slot_listed(offset)) return;
        const uint16_t key = sprite_slot_for(offset);
        int16_t &slot = m_capture_slot[key];
        if (slot < 0) {
            Entry entry{key, {}};
            std::memcpy(entry.bytes.data(), m_machine.graphics.data() + (offset & ~15u), 16);
            slot = int16_t(m_capture.size());
            m_capture.push_back(entry);
        }
        m_capture[size_t(slot)].bytes[offset & 15] = value;
        return;
    }
    if (a < ROM_END || a - PALETTE_BASE < PALETTE_RAM_SIZE ||
        a - GRAPHICS_BASE < GRAPHICS_RAM_SIZE)
        abort(Abort::ForbiddenWrite);
    else abort(Abort::Device);
}

uint32_t Sandbox::read(uint32_t address, unsigned width) {
    if (m_abort != Abort::Count) return 0;
    uint32_t value = 0;
    for (unsigned i = 0; i < width; ++i) value = value << 8 | read8(address + i);
    return value;
}

void Sandbox::write(uint32_t address, uint32_t value, unsigned width) {
    if (m_abort != Abort::Count) return;
    for (unsigned i = 0; i < width; ++i)
        write8(address + i, uint8_t(value >> (8 * (width - 1 - i))));
}

int Sandbox::unit_exit(uint32_t unit) {
    if (unit != m_unit || m_cpu.pc != m_end_pc || m_abort != Abort::Count) return 0;
    m_done = true;
    return 1;
}

void Sandbox::begin(uint32_t unit, uint32_t start_pc, uint32_t end_pc) {
    for (uint16_t page : m_touched_pages) m_page_slot[page] = -1;
    m_touched_pages.clear();
    for (const Entry &entry : m_capture) m_capture_slot[entry.key] = -1;
    m_capture.clear();
    f3_cc_flush(&m_machine.cpu);
    m_cpu = m_machine.cpu;
    m_cpu.ram = nullptr;
    m_cpu.ram_writer = nullptr;
    m_cpu.rom = nullptr;
    m_cpu.rom_size = 0;
    m_cpu.runtime = &m_machine;
    m_cpu.pc = start_pc;
    m_cpu.stopped = m_cpu.halted = 0;
    m_unit = unit;
    m_end_pc = end_pc;
    m_done = false;
    m_abort = Abort::Count;
}

Sandbox::Abort Sandbox::run() {
    const uint64_t limit = m_cpu.cycles + REPLAY_BUDGET_CYCLES;
    for (uint32_t blocks = 0; m_abort == Abort::Count && !m_done; ++blocks) {
        if (blocks >= REPLAY_BUDGET_BLOCKS || m_cpu.cycles >= limit) {
            abort(Abort::Budget);
            break;
        }
        if (m_cpu.halted || m_cpu.stopped || (m_cpu.sr & 0xc000)) {
            abort(Abort::Stopped);
            break;
        }
        const f3_block *block = find_block(m_cpu.pc);
        if (!block) {
            abort(Abort::Untranslated);
            break;
        }
        m_cpu.dispatch_deadline = limit;
        block->execute(&m_cpu);
    }
    return m_abort;
}

void Sandbox::collect(Result &out) const {
    out.written = m_capture;
    std::sort(out.written.begin(), out.written.end(),
              [](const Entry &a, const Entry &b) { return a.key < b.key; });
    if (m_capture.empty()) return;
    out.bank = m_capture.front().key >= SPRITE_ENTRIES;
    uint16_t lo = 0xffff, hi = 0;
    for (const Entry &entry : m_capture) {
        if ((entry.key >= SPRITE_ENTRIES) != out.bank) continue;
        lo = std::min<uint16_t>(lo, entry.key & (SPRITE_ENTRIES - 1));
        hi = std::max<uint16_t>(hi, entry.key & (SPRITE_ENTRIES - 1));
    }
    out.first = lo;
    out.last = hi;
    out.dense.resize((size_t(hi) - lo + 1) * 16);
    for (uint16_t entry = lo; entry <= hi; ++entry) {
        const uint16_t key = uint16_t((out.bank ? SPRITE_ENTRIES : 0) + entry);
        const int16_t slot = m_capture_slot[key];
        const uint8_t *source =
            slot >= 0 ? m_capture[size_t(slot)].bytes.data()
                      : m_machine.graphics.data() + (out.bank ? BANK_BYTES : 0) +
                            size_t(entry) * 16;
        std::memcpy(out.dense.data() + size_t(entry - lo) * 16, source, 16);
    }
}

}
