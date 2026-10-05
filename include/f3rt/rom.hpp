#pragma once
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>
namespace f3rt {
struct VideoConfig {
    unsigned rotation = 0;
    unsigned sprite_lag = 1;
    unsigned visible_y = 24;
    unsigned visible_height = 232;
    bool extend = true;
    // Immutable ROM layout capability: extended PF2/PF3 alternates are maps 4/5.
    bool extended_alt_maps = false;
};
struct RomSet {
    std::string name;
    VideoConfig video;
    std::vector<uint8_t> main, sprites, sprites_hi, tiles, tiles_hi, sound, samples;
    // Optional immutable 128-byte factory image; guest writes never alter ROM assets.
    std::vector<uint8_t> factory_eeprom;
    // Directory contains extracted MAME filenames. Each chip is CRC/SHA1-validated.
    static RomSet load(const std::filesystem::path &directory, const std::string &set = "landmakrj");
};
uint32_t crc32(const uint8_t *data, size_t size);
}
