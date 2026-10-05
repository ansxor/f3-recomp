#include "f3rt/rom.hpp"
#include "rom_manifest.hpp"
#include <algorithm>
#include <array>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

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
// Stack-only SHA1 for full physical chip integrity, including padded short dumps.
bool sha1_matches(std::span<const uint8_t> bytes, const char *expected) {
    const auto rotate = [](uint32_t value, unsigned bits) {
        return (value << bits) | (value >> (32 - bits));
    };
    std::array<uint32_t, 5> hash = {0x67452301u, 0xefcdab89u, 0x98badcfeu,
                                  0x10325476u, 0xc3d2e1f0u};
    const size_t padded_size = ((bytes.size() + 9 + 63) / 64) * 64;
    const uint64_t bit_size = uint64_t(bytes.size()) * 8;
    for (size_t block = 0; block < padded_size; block += 64) {
        std::array<uint32_t, 80> words{};
        for (size_t i = 0; i < 64; ++i) {
            const size_t position = block + i;
            uint8_t value = 0;
            if (position < bytes.size()) value = bytes[position];
            else if (position == bytes.size()) value = 0x80;
            else if (position >= padded_size - 8)
                value = uint8_t(bit_size >> ((padded_size - 1 - position) * 8));
            words[i / 4] |= uint32_t(value) << (24 - (i % 4) * 8);
        }
        for (size_t i = 16; i < words.size(); ++i)
            words[i] = rotate(words[i - 3] ^ words[i - 8] ^ words[i - 14] ^ words[i - 16], 1);
        auto [a, b, c, d, e] = hash;
        for (unsigned i = 0; i < words.size(); ++i) {
            const uint32_t function = i < 20 ? ((b & c) | (~b & d)) :
                                      i < 40 ? (b ^ c ^ d) :
                                      i < 60 ? ((b & c) | (b & d) | (c & d)) : (b ^ c ^ d);
            const uint32_t constant = i < 20 ? 0x5a827999u : i < 40 ? 0x6ed9eba1u :
                                      i < 60 ? 0x8f1bbcdcu : 0xca62c1d6u;
            const uint32_t next = rotate(a, 5) + function + e + constant + words[i];
            e = d; d = c; c = rotate(b, 30); b = a; a = next;
        }
        hash[0] += a; hash[1] += b; hash[2] += c; hash[3] += d; hash[4] += e;
    }
    for (size_t i = 0; i < 40; ++i) {
        const unsigned nibble = (hash[i / 8] >> (28 - (i % 8) * 4)) & 15;
        const char actual = "0123456789abcdef"[nibble];
        const char wanted = expected[i] >= 'A' && expected[i] <= 'F' ?
                            expected[i] + ('a' - 'A') : expected[i];
        if (actual != wanted) return false;
    }
    return expected[40] == '\0';
}
std::vector<uint8_t> chip(const std::filesystem::path &dir, const rom_manifest::Chip &entry) {
    const auto path = dir / entry.file;
    std::ifstream input(path, std::ios::binary | std::ios::ate);
    if (!input) throw std::runtime_error("Cannot open ROM: " + path.string());
    const auto length = input.tellg();
    if (length < 0 || (size_t(length) != entry.size &&
        !(entry.short_size && size_t(length) == entry.short_size)))
        throw std::runtime_error("Wrong ROM length: " + path.string());
    std::vector<uint8_t> bytes(entry.size, 0xff);
    input.seekg(0);
    if (!input.read(reinterpret_cast<char *>(bytes.data()), length))
        throw std::runtime_error("Cannot read ROM: " + path.string());
    const bool short_dump = size_t(length) != entry.size;
    if (crc32(bytes.data(), size_t(length)) != (short_dump ? entry.short_crc : entry.crc))
        throw std::runtime_error("ROM CRC mismatch: " + path.string());
    if (!sha1_matches(std::span<const uint8_t>(bytes.data(), size_t(length)),
                      short_dump ? entry.short_sha1 : entry.sha1))
        throw std::runtime_error("ROM SHA1 mismatch: " + path.string());
    if (short_dump) {
        if (crc32(bytes.data(), bytes.size()) != entry.crc || !sha1_matches(bytes, entry.sha1))
            throw std::runtime_error("Padded ROM integrity mismatch: " + path.string());
    }
    return bytes;
}
}
RomSet RomSet::load(const std::filesystem::path &dir, const std::string &set) {
    const rom_manifest::Game *game = nullptr;
    for (const auto &candidate : rom_manifest::games)
        if (candidate.id == set) { game = &candidate; break; }
    if (!game) throw std::runtime_error("Unsupported ROM set: " + set);
    RomSet roms;
    roms.name = set;
    roms.video = game->video;
    const std::array regions = {&roms.main, &roms.sprites, &roms.sprites_hi,
                               &roms.tiles, &roms.tiles_hi, &roms.sound, &roms.samples,
                               &roms.factory_eeprom};
    for (size_t region_index = 0; region_index < regions.size(); ++region_index) {
        auto &region = *regions[region_index];
        const auto &spec = game->regions[region_index];
        region.resize(spec.mapped_size, spec.fill);
        struct CachedChip {
            std::string_view file;
            std::vector<uint8_t> bytes;
        };
        // Retain only chips used by later CONTINUE/RELOAD lanes, not every region chip.
        std::vector<CachedChip> continued;
        for (size_t lane = 0; lane < spec.chips.size(); ++lane) {
            const auto &entry = spec.chips[lane];
            const std::string_view file = entry.file;
            const auto cached = std::find_if(continued.begin(), continued.end(),
                [file](const auto &item) { return item.file == file; });
            std::vector<uint8_t> physical;
            const std::vector<uint8_t> *bytes;
            if (cached != continued.end()) bytes = &cached->bytes;
            else {
                physical = chip(dir, entry);
                bytes = &physical;
            }
            size_t destination = entry.offset;
            for (size_t i = 0; i < entry.length; i += entry.group, destination += entry.stride)
                std::copy_n(bytes->data() + entry.source_offset + i, entry.group,
                            region.data() + destination);
            const bool used_again = std::any_of(spec.chips.begin() + lane + 1, spec.chips.end(),
                [file](const auto &next) { return file == next.file; });
            if (cached == continued.end()) {
                if (used_again) continued.push_back({file, std::move(physical)});
            } else if (!used_again) continued.erase(cached);
        }
        for (size_t offset = spec.size; offset < spec.mapped_size; offset += spec.size)
            std::copy_n(region.data(), spec.size, region.data() + offset);
    }
    return roms;
}
}
