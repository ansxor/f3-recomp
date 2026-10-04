#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace f3rt {
class Machine;
// Lossless bus observation; decoding is offline and never reads device registers.
class SoundTrace {
public:
    enum Kind : uint8_t { MainWrite=1, SoundRead=2, SoundWrite=3, Reset=4, End=5,
                          VoiceContext=6, RamSnapshot=7, NoteContext=8, DirectNote=9, NoteEvent=10 };
    explicit SoundTrace(const std::filesystem::path &path);
    void record(const Machine &machine, Kind kind, uint32_t pc, uint32_t address,
                uint32_t value, uint8_t width);
    void voice_context(const Machine &machine, uint32_t pc, uint32_t voice_address);
    void note_context(const Machine &machine, uint32_t pc, uint32_t note, uint32_t channel,
                      uint32_t track, uint32_t event);
    void finish(const Machine &machine);
private:
    void ram_snapshot(const Machine &machine, uint32_t pc, uint32_t address, uint32_t bytes);
    std::ofstream output;
};
}
