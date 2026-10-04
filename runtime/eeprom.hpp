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
    bool output() const { return data_out; }
    void pins(uint8_t pins) {
        const bool select = pins & 0x10, clock = pins & 0x08, bit = pins & 0x04;
        if (!select) { selected = false; old_clock = clock; reset_command(); return; }
        if (!selected) { selected = true; reset_command(); }
        if (clock && !old_clock) edge(bit);
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
    void reset_command() { mode = Mode::Command; shift = count = 0; data_out = true; }
    void edge(bool bit) {
        if (mode == Mode::Read) {
            data_out = (words[address] >> (15 - read_bit)) & 1;
            if (++read_bit == 16) { read_bit = 0; address = (address + 1) & 63; }
            return;
        }
        if (mode == Mode::Done) return;
        if (mode == Mode::Command && !count && !bit) return;
        shift = (shift << 1) | unsigned(bit); ++count;
        if (mode == Mode::Write || mode == Mode::WriteAll) {
            if (count == 16) {
                if (writable) { if (mode == Mode::WriteAll) words.fill(uint16_t(shift)); else words[address] = uint16_t(shift); }
                mode = Mode::Done; data_out = true;
            }
            return;
        }
        if (count != 9) return;
        address = shift & 63;
        switch ((shift >> 6) & 3) {
        case 2: mode = Mode::Read; read_bit = 0; data_out = false; break;
        case 1: mode = Mode::Write; count = shift = 0; break;
        case 3: if (writable) words[address] = 0xffff; mode = Mode::Done; break;
        case 0:
            switch (address >> 4) {
            case 0: writable = false; mode = Mode::Done; break;
            case 1: mode = Mode::WriteAll; count = shift = 0; break;
            case 2: if (writable) words.fill(0xffff); mode = Mode::Done; break;
            case 3: writable = true; mode = Mode::Done; break;
            }
        }
    }
};
}
