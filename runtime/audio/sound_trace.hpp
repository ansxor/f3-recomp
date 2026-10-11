#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace f3rt {
class Machine;
// Sound trace bus monitoring range [SOUND_TRACE_BUS_BEGIN, SOUND_TRACE_BUS_END).
// Captures sound hardware registers, ES5505/ES5510, DUART, volume, and mailbox.
constexpr uint32_t SOUND_TRACE_BUS_BEGIN = 0x140000;
constexpr uint32_t SOUND_TRACE_BUS_END = 0x340004;

// Lossless bus observation; decoding is offline and never reads device registers.
class SoundTrace {
public:
    enum Kind : uint8_t { MainWrite = 1, SoundRead = 2, SoundWrite = 3, Reset = 4, End = 5 };
    explicit SoundTrace(const std::filesystem::path &path);
    void record(const Machine &machine, Kind kind, uint32_t pc, uint32_t address,
                uint32_t value, uint8_t width);
    void finish(const Machine &machine);
private:
    std::ofstream output;
};
}
