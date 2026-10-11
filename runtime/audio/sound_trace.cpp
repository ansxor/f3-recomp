#include "audio/sound_trace.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include <array>
#include <stdexcept>

namespace f3rt {
SoundTrace::SoundTrace(const std::filesystem::path &path) : output(path, std::ios::binary) {
    if (!output) throw std::runtime_error("Cannot open sound trace: " + path.string());
    output.write("F3SND2\0\0", 8);
}
void SoundTrace::record(const Machine &m, Kind kind, uint32_t pc, uint32_t address,
                        uint32_t value, uint8_t width) {
    std::array<char, 32> row{};
    auto put = [&](unsigned offset, uint64_t number, unsigned bytes) {
        for (unsigned i=0; i<bytes; ++i) row[offset+i]=char(number >> (i*8));
    };
    put(0, kind==MainWrite ? m.cpu.cycles : m.audio->clock_ticks(), 8);
    put(8, m.audio->generated_frames(), 8);
    put(16, pc, 4); put(20, address, 4); put(24, value, 4);
    row[28]=char(kind); row[29]=char(width);
    output.write(row.data(), row.size());
    if (!output) throw std::runtime_error("Sound trace write failed");
}
void SoundTrace::ram_snapshot(const Machine &m, uint32_t pc, uint32_t address, uint32_t bytes) {
    for (uint32_t offset=0; offset<bytes; offset+=4) {
        const uint32_t at=(address+offset)&0xffff;
        record(m, RamSnapshot, pc, at, m.audio->read32(0xff0000|at), 4);
    }
}
void SoundTrace::voice_context(const Machine &m, uint32_t pc, uint32_t voice_address) {
    // Only work-RAM mirrors are inspected; these reads have no device side effects.
    const uint32_t voice = voice_address & 0xffff;
    ram_snapshot(m, pc, voice, 0xac);
    ram_snapshot(m, pc, m.audio->read16(0xff0000|((voice+0x10)&0xffff)), 0x40);
    ram_snapshot(m, pc, m.audio->read16(0xff0000|((voice+0x8c)&0xffff)), 0x20);
    ram_snapshot(m, pc, 0xd81a, 0x28);
    record(m, VoiceContext, pc, voice, m.audio->read16(0xff0000|((voice+0x0c)&0xffff))&31, 0);
}
void SoundTrace::note_context(const Machine &m, uint32_t pc, uint32_t note, uint32_t channel,
                              uint32_t track, uint32_t event) {
    ram_snapshot(m, pc, channel, 0x38);
    ram_snapshot(m, pc, track, 0x10);
    ram_snapshot(m, pc, event, 0x18);
    ram_snapshot(m, pc, 0xd40e, 4);
    record(m, NoteEvent, pc, event&0xffff, 0, 0);
    record(m, NoteContext, pc, note&0xffff, ((track&0xffff)<<16)|(channel&0xffff), 0);
}
void SoundTrace::finish(const Machine &m) {
    record(m, End, 0, 0, 0, 0);
    output.flush();
    if (!output) throw std::runtime_error("Sound trace flush failed");
}
}
