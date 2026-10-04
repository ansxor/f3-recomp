#pragma once
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace f3rt {
class Machine;
// Lossless bus observation; decoding is offline and never reads device registers.
class SoundTrace {
public:
    enum Kind : uint8_t { MainWrite=1, SoundRead=2, SoundWrite=3, Reset=4, End=5 };
    explicit SoundTrace(const std::filesystem::path &path);
    void record(const Machine &machine, Kind kind, uint32_t pc, uint32_t address,
                uint32_t value, uint8_t width);
    void finish(const Machine &machine);
private:
    std::ofstream output;
};
}
