#pragma once
#include <cstdint>
#include <vector>
namespace f3rt {
class StateWriter;
class StateReader;
class Machine;
// Musashi's global core is serialized on the emulation thread. Contexts are
// preallocated; audio execution never nests inside a main CPU bus callback.
class Interpreter {
public:
    explicit Interpreter(Machine &machine);
    void audio_reset(bool asserted);
    void audio_irq(bool asserted);
    int run_main(int cycles);
    int run_audio(int cycles);
    uint32_t sound_pc() const;
    size_t sound_state_size() const;
    void save_sound_state(StateWriter &writer) const;
    void load_sound_state(StateReader &reader);
    void sync_main_from_cpu();

private:
    Machine &m_machine;
    std::vector<uint64_t> m_main_context, m_sound_context;
    bool m_sound_needs_reset = true;
};
} // namespace f3rt
