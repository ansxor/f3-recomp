#pragma once
#include "f3rt/machine.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <fstream>
#include <stdexcept>
#include <vector>

namespace f3rt {
// Local slots retain exact expanded presentation history.
// Loading a slot requires the same ROM set, scale and border.
inline void frontend_state_slot(Machine &machine,
                                const std::filesystem::path &path, unsigned scale,
                                unsigned border, bool save) {
    // Layout: magic[0,8) ROM CRCs[8,36) state size[36,40) state CRC[40,44) scale[44,48) border[48,52).
    constexpr size_t header_size = 52;
    std::array<uint8_t, header_size> header{};
    constexpr std::array<uint8_t, 8> magic{'F','3','S','L','O','T','0','2'};
    std::copy(magic.begin(), magic.end(), header.begin());
    auto put = [&](size_t offset, uint32_t value) {
        for (unsigned i = 0; i < 4; ++i) header[offset + i] = uint8_t(value >> (24 - i * 8));
    };
    const std::vector<uint8_t> *regions[] = {&machine.roms.main, &machine.roms.sprites, &machine.roms.sprites_hi,
        &machine.roms.tiles, &machine.roms.tiles_hi, &machine.roms.sound, &machine.roms.samples};
    for (size_t i = 0; i < 7; ++i) put(8 + i * 4, crc32(regions[i]->data(), regions[i]->size()));
    if (machine.state_size() > UINT32_MAX) throw std::runtime_error("Save state exceeds file format limit");
    put(36, uint32_t(machine.state_size()));
    put(44, scale); put(48, border);
    if (save) {
        std::vector<uint8_t> bytes(machine.state_size());
        machine.save_state(bytes);
        put(40, crc32(bytes.data(), bytes.size()));
        if (!path.parent_path().empty()) std::filesystem::create_directories(path.parent_path());
        auto temporary = path;
        temporary += ".tmp." + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
        try {
            std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
            out.write(reinterpret_cast<const char *>(header.data()), header.size());
            out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
            out.close();
            if (!out) throw std::runtime_error("Cannot write save-state slot: " + path.string());
            std::filesystem::rename(temporary, path);
        } catch (...) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            throw;
        }
        return;
    }
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in || in.tellg() != std::streamoff(header_size + machine.state_size()))
        throw std::runtime_error("Save-state slot is missing or has an incompatible size: " + path.string());
    in.seekg(0);
    std::array<uint8_t, header_size> received{};
    in.read(reinterpret_cast<char *>(received.data()), received.size());
    if (!in || !std::equal(header.begin(), header.begin() + 40, received.begin()) ||
        !std::equal(header.begin() + 44, header.end(), received.begin() + 44))
        throw std::runtime_error("Save-state ROM/scale/border does not match this session");
    std::vector<uint8_t> bytes(machine.state_size()), previous(machine.state_size());
    in.read(reinterpret_cast<char *>(bytes.data()), bytes.size());
    uint32_t checksum = 0;
    for (unsigned i = 0; i < 4; ++i) checksum = (checksum << 8) | received[40 + i];
    if (!in || crc32(bytes.data(), bytes.size()) != checksum)
        throw std::runtime_error("Save-state checksum mismatch");
    machine.save_state(previous);
    try { machine.load_state(bytes); }
    catch (...) { machine.load_state(previous); throw; }
}
} // namespace f3rt
