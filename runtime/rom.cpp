#include "f3rt/rom.hpp"
#include "rom_manifest.hpp"
extern "C" {
#include "third_party/sha1dc/sha1.h"
}
#include <zlib.h>
#include <algorithm>
#include <array>
#include <fstream>
#include <span>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace f3rt {
uint32_t crc32(const uint8_t *data, size_t size) {
    uLong crc = ::crc32(0L, Z_NULL, 0);
    while (size) {
        const uInt chunk = uInt(std::min<size_t>(size, 1u << 30));
        crc = ::crc32(crc, data, chunk);
        data += chunk;
        size -= chunk;
    }
    return uint32_t(crc);
}
std::array<uint32_t, 7> rom_region_crcs(const RomSet &roms) {
    const std::vector<uint8_t> *regions[] = {
        &roms.main, &roms.sprites, &roms.sprites_hi,
        &roms.tiles, &roms.tiles_hi, &roms.sound, &roms.samples
    };
    std::array<uint32_t, 7> crcs{};
    for (size_t i = 0; i < 7; ++i) {
        crcs[i] = crc32(regions[i]->data(), regions[i]->size());
    }
    return crcs;
}
namespace {
// SHA1 with collision detection (sha1collisiondetection); rejects detected attack inputs.
bool sha1_matches(std::span<const uint8_t> bytes, const char *expected) {
    SHA1_CTX context;
    SHA1DCInit(&context);
    SHA1DCUpdate(&context, reinterpret_cast<const char *>(bytes.data()), bytes.size());
    unsigned char digest[20];
    if (SHA1DCFinal(digest, &context)) return false;
    for (size_t i = 0; i < 40; ++i) {
        const unsigned nibble = (digest[i / 2] >> ((i % 2) ? 0 : 4)) & 15;
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
