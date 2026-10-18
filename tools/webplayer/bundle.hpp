#pragma once

// Bundle layout and writers of the web-player exporter (tools/webplayer/README.md, section 5): output-directory
// hygiene, site/WASM copy, ROM dumps, manifest.json and the games.json merge. Every writer produces the same bytes
// for the same input (no timestamps, fixed member order).

#include "json.hpp"

#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace f3rt::webplayer {

namespace fs = std::filesystem;

// Throws unless `out` is git-ignored (`git check-ignore -q`) or lies outside any git work tree (nothing there can be
// committed). `allow_tracked` skips the check.
void require_ignored_output(const fs::path &out, bool allow_tracked);

// Creates parent directories, writes to a sibling temp file and renames it over `path`.
void write_file(const fs::path &path, std::span<const uint8_t> bytes);
void write_file(const fs::path &path, std::string_view text);
std::vector<uint8_t> read_file(const fs::path &path);

// Copies every file below `site_dir` into `out` (same relative paths). Throws when the directory is missing.
void copy_site(const fs::path &site_dir, const fs::path &out);
// Copies `wasm` to `<out>/f3hle.wasm`. Throws with a build hint when the file is missing.
void copy_wasm(const fs::path &wasm, const fs::path &out);

// The sample word array the HLE engine reads (Audio::load_sample_rom): word i = (rom[2i] << 8) | rom[2i + 1] of the
// big-endian ROM image. The size must be even.
std::vector<uint16_t> sample_words(std::span<const uint8_t> big_endian_rom);
// samples.bin: the little-endian image of that word array (the in-memory array on x86 and WebAssembly).
std::vector<uint8_t> samples_image(std::span<const uint16_t> words);

std::string hex_string(std::span<const uint8_t> bytes);
std::string crc_string(uint32_t crc); // 8 lowercase hex digits

// "<short hash>" of the repository HEAD at `source_dir`, "unknown" when git cannot tell.
std::string git_short_hash(const fs::path &source_dir);

struct ChipInfo {
    std::string file;
    uint32_t crc32 = 0;
};

struct RomFile {
    std::string file; // name inside the set directory
    uint64_t bytes = 0;
    uint32_t crc32 = 0; // sound: of the file; samples: of the big-endian region (not of the file)
    uint64_t words = 0; // samples only
    std::vector<ChipInfo> chips;
};

struct ManifestCall {
    uint32_t code = 0;
    std::string bytes; // hex
};

struct ManifestSong {
    std::string id;
    uint32_t code = 0;
    std::vector<uint32_t> codes; // every code that starts the same sequence, ascending, includes `code`
    uint32_t sequence = 0;
    std::optional<uint32_t> bank, page;
    std::string label;
    std::vector<ManifestCall> calls;
    std::string replay, trace, trace_bin; // paths relative to the set directory
    uint64_t frames = 0, audio_frames = 0;
    bool loop = false;
    std::string end_reason;
    uint64_t bytes = 0; // replay + trace + trace_bin
};

// Something the exporter did not turn into a song, with the reason. Never dropped silently.
struct ManifestSkipped {
    enum class Scope { Bank, Song, Code };
    Scope scope = Scope::Song;
    std::optional<uint32_t> bank, page;
    std::string id;
    uint32_t code = 0;
    uint32_t sequence = 0;
    uint32_t songs = 0; // Bank scope: how many songs failed the same way
    std::string error;
};

// A sound-test BANK value the export covers: `aliases` select the same sound ROM page and are not exported again.
struct ManifestBank {
    uint32_t bank = 0, page = 0;
    std::vector<uint32_t> aliases;
};

struct Manifest {
    std::string set, title;
    std::string git;
    uint32_t abi = 1;
    RomFile sound, samples;
    bool sound_banked = false;
    std::array<uint8_t, 2> gain_raw{};
    std::vector<std::string> boot_packets; // hex
    struct {
        uint32_t codes_scanned = 0, music_codes = 0, songs = 0;
    } discovery;
    std::vector<ManifestBank> banks; // empty for a set without sound bank switching
    std::vector<ManifestSong> songs;
    std::vector<ManifestSkipped> skipped;
};

std::string manifest_json(const Manifest &manifest);

// Replaces (or adds) the entry of `set` in `<out>/games.json`; entries are kept sorted by set id.
void merge_games_json(const fs::path &out, std::string_view set, std::string_view title, uint32_t songs);

} // namespace f3rt::webplayer
