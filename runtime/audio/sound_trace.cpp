#include "audio/sound_trace.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include <array>
#include <stdexcept>

namespace f3rt {
SoundTrace::SoundTrace(const std::filesystem::path &path) : m_output(path, std::ios::binary) {
    if (!m_output) throw std::runtime_error("Cannot open sound trace: " + path.string());
    m_output.write("F3SND2\0\0", 8);
}
void SoundTrace::record(const Machine &m, Kind kind, uint32_t pc, uint32_t address, uint32_t value,
                        uint8_t width) {
    std::array<char, 32> row{};
    auto put = [&](unsigned offset, uint64_t number, unsigned bytes) {
        for (unsigned i = 0; i < bytes; ++i) row[offset + i] = char(number >> (i * 8));
    };
    put(0, kind == MainWrite ? m.cpu.cycles : m.audio->clock_ticks(), 8);
    put(8, m.audio->generated_frames(), 8);
    put(16, pc, 4);
    put(20, address, 4);
    put(24, value, 4);
    row[28] = char(kind);
    row[29] = char(width);
    m_output.write(row.data(), row.size());
    if (!m_output) throw std::runtime_error("Sound trace write failed");
}
void SoundTrace::finish(const Machine &m) {
    record(m, End, 0, 0, 0, 0);
    m_output.flush();
    if (!m_output) throw std::runtime_error("Sound trace flush failed");
}
} // namespace f3rt
