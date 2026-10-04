#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
namespace f3rt {
// 93C46, 64 x 16 organization. DO read dummy bit and sequential reads included.
class Eeprom {
public:
    std::array<uint16_t, 64> words;
    Eeprom() { words.fill(0xffff); }
    // Absolute 16 MHz main-clock ticks. Programming completion is independent
    // of serial clock edges; DO reports busy when CS is raised to poll it.
    bool output(uint64_t now) const {
        return selected && mode == Mode::Command && count == 0 ? now >= ready_at : data_out;
    }
    void reset() {
        selected = old_clock = writable = false;
        ready_at = 0;
        reset_command();
    }
    void pins(uint8_t pins, uint64_t now) {
        const bool select = pins & 0x10, clock = pins & 0x08, bit = pins & 0x04;
        if (!select) { selected = false; old_clock = clock; reset_command(); return; }
        if (!selected) { selected = true; reset_command(); }
        if (clock && !old_clock) edge(bit, now);
        old_clock = clock;
    }
    void load(const std::filesystem::path &path) {
        if (!std::filesystem::exists(path)) return;
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f || f.tellg() != 128) throw std::runtime_error("Invalid EEPROM file: " + path.string());
        f.seekg(0);
        std::array<uint8_t, 128> data{};
        if (!f.read(reinterpret_cast<char *>(data.data()), data.size())) throw std::runtime_error("EEPROM read failed");
        for (size_t i = 0; i < 64; ++i) words[i] = uint16_t(data[2*i] << 8 | data[2*i+1]);
    }
    void save(const std::filesystem::path &path) const {
        std::array<uint8_t, 128> data{};
        for (size_t i = 0; i < 64; ++i) { data[2*i] = uint8_t(words[i] >> 8); data[2*i+1] = uint8_t(words[i]); }
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f.write(reinterpret_cast<const char *>(data.data()), data.size())) throw std::runtime_error("EEPROM write failed");
    }
private:
    enum class Mode { Command, Read, Write, WriteAll, Done } mode = Mode::Command;
    bool selected = false, old_clock = false, data_out = true, writable = false;
    uint32_t shift = 0;
    unsigned count = 0, address = 0, read_bit = 0;
    uint64_t ready_at = 0;
    void reset_command() { mode = Mode::Command; shift = count = 0; data_out = true; }
    void edge(bool bit, uint64_t now) {
        if (mode == Mode::Read) {
            data_out = (words[address] >> (15 - read_bit)) & 1;
            if (++read_bit == 16) { read_bit = 0; address = (address + 1) & 63; }
            return;
        }
        if (mode == Mode::Done) return;
        if (mode == Mode::Command && !count && (!bit || now < ready_at)) return;
        shift = (shift << 1) | unsigned(bit); ++count;
        if (mode == Mode::Write || mode == Mode::WriteAll) {
            if (count == 16) {
                if (writable) {
                    if (mode == Mode::WriteAll) words.fill(uint16_t(shift));
                    else words[address] = uint16_t(shift);
                    ready_at = now + (mode == Mode::WriteAll ? 128000 : 28000);
                }
                mode = Mode::Done; data_out = true;
            }
            return;
        }
        if (count != 9) return;
        address = shift & 63;
        switch ((shift >> 6) & 3) {
        case 2: mode = Mode::Read; read_bit = 0; data_out = false; break;
        case 1: mode = Mode::Write; count = shift = 0; break;
        case 3:
            if (writable) { words[address] = 0xffff; ready_at = now + 16000; }
            mode = Mode::Done;
            break;
        case 0:
            switch (address >> 4) {
            case 0: writable = false; mode = Mode::Done; break;
            case 1: mode = Mode::WriteAll; count = shift = 0; break;
            case 2:
                if (writable) { words.fill(0xffff); ready_at = now + 128000; }
                mode = Mode::Done;
                break;
            case 3: writable = true; mode = Mode::Done; break;
            }
        }
    }
};
}
