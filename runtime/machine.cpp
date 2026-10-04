#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/video.hpp"
#include "f3rt/game_video.hpp"
#include "sound_trace.hpp"
#include "sound_native.hpp"
#include "eeprom.hpp"
#include "interpreter.hpp"
#include <algorithm>
#include <stdexcept>
#include <sstream>

namespace f3rt {
Machine::Machine(RomSet set) : roms(std::move(set)), video(std::make_unique<Video>()),
    audio(std::make_unique<Audio>()), eeprom(std::make_unique<Eeprom>()) {
    if (roms.main.size() != 0x200000) throw std::runtime_error("Main ROM must be 2 MiB");
    if (!video->load_roms(roms.sprites, roms.sprites_hi, roms.tiles, roms.tiles_hi))
        throw std::runtime_error("Invalid video ROM regions");
    audio->set_shared_ram(shared.data(), shared.size());
    audio->load_sound_rom(roms.sound);
    audio->load_sample_rom(roms.samples);
    interpreter = std::make_unique<Interpreter>(*this);
    audio->set_cpu_runner([this](int cycles) { return interpreter->run_audio(cycles); });
    audio->set_reset_callback([this](bool asserted) { interpreter->audio_reset(asserted); });
    audio->set_irq_callback([this](bool asserted) { interpreter->audio_irq(asserted); });
    reset();
}
Machine::~Machine() = default;
void Machine::use_native_sound(const f3_block *program, size_t count) {
    if (!audio->is_reset() || audio->clock_ticks())
        throw std::runtime_error("Select the native sound driver before machine execution");
    sound_native = std::make_unique<SoundNative>(*this, program, count);
    audio->set_cpu_runner([this](int cycles) { return sound_native->run(cycles); });
    audio->set_reset_callback([this](bool asserted) { sound_native->reset(asserted); });
    // Native SR/IRQ recognition reads the DUART's current line directly.
    audio->set_irq_callback({});
    sound_native->reset(true);
}
uint32_t Machine::sound_pc() const {
    return sound_native ? sound_native->pc() : interpreter->sound_pc();
}
uint64_t Machine::raster_cycle(uint64_t pixels) const {
    return (pixels * main_clock + pixel_clock - 1) / pixel_clock;
}
void Machine::reset_devices() {
    cpu.dispatch_deadline = 0;
    audio->set_reset(true);
    eeprom->pins(0, cpu.cycles);
    pending_irqs = 0;
    irq3_at = UINT64_MAX;
    watchdog_at = cpu.cycles + 3ull * main_clock;
}
void Machine::reset() {
    cpu = {};
    cpu.runtime = this;
    eeprom->reset();
    frame = hardware_cycles = 0;
    // Reference beam time zero is VBSTART, not scanline zero.
    next_vblank = raster_cycle(frame_pixels);
    reset_devices();
    audio->reset_board();
    if (sound_trace) sound_trace->record(*this, SoundTrace::Reset, cpu.pc, 0, 2, 0);
    video->reset();
    if (game_video) game_video->reset();
    interpreter->reset_main();
}
uint32_t Machine::input_word(unsigned index) const {
    if (index >= inputs.size()) return 0xffffffff;
    uint32_t value = inputs[index];
    if (index == 0) {
        const uint32_t io = (system_inputs & 0xfe) | unsigned(eeprom->output(cpu.cycles));
        value = (value & 0xffff) | (io << 16) | (io << 24);
    } else if (index == 1 || index == 5) {
        value = (value & 0xffff) | (uint32_t(coin_word[index == 5]) << 16);
    }
    return value;
}
uint8_t Machine::read8(uint32_t a) {
    a &= 0xffffff;
    if (a < 0x200000) return roms.main[a];
    if (a >= 0x400000 && a < 0x440000) return ram[a & 0x1ffff];
    if (a >= 0x440000 && a < 0x448000) return palette[a - 0x440000];
    if (a >= 0x4a0000 && a < 0x4a0020) return uint8_t(input_word((a - 0x4a0000) / 4) >> (24 - 8 * (a & 3)));
    if (a >= 0x600000 && a < 0x640000) return graphics[a - 0x600000];
    if (a >= 0xc00000 && a < 0xc00800) return shared[a - 0xc00000];
    return 0xff; // MAME unmapped bus value, not physical-board mirror speculation.
}
uint16_t Machine::read16(uint32_t a) { return uint16_t(uint16_t(read8(a)) << 8 | read8(a + 1)); }
uint32_t Machine::read32(uint32_t a) { return uint32_t(read16(a)) << 16 | read16(a + 2); }
void Machine::coin_write(unsigned bank, uint8_t value) {
    const uint8_t old = uint8_t(coin_word[bank] >> 8);
    coin_word[bank] = uint16_t(value << 8) | (coin_word[bank] & 0xff);
    for (unsigned i = 0; i < 2; ++i) {
        coin_locked[bank * 2 + i] = !(value & (1u << i));
        if ((value & (4u << i)) && !(old & (4u << i))) ++coin_count[bank * 2 + i];
    }
}
void Machine::write8(uint32_t a, uint8_t v) {
    a &= 0xffffff;
    if (a >= 0x400000 && a < 0x440000) { ram[a & 0x1ffff] = v; return; }
    if (a >= 0x440000 && a < 0x448000) { palette[a - 0x440000] = v; return; }
    if (a >= 0x600000 && a < 0x640000) {
        if (game_video) game_video->observe_write(cpu.pc, a);
        graphics[a - 0x600000] = v;
        return;
    }
    if (a >= 0x660000 && a < 0x660020) {
        if (game_video) game_video->observe_write(cpu.pc, a);
        control[a - 0x660000] = v; return;
    }
    if (a >= 0xc00000 && a < 0xc00800) {
        if (sound_trace) sound_trace->record(*this, SoundTrace::MainWrite, cpu.pc, a, v, 1);
        shared[a - 0xc00000] = v; return;
    }
    if ((a >= 0xc80000 && a <= 0xc80003) || (a >= 0xc80100 && a <= 0xc80103)) {
        if (sound_trace) sound_trace->record(*this, SoundTrace::MainWrite, cpu.pc, a, v, 1);
        audio->set_reset(a >= 0xc80100); return;
    }
    if (a >= 0x4a0000 && a <= 0x4a0003) { watchdog_at = cpu.cycles + 3ull * main_clock; return; }
    if (a == 0x4a0004 || a == 0x4a0014) { coin_write(a == 0x4a0014, v); return; }
    if (a == 0x4a0005 || a == 0x4a0015) { auto &word = coin_word[a == 0x4a0015]; word = uint16_t((word & 0xff00) | v); return; }
    if (a == 0x4a0013) { eeprom->pins(v, cpu.cycles); return; }
    if (a == 0x4c0000) timer_control = uint16_t((timer_control & 0xff) | (v << 8));
    if (a == 0x4c0001) timer_control = uint16_t((timer_control & 0xff00) | v);
    // MAME records this timer control but does not assert timer IRQ5.
}
void Machine::write16(uint32_t a, uint16_t v) { write8(a, uint8_t(v >> 8)); write8(a + 1, uint8_t(v)); }
void Machine::write32(uint32_t a, uint32_t v) { write16(a, uint16_t(v >> 16)); write16(a + 2, uint16_t(v)); }
void Machine::advance_to(uint64_t cycles) {
    if (cycles < hardware_cycles) throw std::runtime_error("CPU cycle clock moved backwards");
    while (hardware_cycles < cycles) {
        const uint64_t event = std::min(next_vblank, irq3_at);
        const uint64_t end = std::min(cycles, event);
        audio->advance(uint32_t(end - hardware_cycles));
        hardware_cycles = end;
        if (hardware_cycles == next_vblank) {
            if (game_video) game_video->render_frame();
            else video->render_frame(palette, graphics, control, pixels);
            pending_irqs |= 1u << 2;
            irq3_at = next_vblank + 10000;
            ++frame;
            next_vblank = raster_cycle((frame + 1) * frame_pixels);
        }
        if (hardware_cycles == irq3_at) { pending_irqs |= 1u << 3; irq3_at = UINT64_MAX; }
    }
}
int Machine::boundary() {
    cpu.dispatch_deadline = 0;
    advance_to(cpu.cycles);
    if (cpu.halted) return 1;
    for (int level = 7; level > int((cpu.sr >> 8) & 7); --level) {
        if (pending_irqs & (1u << level)) {
            pending_irqs &= uint8_t(~(1u << level));
            const uint16_t old_sr = cpu.sr;
            const uint32_t old_pc = cpu.pc;
            f3_exception(&cpu, 24 + unsigned(level), old_pc);
            if (old_sr & 0x1000) {
                // 68020 interrupt from master mode: additional format-1 frame on ISP.
                f3_set_sr(&cpu, uint16_t(cpu.sr & ~0x1000));
                cpu.a[7] -= 8;
                write16(cpu.a[7], uint16_t(old_sr | 0x2000));
                write32(cpu.a[7] + 2, old_pc);
                write16(cpu.a[7] + 6, uint16_t(0x1000 | ((24 + level) * 4)));
            }
            f3_set_sr(&cpu, uint16_t((cpu.sr & ~0x0700) | (level << 8)));
            if (!cpu.pc) cpu.pc = read32(cpu.vbr + 15 * 4);
            return 1;
        }
    }
    if (cpu.cycles >= watchdog_at) {
        reset_devices();
        audio->reset_board();
        if (sound_trace) sound_trace->record(*this, SoundTrace::Reset, cpu.pc, 0, 2, 0);
        interpreter->reset_main();
        return 1;
    }
    const uint64_t next_event = std::min({next_vblank, irq3_at, watchdog_at});
    if (cpu.stopped) {
        cpu.cycles = next_event;
        advance_to(cpu.cycles);
        return 1;
    }
    cpu.dispatch_deadline = next_event;
    return 0;
}
int Machine::fallback() {
    if (cpu.halted) return 0;
    if (!allow_main_fallback) {
        std::ostringstream message;
        message << "Untranslated main CPU instruction at PC 0x" << std::hex << cpu.pc;
        throw std::runtime_error(message.str());
    }
    ++fallback_instructions;
    if (!fallback_hits.empty()) ++fallback_hits[(cpu.pc & 0xffffff) >> 1];
    return interpreter->run_main(1) > 0 && !cpu.halted;
}
bool Machine::run_frame(bool translated) {
    const uint64_t target = frame + 1;
    while (frame < target && !cpu.halted) {
        if (translated) { if (!f3_dispatch(&cpu)) return false; }
        else if (!boundary()) {
            // Reference execution must hand MMIO/IRQ changes back at every
            // instruction boundary. Coarse slices alter the ROM boot checks.
            interpreter->run_main(1);
        }
    }
    return !cpu.halted;
}
void Machine::load_eeprom(const std::filesystem::path &p) { eeprom->load(p); }
void Machine::save_eeprom(const std::filesystem::path &p) const { eeprom->save(p); }
void Machine::set_input(unsigned port, uint32_t mask, bool pressed) {
    if (port >= inputs.size()) throw std::out_of_range("Input port index");
    if (pressed) inputs[port] &= ~mask; else inputs[port] |= mask;
}
}
