#include "interpreter.hpp"
#include "f3rt/machine.hpp"
#include "m68k.h"
#include "state_io.hpp"

namespace {
f3rt::Machine *active_machine = nullptr;
// Musashi's tables initialize once per process. devkitARM's libstdc++ routes std::call_once
// through pthread_once, but its libpthread is a dummy, so the call throws on the 3DS; the
// interpreter is constructed from one thread at startup, so a plain guard is equivalent.
bool musashi_initialized = false;
int acknowledge(int) { return int(M68K_INT_ACK_AUTOVECTOR); }
void reset_devices() { active_machine->reset_devices(); }
void callbacks() {
    m68k_set_int_ack_callback(acknowledge);
    m68k_set_reset_instr_callback(reset_devices);
}
} // namespace
extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) { return active_machine->read8(a); }
unsigned int m68k_read_memory_16(unsigned int a) { return active_machine->read16(a); }
unsigned int m68k_read_memory_32(unsigned int a) { return active_machine->read32(a); }
void m68k_write_memory_8(unsigned int a, unsigned int v) { active_machine->write8(a, uint8_t(v)); }
void m68k_write_memory_16(unsigned int a, unsigned int v) { active_machine->write16(a, uint16_t(v)); }
void m68k_write_memory_32(unsigned int a, unsigned int v) { active_machine->write32(a, v); }
} // extern "C"
namespace f3rt {
Interpreter::Interpreter(Machine &m) : m_machine(m) {
    if (!musashi_initialized) {
        m68k_init();
        musashi_initialized = true;
    }
    m_main_context.resize((m68k_context_size() + 7) / 8);
    // The main context keeps the 68EC020 type and interrupt callbacks. Main CPU
    // reset is native (Machine::reset_main_cpu); run_main imports canonical state.
    m68k_set_context(m_main_context.data());
    m68k_set_cpu_type(M68K_CPU_TYPE_68EC020);
    callbacks();
    m68k_get_context(m_main_context.data());
}
int Interpreter::run_main(int cycles) {
    active_machine = &m_machine;
    f3_cc_flush(&m_machine.cpu);
    m68k_set_context(m_main_context.data());
    f3rt_core_import(&m_machine.cpu);
    m68k_set_irq(0); // Main IRQs enter via the shared ABI boundary.
    const int used = m68k_execute(cycles);
    f3rt_core_export(&m_machine.cpu);
    m68k_get_context(m_main_context.data());
    m_machine.cpu.cycles += unsigned(used);
    return used;
}
void Interpreter::sync_main_from_cpu() {
    active_machine = &m_machine;
    m68k_set_context(m_main_context.data());
    f3rt_core_import(&m_machine.cpu);
    callbacks();
    m68k_get_context(m_main_context.data());
}
} // namespace f3rt
