#pragma once
#include "f3rt/memory_map.hpp"
#include <cstdint>
#include <filesystem>
#include <fstream>

namespace f3rt {
class Machine;

// Lossless bus observation; decoding is offline and never reads device registers.
// File: 8-byte header "F3SND2\0\0", then 32-byte little-endian records:
//   0 u64 time (main CPU cycles for MainWrite, else audio clock_ticks())
//   8 u64 audio generated_frames()   16 u32 pc   20 u32 address   24 u32 value
//   28 u8 kind (Kind: MainWrite=1 SoundRead=2 SoundWrite=3 Reset=4 End=5)   29 u8 width   30-31 zero
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
