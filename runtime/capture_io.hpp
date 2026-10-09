#pragma once
#include "f3rt/machine.hpp"
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace f3rt {
inline void read_exact(const std::filesystem::path &path, std::span<uint8_t> out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f || f.tellg() != std::streamoff(out.size())) throw std::runtime_error("Wrong capture size: " + path.string());
    f.seekg(0);
    if (!f.read(reinterpret_cast<char *>(out.data()), out.size())) throw std::runtime_error("Capture read failed: " + path.string());
}
inline void write_bytes(const std::filesystem::path &path, std::span<const uint8_t> data) {
    std::ofstream f(path, std::ios::binary);
    if (!f.write(reinterpret_cast<const char *>(data.data()), data.size())) throw std::runtime_error("Capture write failed: " + path.string());
}
inline void le16(std::ostream &out, uint16_t value) { out.put(char(value)); out.put(char(value >> 8)); }
inline void le32(std::ostream &out, uint32_t value) { le16(out,uint16_t(value)); le16(out,uint16_t(value >> 16)); }
inline void write_argb(const std::filesystem::path &path, std::span<const uint32_t> pixels) {
    std::ofstream f(path, std::ios::binary);
    for (auto p : pixels) le32(f,p);
    if (!f) throw std::runtime_error("Frame write failed: " + path.string());
}
inline void write_bmp(const std::filesystem::path &path, std::span<const uint32_t> pixels) {
    std::ofstream f(path, std::ios::binary);
    const uint32_t height=uint32_t(pixels.size()/320), bytes=uint32_t(pixels.size()*4);
    f.write("BM",2); le32(f,54+bytes); le32(f,0); le32(f,54);
    le32(f,40); le32(f,320); le32(f,0u-height); le16(f,1); le16(f,32);
    le32(f,0); le32(f,bytes); le32(f,2835); le32(f,2835); le32(f,0); le32(f,0);
    for (auto p : pixels) le32(f,p);
    if (!f) throw std::runtime_error("BMP write failed: " + path.string());
}
inline std::string frame_name(uint64_t frame) {
    std::ostringstream name;
    name << "frame_" << std::setfill('0') << std::setw(4) << frame;
    return name.str();
}
inline void dump_machine(const Machine &m, const std::filesystem::path &root) {
    const auto dir = root / frame_name(m.frame);
    std::filesystem::create_directories(dir);
    write_bytes(dir/"palette.bin",m.palette);
    write_bytes(dir/"graphics.bin",m.graphics);
    write_bytes(dir/"control.bin",m.control);
    write_bytes(dir/"mainram.bin",m.ram);
    write_bytes(dir/"shared.bin",m.shared);
    write_argb(dir/"rendered.argb",m.native_pixels());
    write_bmp(dir/"rendered.bmp",m.native_pixels());
    if (m.sprite_writers) {
        std::ofstream sw(dir/"sprite_writers.bin", std::ios::binary);
        for (uint32_t pc : *m.sprite_writers) le32(sw, pc);
        if (!sw) throw std::runtime_error("Sprite writers write failed");
    }
    std::ofstream state(dir/"cpu.json");
    state << "{\"frame\":" << m.frame << ",\"cycles\":" << m.cpu.cycles
          << ",\"pc\":" << m.cpu.pc << ",\"sr\":" << m.cpu.sr << ",\"d\":[";
    for (int i=0;i<8;++i) state << (i ? "," : "") << m.cpu.d[i];
    state << "],\"a\":[";
    for (int i=0;i<8;++i) state << (i ? "," : "") << m.cpu.a[i];
    state << "]}\n";
    if (!state) throw std::runtime_error("CPU state write failed");
}
class WavWriter {
    std::fstream out;
    uint32_t rate = 0, bytes = 0;
public:
    WavWriter(const std::filesystem::path &path, uint32_t sample_rate) : out(path,std::ios::binary|std::ios::out|std::ios::in|std::ios::trunc),rate(sample_rate) {
        if (!out) throw std::runtime_error("WAV open failed");
        header();
    }
    void header() {
        out.seekp(0); out.write("RIFF",4); le32(out,36+bytes); out.write("WAVEfmt ",8);
        le32(out,16); le16(out,1); le16(out,2); le32(out,rate); le32(out,rate*4);
        le16(out,4); le16(out,16); out.write("data",4); le32(out,bytes);
    }
    void append(std::span<const int16_t> samples) {
        for (auto v:samples) le16(out,uint16_t(v));
        bytes += uint32_t(samples.size()*2);
        if (!out) throw std::runtime_error("WAV write failed");
    }
    ~WavWriter() { header(); }
};
}
