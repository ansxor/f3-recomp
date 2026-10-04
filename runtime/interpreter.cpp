#include "interpreter.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "m68k.h"
#include <mutex>

extern "C" {
void f3rt_core_import(const f3_cpu *cpu);
void f3rt_core_export(f3_cpu *cpu);
}
namespace {
f3rt::Machine *active_machine = nullptr;
bool sound_bus = false;
std::once_flag init_flag;
int acknowledge(int level) {
    return sound_bus ? active_machine->audio->irq_ack(level) : int(M68K_INT_ACK_AUTOVECTOR);
}
void reset_devices() {
    if (!sound_bus) active_machine->reset_devices();
}
void bind(f3rt::Machine &machine, bool sound) {
    active_machine = &machine;
    sound_bus = sound;
}
void callbacks() {
    m68k_set_int_ack_callback(acknowledge);
    m68k_set_reset_instr_callback(reset_devices);
}
}
extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) { return sound_bus ? active_machine->audio->read8(a) : active_machine->read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return sound_bus ? active_machine->audio->read16(a) : active_machine->read16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return sound_bus ? active_machine->audio->read32(a) : active_machine->read32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { if (sound_bus) active_machine->audio->write8(a,uint8_t(v)); else active_machine->write8(a,uint8_t(v)); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { if (sound_bus) active_machine->audio->write16(a,uint16_t(v)); else active_machine->write16(a,uint16_t(v)); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { if (sound_bus) active_machine->audio->write32(a,v); else active_machine->write32(a,v); }
}
namespace f3rt {
Interpreter::Interpreter(Machine &m) : machine(m) {
    std::call_once(init_flag, [] { m68k_init(); });
    main_context.resize((m68k_context_size() + 7) / 8);
    sound_context.resize(main_context.size());
}
void Interpreter::reset_main() {
    bind(machine, false);
    m68k_set_context(main_context.data());
    m68k_set_cpu_type(M68K_CPU_TYPE_68EC020);
    callbacks();
    m68k_pulse_reset();
    f3rt_core_export(&machine.cpu);
    m68k_get_context(main_context.data());
}
void Interpreter::audio_reset(bool asserted) {
    if (!asserted) sound_needs_reset = true;
}
int Interpreter::run_main(int cycles) {
    bind(machine, false);
    m68k_set_context(main_context.data());
    f3rt_core_import(&machine.cpu);
    m68k_set_irq(0); // Main IRQs enter via the shared ABI boundary.
    const int used = m68k_execute(cycles);
    f3rt_core_export(&machine.cpu);
    m68k_get_context(main_context.data());
    machine.cpu.cycles += unsigned(used);
    return used;
}
int Interpreter::run_audio(int cycles) {
    bind(machine, true);
    m68k_set_context(sound_context.data());
    if (sound_needs_reset) {
        m68k_set_cpu_type(M68K_CPU_TYPE_68000);
        callbacks();
        m68k_pulse_reset();
        sound_needs_reset = false;
    }
    m68k_set_irq(unsigned(machine.audio->irq_level()));
    const int used = m68k_execute(cycles);
    m68k_get_context(sound_context.data());
    bind(machine, false);
    return used;
}
uint32_t Interpreter::sound_pc() const {
    return m68k_get_reg(const_cast<uint64_t *>(sound_context.data()), M68K_REG_PC);
}
}
