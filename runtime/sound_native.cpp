#include "sound_native.hpp"
#include "sound_native_ops.h"
#include "sound_trace.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/rom.hpp"
#include "state_io.hpp"
#include <cstdio>
#include <stdexcept>
#include <string>

namespace f3rt {

SoundNative::SoundNative(Machine &machine, const f3_block *blocks, size_t block_count,
                         std::span<const f3_excluded_range> excluded)
    : m_machine(machine), m_blocks(blocks), m_excluded(excluded)
{
    const auto &rom_bytes = m_machine.roms.sound;
    uint32_t crc = crc32(rom_bytes.data(), rom_bytes.size());
    if (crc != 0x5a7e9117u) {
        char buf[128];
        snprintf(buf, sizeof(buf), "SoundNative: unsupported sound ROM CRC 0x%08x (expected 0x5a7e9117)", crc);
        throw std::runtime_error(buf);
    }

    m_cpu.runtime = this;
    m_cpu.sr = F3_CCR_Z; // A zeroed Musashi context starts with its inverted-Z latch clear.

    uint32_t previous_end = ROM_BASE;
    size_t excluded_words = 0;
    for (const auto &range : m_excluded) {
        if ((range.start | range.end) & 1u || range.start < previous_end ||
            range.start >= range.end || range.end > ROM_BASE + ROM_SIZE ||
            !range.reason || !*range.reason || !range.evidence || !*range.evidence)
            throw std::runtime_error("SoundNative: invalid excluded ROM ranges");
        excluded_words += (range.end - range.start) / 2;
        previous_end = range.end;
    }
    if ((!blocks && block_count) || block_count != ROM_SIZE / 2 - excluded_words)
        throw std::runtime_error("SoundNative requires the exact aligned ROM exclusion complement");
    size_t index = 0;
    uint32_t next_pc = ROM_BASE;
    for (const auto &range : m_excluded) {
        for (; next_pc < range.start; next_pc += 2, ++index) {
            if (blocks[index].address != next_pc || !blocks[index].execute)
                throw std::runtime_error("SoundNative: missing or unsorted aligned ROM entry");
        }
        next_pc = range.end;
    }
    for (; next_pc < ROM_BASE + ROM_SIZE; next_pc += 2, ++index) {
        if (blocks[index].address != next_pc || !blocks[index].execute)
            throw std::runtime_error("SoundNative: missing or unsorted aligned ROM entry");
    }
}

SoundNative::~SoundNative() = default;

void SoundNative::do_reset() {
    m_cpu.stopped = 0;
    m_cpu.halted = 0;
    f3_cc_flush(&m_cpu);
    if (!(m_cpu.sr & 0x2000u)) m_cpu.usp = m_cpu.a[7];
    m_cpu.sr = uint16_t(0x2700u | (m_cpu.sr & 0x1fu));
    m_cpu.cc_op = F3_CC_OP_NONE;
    m_cpu.a[7] = m_machine.audio->read32(0);
    m_cpu.ssp = m_cpu.a[7];
    m_cpu.pc = m_machine.audio->read32(4);
    m_reset_cycles = 40;
    m_needs_reset = false;
}

void SoundNative::reset(bool asserted) {
    if (!asserted) {
        m_needs_reset = true;
    }
}


uint32_t SoundNative::pc() const {
    return m_cpu.pc;
}

void SoundNative::trace_sound(uint32_t address, uint32_t value, uint8_t width, bool write) {
    if (!m_machine.sound_trace) return;
    address &= 0xffffffu; // The 68000 exposes a 24-bit physical bus.
    const uint32_t pc = m_cpu.pc;
    if (write && width == 1 && pc == 0xc130f0 &&
        address == ((m_cpu.a[4] + 0x14u) & 0xffffffu))
        m_machine.sound_trace->record(m_machine, SoundTrace::DirectNote, pc, address, value, width);
    if (write && width == 2 && pc == 0xc140e4 &&
        address == ((m_cpu.a[1] + 0x24u) & 0xffffffu))
        m_machine.sound_trace->note_context(m_machine, pc,
            m_cpu.a[5], m_cpu.a[1], m_cpu.a[6], m_cpu.a[4]);
    if (write && width == 2 && pc == 0xc17632 &&
        address == ((m_cpu.a[4] + 0xau) & 0xffffffu))
        m_machine.sound_trace->voice_context(m_machine, pc, m_cpu.a[4]);
    if (address < 0x140000 || address >= 0x340004) return;
    if (write && width == 2 && address == 0x20001e &&
        (pc == 0xc17e62 || pc == 0xc17806))
        m_machine.sound_trace->voice_context(m_machine, pc, m_cpu.a[4]);
    m_machine.sound_trace->record(m_machine,
        write ? SoundTrace::SoundWrite : SoundTrace::SoundRead,
        pc, address, value, width);
}

uint8_t SoundNative::read8(uint32_t address) {
    uint8_t val = m_machine.audio->read8(address);
    trace_sound(address, val, 1, false);
    return val;
}

uint16_t SoundNative::read16(uint32_t address) {
    uint16_t val = m_machine.audio->read16(address);
    trace_sound(address, val, 2, false);
    return val;
}

uint32_t SoundNative::read32(uint32_t address) {
    uint32_t val = m_machine.audio->read32(address);
    trace_sound(address, val, 4, false);
    return val;
}

void SoundNative::write8(uint32_t address, uint8_t value) {
    trace_sound(address, value, 1, true);
    m_machine.audio->write8(address, value);
}

void SoundNative::write16(uint32_t address, uint16_t value) {
    trace_sound(address, value, 2, true);
    m_machine.audio->write16(address, value);
}

void SoundNative::write32(uint32_t address, uint32_t value) {
    trace_sound(address, value, 4, true);
    m_machine.audio->write32(address, value);
}

void SoundNative::set_sr(uint16_t sr) {
    f3_sound_cc_flush(&m_cpu);
    sr &= 0xa71fu; // 68000 SR mask
    uint16_t old_s = m_cpu.sr & 0x2000u;
    uint16_t new_s = sr & 0x2000u;
    if (old_s != new_s) {
        if (new_s) {
            m_cpu.usp = m_cpu.a[7];
            m_cpu.a[7] = m_cpu.ssp;
        } else {
            m_cpu.ssp = m_cpu.a[7];
            m_cpu.a[7] = m_cpu.usp;
        }
    }
    m_cpu.sr = sr;
    m_cpu.cc_op = F3_CC_OP_NONE;
    check_interrupts();
}

void SoundNative::exception(unsigned vector, uint32_t return_pc) {
    if (vector > 255) {
        m_cpu.halted = 1;
        return;
    }
    f3_sound_cc_flush(&m_cpu);
    uint16_t old_sr = m_cpu.sr;
    if (!(m_cpu.sr & 0x2000)) {
        m_cpu.usp = m_cpu.a[7];
        m_cpu.a[7] = m_cpu.ssp;
    }
    m_cpu.sr = (m_cpu.sr | 0x2000u) & ~0x8000u; // S=1, T1=0
    m_cpu.stopped = 0;
    m_cpu.a[7] -= 6;
    write16(m_cpu.a[7], old_sr);
    write32(m_cpu.a[7] + 2, return_pc);
    m_cpu.pc = read32(vector * 4);

    static const uint8_t s_sound_exception_cycles[16] = {
        40, 4, 50, 50, 34, 38, 40, 34, 34, 34, 34, 34, 4, 4, 4, 44
    };
    uint32_t cost = 34;
    if (vector < 16) cost = s_sound_exception_cycles[vector];
    else if (vector >= 24 && vector < 32) cost = 44;
    else if (vector >= 32 && vector < 48) cost = 34;
    else cost = 4;
    m_cpu.cycles += cost;
}

void SoundNative::rte() {
    f3_sound_cc_flush(&m_cpu);
    if (!(m_cpu.sr & 0x2000)) {
        exception(8, m_cpu.pc); // Privilege violation
        return;
    }
    uint16_t new_sr = read16(m_cpu.a[7]);
    uint32_t new_pc = read32(m_cpu.a[7] + 2);
    m_cpu.a[7] += 6;
    m_cpu.pc = new_pc;
    m_cpu.cycles += 20;
    set_sr(new_sr);
}

void SoundNative::stop(uint16_t new_sr, uint32_t next_pc) {
    f3_cc_flush(&m_cpu);
    if (!(m_cpu.sr & 0x2000)) {
        exception(8, m_cpu.pc);
        return;
    }
    m_cpu.stopped = 1;
    m_cpu.pc = next_pc;
    m_cpu.cycles += 4;
    set_sr(new_sr);
    if (!m_cpu.stopped) m_cpu.cycles += 4; // Pending-at-STOP polling cost in the oracle.
}

void SoundNative::check_interrupts() {
    int irq_level = m_machine.audio->irq_level();
    int mask = (m_cpu.sr >> 8) & 7;
    if (irq_level > mask || irq_level == 7) {
        m_cpu.stopped = 0;
        int vector = m_machine.audio->irq_ack(irq_level);
        if (vector == 0) vector = 15; // Uninitialized vector, not the autovector sentinel.
        f3_sound_cc_flush(&m_cpu);
        uint16_t old_sr = m_cpu.sr;
        if (!(m_cpu.sr & 0x2000)) {
            m_cpu.usp = m_cpu.a[7];
            m_cpu.a[7] = m_cpu.ssp;
        }
        m_cpu.sr = (m_cpu.sr & ~0xa700u) | 0x2000u | (uint16_t(irq_level) << 8);
        m_cpu.cc_op = F3_CC_OP_NONE;

        m_cpu.a[7] -= 6;
        write16(m_cpu.a[7], old_sr);
        write32(m_cpu.a[7] + 2, m_cpu.pc);

        m_cpu.pc = read32(vector * 4);

        m_cpu.cycles += 44;
    }
}

size_t SoundNative::block_index(uint32_t pc) const {
    size_t removed_words = 0;
    for (const auto &range : m_excluded) {
        if (pc < range.start) break;
        if (pc < range.end) {
            char prefix[128];
            snprintf(prefix, sizeof(prefix),
                     "SoundNative: fatal excluded reachable PC 0x%08x in [0x%08x, 0x%08x): ",
                     pc, range.start, range.end);
            throw std::runtime_error(std::string(prefix) + range.reason);
        }
        removed_words += (range.end - range.start) / 2;
    }
    if (pc < ROM_BASE || pc >= ROM_BASE + ROM_SIZE || (pc & 1u)) {
        char buf[128];
        snprintf(buf, sizeof(buf), "SoundNative: fatal unsupported reachable PC 0x%08x (%s)",
                 pc, (pc & 1u) ? "odd address" : "outside sound ROM");
        throw std::runtime_error(buf);
    }
    return ((pc - ROM_BASE) >> 1) - removed_words;
}

void SoundNative::dispatch_one() {
    const size_t index = block_index(m_cpu.pc);
    ++m_instruction_count;
    m_blocks[index].execute(&m_cpu);
}

int SoundNative::run(int cycles) {
    if (m_needs_reset) do_reset();
    const uint64_t begin = m_cpu.cycles;
    const uint64_t deadline = begin + unsigned(cycles);
    if (m_reset_cycles) {
        m_cpu.cycles += unsigned(m_reset_cycles);
        m_reset_cycles = 0;
        if (m_cpu.cycles >= deadline) return int(m_cpu.cycles - begin);
    }
    check_interrupts();
    if (!m_cpu.stopped && !m_cpu.halted) {
        // Musashi executes the first handler instruction even when IRQ entry
        // already exhausted the requested one-cycle dispatch budget.
        do {
            dispatch_one();
        } while (!m_cpu.stopped && !m_cpu.halted && m_cpu.cycles < deadline);
    }
    if (m_cpu.stopped && m_cpu.cycles < deadline) m_cpu.cycles = deadline;
    return int(m_cpu.cycles - begin);
}

size_t SoundNative::state_size() const {
    return sizeof(CanonicalSoundNative);
}

void SoundNative::save_state(StateWriter &writer) const {
    CanonicalSoundNative st{};
    for (int i = 0; i < 8; ++i) {
        st.cpu.d[i] = m_cpu.d[i];
        st.cpu.a[i] = m_cpu.a[i];
    }
    st.cpu.pc = m_cpu.pc;
    st.cpu.usp = m_cpu.usp;
    st.cpu.ssp = m_cpu.ssp;
    st.cpu.msp = m_cpu.msp;
    st.cpu.vbr = m_cpu.vbr;
    st.cpu.sfc = m_cpu.sfc;
    st.cpu.dfc = m_cpu.dfc;
    st.cpu.cacr = m_cpu.cacr;
    st.cpu.caar = m_cpu.caar;
    st.cpu.sr = m_cpu.sr;
    st.cpu.stopped = m_cpu.stopped;
    st.cpu.halted = m_cpu.halted;
    st.cpu.cc_src = m_cpu.cc_src;
    st.cpu.cc_dst = m_cpu.cc_dst;
    st.cpu.cc_result = m_cpu.cc_result;
    st.cpu.cc_op = m_cpu.cc_op;
    st.cpu.cc_width = m_cpu.cc_width;
    st.cpu.cc_mask = m_cpu.cc_mask;
    st.cpu.cycles = m_cpu.cycles;
    st.cpu.dispatch_deadline = m_cpu.dispatch_deadline;
    st.needs_reset = m_needs_reset ? 1 : 0;
    st.reset_cycles = m_reset_cycles;
    writer.write(st);
}

void SoundNative::load_state(StateReader &reader) {
    CanonicalSoundNative st;
    reader.read(st);
    for (int i = 0; i < 8; ++i) {
        m_cpu.d[i] = st.cpu.d[i];
        m_cpu.a[i] = st.cpu.a[i];
    }
    m_cpu.pc = st.cpu.pc;
    m_cpu.usp = st.cpu.usp;
    m_cpu.ssp = st.cpu.ssp;
    m_cpu.msp = st.cpu.msp;
    m_cpu.vbr = st.cpu.vbr;
    m_cpu.sfc = st.cpu.sfc;
    m_cpu.dfc = st.cpu.dfc;
    m_cpu.cacr = st.cpu.cacr;
    m_cpu.caar = st.cpu.caar;
    m_cpu.sr = st.cpu.sr;
    m_cpu.stopped = st.cpu.stopped;
    m_cpu.halted = st.cpu.halted;
    m_cpu.cc_src = st.cpu.cc_src;
    m_cpu.cc_dst = st.cpu.cc_dst;
    m_cpu.cc_result = st.cpu.cc_result;
    m_cpu.cc_op = st.cpu.cc_op;
    m_cpu.cc_width = st.cpu.cc_width;
    m_cpu.cc_mask = st.cpu.cc_mask;
    m_cpu.cc_pad = 0;
    m_cpu.cycles = st.cpu.cycles;
    m_cpu.dispatch_deadline = st.cpu.dispatch_deadline;
    m_cpu.runtime = this;
    m_needs_reset = st.needs_reset != 0;
    m_reset_cycles = st.reset_cycles;
}

} // namespace f3rt

extern "C" {

uint8_t f3_sound_read8(f3_cpu *cpu, uint32_t address) {
    return static_cast<f3rt::SoundNative*>(cpu->runtime)->read8(address);
}

uint16_t f3_sound_read16(f3_cpu *cpu, uint32_t address) {
    return static_cast<f3rt::SoundNative*>(cpu->runtime)->read16(address);
}

uint32_t f3_sound_read32(f3_cpu *cpu, uint32_t address) {
    return static_cast<f3rt::SoundNative*>(cpu->runtime)->read32(address);
}

void f3_sound_write8(f3_cpu *cpu, uint32_t address, uint8_t value) {
    static_cast<f3rt::SoundNative*>(cpu->runtime)->write8(address, value);
}

void f3_sound_write16(f3_cpu *cpu, uint32_t address, uint16_t value) {
    static_cast<f3rt::SoundNative*>(cpu->runtime)->write16(address, value);
}

void f3_sound_write32(f3_cpu *cpu, uint32_t address, uint32_t value) {
    static_cast<f3rt::SoundNative*>(cpu->runtime)->write32(address, value);
}

void f3_sound_set_sr(f3_cpu *cpu, uint16_t sr) {
    static_cast<f3rt::SoundNative*>(cpu->runtime)->set_sr(sr);
}

void f3_sound_exception(f3_cpu *cpu, unsigned vector, uint32_t return_pc) {
    static_cast<f3rt::SoundNative*>(cpu->runtime)->exception(vector, return_pc);
}

void f3_sound_rte(f3_cpu *cpu) {
    static_cast<f3rt::SoundNative*>(cpu->runtime)->rte();
}

void f3_sound_stop(f3_cpu *cpu, uint16_t new_sr, uint32_t next_pc) {
    static_cast<f3rt::SoundNative*>(cpu->runtime)->stop(new_sr, next_pc);
}


void f3_sound_unsupported_pc(f3_cpu *cpu, uint32_t pc) {
    uint16_t opcode = static_cast<f3rt::SoundNative*>(cpu->runtime)->read16(pc);
    char buf[128];
    snprintf(buf, sizeof(buf), "SoundNative: fatal unsupported reachable PC: 0x%08x (opcode 0x%04x)", pc, opcode);
    fprintf(stderr, "%s\n", buf);
    cpu->halted = 1;
    throw std::runtime_error(buf);
}

} // extern "C"
