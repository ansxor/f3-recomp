#pragma once
#include <cstdint>
#include <vector>
namespace f3rt {
class Machine;
// Musashi's global core is serialized on the emulation thread. Contexts are
// preallocated; audio execution never nests inside a main CPU bus callback.
class Interpreter {
public:
    explicit Interpreter(Machine &machine);
    void reset_main();
    void audio_reset(bool asserted);
    int run_main(int cycles);
    int run_audio(int cycles);
    uint32_t sound_pc() const;
private:
    Machine &machine;
    std::vector<uint64_t> main_context, sound_context;
    bool sound_needs_reset = true;
};
}
