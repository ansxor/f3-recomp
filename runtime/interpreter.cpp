#include "interpreter.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "m68k.h"
#include "state_io.hpp"
#include "state_oracle.h"
#include "sound_trace.hpp"
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
void trace_sound(uint32_t address, uint32_t value, uint8_t width, bool write) {
    if (!active_machine->sound_trace) return;
    const uint32_t pc=m68k_get_reg(nullptr, M68K_REG_PPC);
    if (write && width==1 && pc==0xc130f0 &&
        address==((m68k_get_reg(nullptr,M68K_REG_A4)+0x14u)&0xffffffu))
        active_machine->sound_trace->record(*active_machine, f3rt::SoundTrace::DirectNote,
                                            pc,address,value,width);
    if (write && width==2 && pc==0xc140e4 &&
        address==((m68k_get_reg(nullptr,M68K_REG_A1)+0x24u)&0xffffffu))
        active_machine->sound_trace->note_context(*active_machine, pc,
            m68k_get_reg(nullptr,M68K_REG_A5),m68k_get_reg(nullptr,M68K_REG_A1),
            m68k_get_reg(nullptr,M68K_REG_A6),m68k_get_reg(nullptr,M68K_REG_A4));
    if (write && width==2 && pc==0xc141d6 &&
        address==((m68k_get_reg(nullptr,M68K_REG_A5)+2u)&0xffffffu))
        active_machine->sound_trace->record(*active_machine, f3rt::SoundTrace::NoteRelease,
            pc,m68k_get_reg(nullptr,M68K_REG_A5)&0xffffu,value,width);
    // PPC can name the interrupted instruction during IRQ stack writes.
    if (write && width==2 &&
        (((pc==0xc17e62 || pc==0xc17806) && address==0x20001e) ||
         (pc==0xc17632 && address==((m68k_get_reg(nullptr,M68K_REG_A4)+0xau)&0xffffffu))))
        active_machine->sound_trace->voice_context(*active_machine, pc, m68k_get_reg(nullptr, M68K_REG_A4));
    if (address < 0x140000 || address >= 0x340004) return;
    active_machine->sound_trace->record(*active_machine,
        write ? f3rt::SoundTrace::SoundWrite : f3rt::SoundTrace::SoundRead,
        pc, address, value, width);
}
}
extern "C" {
unsigned int m68k_read_memory_8(unsigned int a) {
    if (!sound_bus) return active_machine->read8(a);
    const auto value=active_machine->audio->read8(a); trace_sound(a,value,1,false); return value;
}
unsigned int m68k_read_memory_16(unsigned int a) {
    if (!sound_bus) return active_machine->read16(a);
    const auto value=active_machine->audio->read16(a); trace_sound(a,value,2,false); return value;
}
unsigned int m68k_read_memory_32(unsigned int a) {
    if (!sound_bus) return active_machine->read32(a);
    const auto value=active_machine->audio->read32(a); trace_sound(a,value,4,false); return value;
}
void m68k_write_memory_8(unsigned int a, unsigned int v) {
    if (!sound_bus) { active_machine->write8(a,uint8_t(v)); return; }
    trace_sound(a,uint8_t(v),1,true); active_machine->audio->write8(a,uint8_t(v));
}
void m68k_write_memory_16(unsigned int a, unsigned int v) {
    if (!sound_bus) { active_machine->write16(a,uint16_t(v)); return; }
    trace_sound(a,uint16_t(v),2,true); active_machine->audio->write16(a,uint16_t(v));
}
void m68k_write_memory_32(unsigned int a, unsigned int v) {
    if (!sound_bus) { active_machine->write32(a,v); return; }
    trace_sound(a,v,4,true); active_machine->audio->write32(a,v);
}
} // extern "C"
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
    // RESET preserves general registers and CCR: generated execution may have
    // advanced them since the last interpreter context was saved.
    f3rt_core_import(&machine.cpu);
    callbacks();
    m68k_pulse_reset();
    // Drain reset latency before canonical imports can discard it. A budget
    // below the 020's four reset cycles returns without executing an opcode.
    machine.cpu.cycles += unsigned(m68k_execute(1));
    f3rt_core_export(&machine.cpu);
    m68k_get_context(main_context.data());
}
void Interpreter::audio_reset(bool asserted) {
    if (!asserted) sound_needs_reset = true;
}
void Interpreter::audio_irq(bool asserted) {
    // DUART acknowledge/IMR writes can lower IRQ during m68k_execute. Waiting
    // until the next slice causes a second, spurious interrupt after RTE.
    if (active_machine == &machine && sound_bus) m68k_set_irq(asserted ? 6 : 0);
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
size_t Interpreter::sound_state_size() const {
    return sizeof(f3rt_sound_oracle_state);
}

void Interpreter::save_sound_state(StateWriter &writer) const {
    f3rt_sound_oracle_state st{};
    f3rt_sound_core_export(sound_context.data(), &st);
    st.sound_needs_reset = sound_needs_reset ? 1 : 0;
    writer.write(st);
}

void Interpreter::load_sound_state(StateReader &reader) {
    f3rt_sound_oracle_state st;
    reader.read(st);
    f3rt_sound_core_import(sound_context.data(), &st);
    sound_needs_reset = st.sound_needs_reset != 0;
    if (!sound_needs_reset) {
        bind(machine, true);
        m68k_set_context(sound_context.data());
        callbacks();
        m68k_get_context(sound_context.data());
        bind(machine, false);
    }
}

void Interpreter::sync_main_from_cpu() {
    bind(machine, false);
    m68k_set_context(main_context.data());
    f3rt_core_import(&machine.cpu);
    callbacks();
    m68k_get_context(main_context.data());
}
}
