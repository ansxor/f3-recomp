#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace f3rt {
struct RomSet {
    std::string name;
    std::vector<uint8_t> main, sprites, sprites_hi, tiles, tiles_hi, sound, samples;
    // Directory contains extracted MAME filenames. Each chip is CRC-validated.
    static RomSet load(const std::filesystem::path &directory, const std::string &set = "landmakrj");
};
uint32_t crc32(const uint8_t *data, size_t size);
}
