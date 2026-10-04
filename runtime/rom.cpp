#include "f3rt/rom.hpp"
#include <array>
#include <fstream>
#include <stdexcept>

namespace f3rt {
uint32_t crc32(const uint8_t *data, size_t size) {
    uint32_t crc = ~0u;
    for (size_t i = 0; i < size; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (0xedb88320u & (0u - (crc & 1)));
    }
    return ~crc;
}
namespace {
std::vector<uint8_t> chip(const std::filesystem::path &dir, const char *name, size_t size,
                          uint32_t crc, uint32_t short_crc = 0) {
    const auto path = dir / name;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open ROM: " + path.string());
    const auto length = input.tellg();
    if (length < 0 || (size_t(length) != size && !(short_crc && size_t(length) == size / 2)))
        throw std::runtime_error("Wrong ROM length: " + path.string());
    std::vector<uint8_t> bytes(size_t(length), 0);
    input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), length))
        throw std::runtime_error("Cannot read ROM: " + path.string());
    const bool short_dump = bytes.size() != size;
    if (crc32(bytes.data(), bytes.size()) != (short_dump ? short_crc : crc))
        throw std::runtime_error("ROM CRC mismatch: " + path.string());
    if (short_dump) {
        bytes.resize(size, 0xff);
        if (crc32(bytes.data(), bytes.size()) != crc)
            throw std::runtime_error("Padded sound ROM CRC mismatch: " + path.string());
    }
    return bytes;
}
void lane(std::vector<uint8_t> &region, const std::vector<uint8_t> &bytes,
          size_t offset, size_t stride, size_t group = 1) {
    for (size_t i = 0; i < bytes.size(); ++i)
        region[offset + (i / group) * stride + i % group] = bytes[i];
}
}
RomSet RomSet::load(const std::filesystem::path &dir, const std::string &set) {
    if (set != "landmakrj" && set != "landmakr")
        throw std::runtime_error("Unsupported ROM set: " + set);
    RomSet r;
    r.name = set;
    r.main.resize(0x200000);
    const std::array<const char *, 4> names = set == "landmakrj"
        ? std::array<const char *, 4>{"e61-13.20", "e61-12.19", "e61-11.18", "e61-10.17"}
        : std::array<const char *, 4>{"e61-19.20", "e61-18.19", "e61-17.18", "e61-16.17"};
    const std::array<uint32_t, 4> crcs = set == "landmakrj"
        ? std::array<uint32_t, 4>{0x0af756a2, 0x636b3df9, 0x279a0ee4, 0xdaabf2b2}
        : std::array<uint32_t, 4>{0xf92eccd0, 0x5a26c9e0, 0x710776a8, 0xb073cda9};
    for (size_t i = 0; i < 4; ++i) lane(r.main, chip(dir, names[i], 0x80000, crcs[i]), i, 4);
    r.sprites.resize(0x400000);
    lane(r.sprites, chip(dir, "e61-03.12", 0x200000, 0xe8abfc46), 0, 2);
    lane(r.sprites, chip(dir, "e61-02.08", 0x200000, 0x1dc4a164), 1, 2);
    r.sprites_hi = chip(dir, "e61-01.04", 0x200000, 0x6cdd8311);
    r.tiles.resize(0x400000);
    lane(r.tiles, chip(dir, "e61-09.47", 0x200000, 0x6ba29987), 0, 4, 2);
    lane(r.tiles, chip(dir, "e61-08.45", 0x200000, 0x76c98e14), 2, 4, 2);
    r.tiles_hi = chip(dir, "e61-07.43", 0x200000, 0x4a57965d);
    // Only the mapped sound program, excluding MAME's unused 0x100000 prefix.
    r.sound.resize(0x80000, 0xff);
    lane(r.sound, chip(dir, "e61-14.32", 0x40000, 0x18961bbb, 0xb905f4a7), 0, 2);
    lane(r.sound, chip(dir, "e61-15.33", 0x40000, 0x2c64557a, 0x87909869), 1, 2);
    r.samples.resize(0x1000000, 0);
    lane(r.samples, chip(dir, "e61-04.38", 0x200000, 0xc27aec0c), 0x400000, 2);
    lane(r.samples, chip(dir, "e61-05.39", 0x200000, 0x83920d9d), 0x800000, 2);
    lane(r.samples, chip(dir, "e61-06.40", 0x200000, 0x2e717bfe), 0xc00000, 2);
    return r;
}
}
