#include "commands.hpp"
#include "../runner/runner.hpp"
#include "../runner/sound/capture.hpp"
#include "../webplayer/bundle.hpp"
#include "../webplayer/discovery.hpp"
#include "../webplayer/trace_recorder.hpp"
#include "audio/hle/replay.hpp"
#include "f3rt/memory_map.hpp"
#include "f3rt/rom.hpp"
#include "rom_manifest.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <iomanip>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <thread>

namespace f3rt::tool {

namespace {

namespace wp = f3rt::webplayer;
namespace fs = std::filesystem;

#ifndef F3RT_SOURCE_DIR
#define F3RT_SOURCE_DIR "."
#endif
#ifndef F3RT_WEBPLAYER_SITE_DIR
#define F3RT_WEBPLAYER_SITE_DIR "tools/webplayer/site"
#endif

constexpr uint32_t FIRST_BANK = 1, LAST_BANK = 31; // sound-test BANK range
constexpr size_t SOUND_REGION = 5, SAMPLES_REGION = 6; // rom_manifest::Game::regions order
constexpr unsigned SONG_ID_CODE_DIGITS = 3;

void print_help(const char *prog) {
    std::cout
        << "Usage: " << prog << " webplayer-export [options]\n\n"
        << "Generates the web music player bundle of one game from its ROMs: the sound ROM and sample dumps, the\n"
        << "boot packet stream, one replay script and one prebaked trace per song of the sound test.\n\n"
        << "Options:\n"
        << "  --rom-dir DIR          Path to ROM directory\n"
        << "  --set SET              ROM set name (default: " F3RT_DEFAULT_SET ")\n"
        << "  --out DIR              Bundle directory (default: build/webplayer). Must be git-ignored (it holds\n"
        << "                         ROM-derived data) unless --allow-tracked-out is given. Writes <set>/ and merges\n"
        << "                         the set into games.json\n"
        << "  --allow-tracked-out    Skip the git-ignore check of --out\n"
        << "  --banks LIST|all       Sound-test BANK values to export (1..31, e.g. 1,4-7,16-31; default all). Banks\n"
        << "                         that select the same sound ROM page are exported once. Needs a set with sound\n"
        << "                         bank switching\n"
        << "  --only CODES           Export only the songs started by these sound-test codes (comma list)\n"
        << "  --jobs N               Songs traced in parallel (default: hardware threads); output does not depend\n"
        << "                         on it\n"
        << "  --loops N              Loop iterations traced after the intro (default 1)\n"
        << "  --max-seconds S        Hard cap of a traced song (default 360)\n"
        << "  --tail S               Silence that ends a non-looping song (default 1.0)\n"
        << "  --boot-frames N        Interpreted main CPU boot frames before freezing (default 900)\n"
        << "  --wasm FILE            WebAssembly module to copy (default: build-wasm/f3hle.wasm)\n"
        << "  --site DIR             Page sources to copy (default: tools/webplayer/site)\n"
        << "  --no-wasm, --no-site   Do not copy the module / the page sources\n"
        << "  --help, -h             Show this help message\n";
}

std::vector<uint32_t> parse_bank_list(const std::string &text) {
    std::vector<uint32_t> banks;
    std::stringstream in(text);
    std::string item;
    while (std::getline(in, item, ',')) {
        const auto [first, last] = runner::parse_range(item);
        if (first < FIRST_BANK || last > LAST_BANK)
            throw std::runtime_error("--banks values must be between 1 and 31");
        for (uint32_t b = first; b <= last; ++b) banks.push_back(b);
    }
    return banks;
}

std::vector<uint32_t> parse_code_list(const std::string &text) {
    std::vector<uint32_t> codes;
    std::stringstream in(text);
    std::string item;
    while (std::getline(in, item, ',')) {
        const auto [first, last] = runner::parse_range(item);
        for (uint32_t c = first; c <= last; ++c) codes.push_back(c);
    }
    return codes;
}

std::string song_id(const std::optional<uint32_t> &bank, uint32_t code) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "b%02u-%0*x", bank.value_or(0), int(SONG_ID_CODE_DIGITS), code);
    return buf;
}

std::string song_label(const std::optional<uint32_t> &bank, uint32_t sequence, uint32_t code) {
    char buf[64];
    if (bank) std::snprintf(buf, sizeof buf, "bank %02u / song 0x%02X / code 0x%03X", *bank, sequence, code);
    else std::snprintf(buf, sizeof buf, "song 0x%02X / code 0x%03X", sequence, code);
    return buf;
}

// The packets the engine queues for a request: every packet of at most REPLAY_MAX_PACKET bytes (longer ones are
// consumed but not queued by the mailbox parser).
void append_queued_packets(std::vector<std::vector<uint8_t>> &out, const std::vector<std::vector<uint8_t>> &packets) {
    for (const auto &p : packets)
        if (p.size() <= hle::REPLAY_MAX_PACKET) out.push_back(p);
}

// Reads the packet items back out of an F3RP script (the manifest form and the script must agree, README 3.2).
std::vector<std::vector<uint8_t>> script_packets(const std::vector<uint8_t> &script) {
    constexpr size_t HEADER = 8;
    constexpr uint8_t OP_RESET = 1, OP_GAIN = 2, OP_BANK = 3, OP_PACKET = 4;
    if (script.size() < HEADER || std::string(script.begin(), script.begin() + 4) != "F3RP")
        throw std::runtime_error("replay script header mismatch");
    const size_t items = size_t(script[6]) | size_t(script[7]) << 8;
    std::vector<std::vector<uint8_t>> packets;
    size_t at = HEADER;
    for (size_t i = 0; i < items; ++i) {
        const uint8_t op = script.at(at++);
        if (op == OP_RESET) continue;
        if (op == OP_GAIN) at += 2;
        else if (op == OP_BANK) at += 1;
        else if (op == OP_PACKET) {
            const size_t n = script.at(at++);
            packets.emplace_back(script.begin() + ptrdiff_t(at), script.begin() + ptrdiff_t(at + n));
            at += n;
        } else {
            throw std::runtime_error("replay script has an unknown item");
        }
    }
    return packets;
}

struct PageEntry {
    std::optional<uint32_t> bank, page;
    std::vector<uint32_t> aliases;
};

struct Job {
    size_t page_index = 0, song_index = 0;
    std::string id;
};

struct JobResult {
    bool ok = false;
    bool empty = false; // traced, but no notes and no voices
    std::string error;
    uint64_t frames = 0, audio_frames = 0, bytes = 0, trace_bytes = 0;
    bool loop = false;
    std::string end_reason;
    std::vector<uint8_t> replay_bytes; // kept for the manifest consistency check only
};

fs::path default_out() { return fs::path(F3RT_SOURCE_DIR) / "build" / "webplayer"; }

} // namespace

int cmd_webplayer_export(int argc, char **argv) try {
    runner::RunnerConfig cfg;
#ifdef F3RT_DEFAULT_ROM_DIR
    cfg.rom_dir = F3RT_DEFAULT_ROM_DIR;
#endif
    cfg.audio_backend = Audio::Backend::Enhanced;
    cfg.translated = false;
    cfg.allow_fallback = true;

    fs::path out = default_out();
    fs::path wasm = fs::path(F3RT_SOURCE_DIR) / "build-wasm" / "f3hle.wasm";
    fs::path site = F3RT_WEBPLAYER_SITE_DIR;
    bool allow_tracked = false, copy_wasm = true, copy_site = true;
    bool all_banks = true;
    std::vector<uint32_t> banks;
    std::vector<uint32_t> only_codes;
    unsigned jobs = std::max(1u, std::thread::hardware_concurrency());
    uint64_t boot_frames = 900;
    wp::TraceOptions trace_options;

    for (int i = 0; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> const char * {
            if (i + 1 >= argc) throw std::runtime_error("Missing value for argument: " + arg);
            return argv[++i];
        };
        auto number = [&]() { return runner::parse_number(value()); };
        auto real = [&]() {
            const std::string text = value();
            size_t end = 0;
            const double v = std::stod(text, &end);
            if (end != text.size() || !(v > 0.0) || std::isinf(v)) throw std::runtime_error(arg + " must be a positive number");
            return v;
        };
        if (arg == "--rom-dir") cfg.rom_dir = value();
        else if (arg == "--set") cfg.set = value();
        else if (arg == "--out") out = value();
        else if (arg == "--allow-tracked-out") allow_tracked = true;
        else if (arg == "--wasm") wasm = value();
        else if (arg == "--site") site = value();
        else if (arg == "--no-wasm") copy_wasm = false;
        else if (arg == "--no-site") copy_site = false;
        else if (arg == "--banks") {
            const std::string text = value();
            if (text == "all") {
                all_banks = true;
                banks.clear();
            } else {
                all_banks = false;
                banks = parse_bank_list(text);
            }
        } else if (arg == "--only") only_codes = parse_code_list(value());
        else if (arg == "--jobs") {
            jobs = number();
            if (jobs == 0) throw std::runtime_error("--jobs must be positive");
        } else if (arg == "--loops") {
            trace_options.loops = number();
            if (trace_options.loops == 0) throw std::runtime_error("--loops must be positive");
        } else if (arg == "--max-seconds") trace_options.max_seconds = real();
        else if (arg == "--tail") trace_options.tail_seconds = real();
        else if (arg == "--boot-frames") {
            boot_frames = number();
            if (boot_frames == 0) throw std::runtime_error("--boot-frames must be positive");
        } else if (arg == "--help" || arg == "-h") {
            print_help("f3rt-tool");
            return 0;
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }
    if (cfg.rom_dir.empty()) throw std::runtime_error("ROM directory must be specified via --rom-dir");

    const auto start_time = std::chrono::steady_clock::now();
    const runner::SoundProfile *profile = runner::find_sound_profile(cfg.set);
    if (!profile) throw std::runtime_error("No sound-test profile registered for set '" + cfg.set + "'");

    // Cheap checks first: nothing below the output directory is touched before they pass.
    wp::require_ignored_output(out, allow_tracked);
    if (copy_wasm && !fs::is_regular_file(wasm))
        throw std::runtime_error("WASM module not found: " + wasm.string() +
                                 " (build it with `cmake --build build-wasm --target f3hle`, or pass --wasm FILE / --no-wasm)");
    if (copy_site && !fs::is_directory(site))
        throw std::runtime_error("Site directory not found: " + site.string() + " (pass --site DIR or --no-site)");

    // ---- boot, boot stream, discovery
    std::cout << "Booting " << cfg.set << " (" << boot_frames << " interpreted frames)...\n" << std::flush;
    auto runner = runner::boot_to_freeze(cfg, boot_frames, /*record_boot_wav=*/false);
    Machine &m = runner->machine();
    const runner::PostBootSnapshot snapshot = runner::take_post_boot_snapshot(m);
    const auto boot_stream = runner::boot_packet_stream(m, snapshot.boot_commands);
    const auto boot_packets = runner::split_packets(boot_stream);
    const std::array<uint8_t, 2> gain_raw = {m.shared[MAILBOX_GAIN_CH0], m.shared[MAILBOX_GAIN_CH1]};

    const wp::CodeScan scan = wp::scan_music_codes(m, snapshot, *profile);
    std::cout << "Discovery: " << scan.codes_scanned << " codes scanned, " << scan.music_codes << " music codes, "
              << scan.songs.size() << " songs\n";
    if (scan.songs.empty()) throw std::runtime_error("No music codes found in the sound test");

    std::vector<size_t> song_indices;
    for (size_t i = 0; i < scan.songs.size(); ++i) {
        const wp::Song &song = scan.songs[i];
        if (only_codes.empty() ||
            std::any_of(only_codes.begin(), only_codes.end(), [&](uint32_t c) {
                return std::find(song.codes.begin(), song.codes.end(), c) != song.codes.end();
            }))
            song_indices.push_back(i);
    }
    if (song_indices.empty()) throw std::runtime_error("--only matches no song");

    // ---- ROM spans (exactly what Audio::load_sound_rom / load_sample_rom hand the engine)
    const std::vector<uint8_t> &sound_rom = m.roms.sound;
    const std::vector<uint16_t> sample_words = wp::sample_words(m.roms.samples);

    // ---- pages
    std::vector<PageEntry> pages;
    wp::Manifest manifest;
    if (m.roms.sound_bankswitch) {
        if (all_banks) {
            banks.clear();
            for (uint32_t b = FIRST_BANK; b <= LAST_BANK; ++b) banks.push_back(b);
        }
        const wp::BankResolution resolution = wp::resolve_bank_pages(sound_rom, sample_words, banks);
        for (const wp::BankPage &p : resolution.pages) {
            pages.push_back({p.bank, p.page, p.aliases});
            manifest.banks.push_back({p.bank, p.page, p.aliases});
        }
        for (const wp::RejectedBank &r : resolution.rejected) {
            wp::ManifestSkipped skipped;
            skipped.scope = wp::ManifestSkipped::Scope::Bank;
            skipped.bank = r.bank;
            skipped.page = r.page;
            skipped.songs = uint32_t(song_indices.size());
            skipped.error = r.error;
            manifest.skipped.push_back(std::move(skipped));
        }
    } else {
        if (!all_banks) throw std::runtime_error("--banks needs a set with sound bank switching");
        pages.push_back({std::nullopt, std::nullopt, {}});
    }

    const fs::path set_dir = out / cfg.set;
    fs::create_directories(set_dir);
    fs::remove_all(set_dir / "songs"); // the manifest is rewritten below: no stale songs
    fs::create_directories(set_dir / "songs");
    if (copy_site) wp::copy_site(site, out);
    if (copy_wasm) wp::copy_wasm(wasm, out);
    wp::write_file(set_dir / "sound.bin", sound_rom);
    wp::write_file(set_dir / "samples.bin", wp::samples_image(sample_words));

    const rom_manifest::Game *game = nullptr;
    for (const auto &g : rom_manifest::games)
        if (g.id == cfg.set) game = &g;
    if (!game) throw std::runtime_error("Set not in the ROM manifest: " + cfg.set);
    const auto crcs = rom_region_crcs(m.roms);
    auto fill_chips = [&](wp::RomFile &rom, size_t region) {
        for (const auto &chip : game->regions[region].chips) rom.chips.push_back({chip.file, chip.crc});
    };
    manifest.set = cfg.set;
    manifest.title = std::string(profile->title);
    manifest.git = wp::git_short_hash(F3RT_SOURCE_DIR);
    manifest.sound = {"sound.bin", sound_rom.size(), crcs[SOUND_REGION], 0, {}};
    fill_chips(manifest.sound, SOUND_REGION);
    manifest.sound_banked = m.roms.sound_bankswitch;
    manifest.samples = {"samples.bin", sample_words.size() * 2, crcs[SAMPLES_REGION], sample_words.size(), {}};
    fill_chips(manifest.samples, SAMPLES_REGION);
    manifest.gain_raw = gain_raw;
    for (const auto &p : boot_packets) manifest.boot_packets.push_back(wp::hex_string(p));
    manifest.discovery = {scan.codes_scanned, scan.music_codes, uint32_t(scan.songs.size())};

    // ---- jobs: one replay script + one trace per (page, song)
    std::vector<Job> job_list;
    for (size_t p = 0; p < pages.size(); ++p)
        for (const size_t s : song_indices) job_list.push_back({p, s, song_id(pages[p].bank, scan.songs[s].code)});
    std::vector<wp::ManifestSong> entries(job_list.size());
    std::vector<JobResult> results(job_list.size());
    std::atomic<size_t> next{0}, done{0};
    std::mutex log_mutex;
    const size_t total = job_list.size();
    std::cout << "Tracing " << total << " songs on " << std::min<size_t>(jobs, total) << " threads...\n" << std::flush;

    auto work = [&] {
        for (;;) {
            const size_t n = next.fetch_add(1);
            if (n >= job_list.size()) return;
            const Job &job = job_list[n];
            const PageEntry &page = pages[job.page_index];
            const wp::Song &song = scan.songs[job.song_index];
            JobResult &result = results[n];
            wp::ManifestSong &entry = entries[n];
            try {
                hle::ReplayScript script;
                script.gain_raw = gain_raw;
                if (page.page) script.page = uint8_t(*page.page);
                append_queued_packets(script.packets, boot_packets);
                for (const auto &call : song.capture.calls) {
                    entry.calls.push_back({call.code, wp::hex_string(call.bytes)});
                    append_queued_packets(script.packets, runner::split_packets(call.bytes));
                }
                const std::vector<uint8_t> replay_bytes = hle::encode_replay(script);
                std::vector<std::vector<uint8_t>> expected;
                append_queued_packets(expected, boot_packets);
                for (const auto &call : song.capture.calls) append_queued_packets(expected, runner::split_packets(call.bytes));
                if (script_packets(replay_bytes) != expected)
                    throw std::runtime_error("replay script and manifest packets disagree");

                wp::SongMeta meta;
                meta.set = cfg.set;
                meta.id = job.id;
                meta.code = song.code;
                meta.bank = page.bank;
                meta.page = page.page;
                meta.label = song_label(page.bank, song.sequence, song.code);
                const wp::TraceResult trace = wp::trace_song(sound_rom, sample_words, replay_bytes, meta, trace_options);
                if (trace.counts.notes == 0 && trace.counts.voices == 0) {
                    result.empty = true;
                    throw std::runtime_error("the song plays no notes (silent)");
                }
                entry.id = job.id;
                entry.code = song.code;
                entry.codes = song.codes;
                entry.sequence = song.sequence;
                entry.bank = page.bank;
                entry.page = page.page;
                entry.label = meta.label;
                entry.replay = "songs/" + job.id + ".replay";
                entry.trace = "songs/" + job.id + ".json";
                entry.trace_bin = "songs/" + job.id + ".bin";
                entry.frames = trace.frames;
                entry.audio_frames = trace.audio_frames;
                entry.loop = trace.loop;
                entry.end_reason = wp::end_reason_name(trace.end_reason);
                entry.bytes = replay_bytes.size() + trace.json.size() + trace.bin.size();
                wp::write_file(set_dir / entry.replay, std::span<const uint8_t>(replay_bytes));
                wp::write_file(set_dir / entry.trace, trace.json);
                wp::write_file(set_dir / entry.trace_bin, std::span<const uint8_t>(trace.bin));
                result.ok = true;
                result.frames = trace.frames;
                result.bytes = entry.bytes;
                result.trace_bytes = trace.json.size() + trace.bin.size();
            } catch (const std::exception &e) {
                result.error = e.what();
            }
            const size_t finished = done.fetch_add(1) + 1;
            if (finished % 100 == 0 || finished == total) {
                std::lock_guard lock(log_mutex);
                std::cout << "  " << finished << "/" << total << "\n" << std::flush;
            }
        }
    };
    {
        std::vector<std::thread> threads;
        const size_t count = std::min<size_t>(jobs, total);
        for (size_t t = 0; t < count; ++t) threads.emplace_back(work);
        for (auto &t : threads) t.join();
    }

    // ---- manifest: songs in (bank, code) order; failures are listed, never dropped
    for (size_t n = 0; n < job_list.size(); ++n)
        if (results[n].ok) manifest.songs.push_back(std::move(entries[n]));
    std::sort(manifest.songs.begin(), manifest.songs.end(), [](const wp::ManifestSong &a, const wp::ManifestSong &b) {
        if (a.bank.value_or(0) != b.bank.value_or(0)) return a.bank.value_or(0) < b.bank.value_or(0);
        return a.code < b.code;
    });
    for (size_t p = 0; p < pages.size(); ++p) {
        // all songs of a page failing the same way = the page itself is rejected
        std::map<std::string, uint32_t> failures;
        uint32_t attempted = 0;
        for (size_t n = 0; n < job_list.size(); ++n)
            if (job_list[n].page_index == p) {
                ++attempted;
                if (!results[n].ok && !results[n].empty) ++failures[results[n].error];
            }
        const bool bank_wide = failures.size() == 1 && failures.begin()->second == attempted;
        if (bank_wide) {
            wp::ManifestSkipped skipped;
            skipped.scope = wp::ManifestSkipped::Scope::Bank;
            skipped.bank = pages[p].bank;
            skipped.page = pages[p].page;
            skipped.songs = attempted;
            skipped.error = failures.begin()->first;
            manifest.skipped.push_back(std::move(skipped));
            continue;
        }
        for (size_t n = 0; n < job_list.size(); ++n) {
            if (job_list[n].page_index != p || results[n].ok) continue;
            const wp::Song &song = scan.songs[job_list[n].song_index];
            wp::ManifestSkipped skipped;
            skipped.scope = wp::ManifestSkipped::Scope::Song;
            skipped.bank = pages[p].bank;
            skipped.page = pages[p].page;
            skipped.id = job_list[n].id;
            skipped.code = song.code;
            skipped.sequence = song.sequence;
            skipped.error = results[n].error;
            manifest.skipped.push_back(std::move(skipped));
        }
    }
    for (const auto &[code, error] : scan.failures) {
        wp::ManifestSkipped skipped;
        skipped.scope = wp::ManifestSkipped::Scope::Code;
        skipped.code = code;
        skipped.error = error;
        manifest.skipped.push_back(std::move(skipped));
    }

    wp::write_file(set_dir / "manifest.json", wp::manifest_json(manifest));
    wp::merge_games_json(out, cfg.set, manifest.title, uint32_t(manifest.songs.size()));

    // ---- summary
    uint64_t song_bytes = 0;
    for (const auto &s : manifest.songs) song_bytes += s.bytes;
    uint32_t skipped_banks = 0, skipped_songs = 0, skipped_codes = 0;
    for (const auto &s : manifest.skipped) {
        skipped_banks += s.scope == wp::ManifestSkipped::Scope::Bank;
        skipped_songs += s.scope == wp::ManifestSkipped::Scope::Song;
        skipped_codes += s.scope == wp::ManifestSkipped::Scope::Code;
    }
    const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
    std::cout << "SUMMARY set=" << cfg.set << " songs=" << manifest.songs.size() << " skipped_banks=" << skipped_banks
              << " skipped_songs=" << skipped_songs << " skipped_codes=" << skipped_codes
              << " song_bytes=" << song_bytes << " rom_bytes=" << (manifest.sound.bytes + manifest.samples.bytes)
              << " wall_time=" << std::fixed << std::setprecision(2) << elapsed << "s\n";
    for (const auto &s : manifest.skipped) {
        std::cout << "  skipped " << (s.scope == wp::ManifestSkipped::Scope::Bank ? "bank" : s.scope == wp::ManifestSkipped::Scope::Song ? "song" : "code");
        if (s.bank) std::cout << " bank=" << *s.bank;
        if (s.page) std::cout << " page=" << *s.page;
        if (!s.id.empty()) std::cout << " id=" << s.id;
        if (s.scope == wp::ManifestSkipped::Scope::Bank) std::cout << " songs=" << s.songs;
        else std::cout << " code=" << s.code;
        std::cout << ": " << s.error << '\n';
    }
    std::vector<const wp::ManifestSong *> largest;
    for (const auto &s : manifest.songs) largest.push_back(&s);
    std::sort(largest.begin(), largest.end(), [](const auto *a, const auto *b) {
        return a->bytes != b->bytes ? a->bytes > b->bytes : a->id < b->id;
    });
    for (size_t i = 0; i < std::min<size_t>(5, largest.size()); ++i)
        std::cout << "  largest " << largest[i]->id << " " << largest[i]->bytes << " bytes, " << largest[i]->frames
                  << " frames (" << largest[i]->end_reason << ")\n";
    return manifest.songs.empty() ? 1 : 0;
} catch (const std::exception &e) {
    std::cerr << "WEBPLAYER_EXPORT ERROR: " << e.what() << '\n';
    return 1;
}

} // namespace f3rt::tool
