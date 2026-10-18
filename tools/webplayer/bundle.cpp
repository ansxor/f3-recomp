#include "bundle.hpp"

#include "f3rt/rom.hpp"

#include <algorithm>
#include <cstdio>
#include <fstream>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

namespace f3rt::webplayer {

namespace {

// Runs argv (PATH lookup), returns the exit status; stdout is captured when `output` is set, stderr is discarded.
int run_process(const std::vector<std::string> &args, std::string *output) {
    int pipe_fds[2] = {-1, -1};
    if (output && pipe(pipe_fds) != 0) throw std::runtime_error("pipe failed");
    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    if (output) {
        posix_spawn_file_actions_adddup2(&actions, pipe_fds[1], STDOUT_FILENO);
        posix_spawn_file_actions_addclose(&actions, pipe_fds[0]);
    } else {
        posix_spawn_file_actions_addopen(&actions, STDOUT_FILENO, "/dev/null", 1, 0);
    }
    posix_spawn_file_actions_addopen(&actions, STDERR_FILENO, "/dev/null", 1, 0);
    std::vector<char *> argv;
    for (const auto &a : args) argv.push_back(const_cast<char *>(a.c_str()));
    argv.push_back(nullptr);
    pid_t pid = 0;
    const int rc = posix_spawnp(&pid, argv[0], &actions, nullptr, argv.data(), environ);
    posix_spawn_file_actions_destroy(&actions);
    if (output) close(pipe_fds[1]);
    if (rc != 0) {
        if (output) close(pipe_fds[0]);
        return -1; // the program could not be started
    }
    if (output) {
        char buf[512];
        ssize_t n;
        while ((n = read(pipe_fds[0], buf, sizeof buf)) > 0) output->append(buf, size_t(n));
        close(pipe_fds[0]);
    }
    int status = 0;
    if (waitpid(pid, &status, 0) < 0) return -1;
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

fs::path nearest_existing_directory(fs::path p) {
    while (!p.empty() && !fs::is_directory(p)) {
        const fs::path parent = p.parent_path();
        if (parent == p) break;
        p = parent;
    }
    return p;
}

void write_hex_array(json::Writer &w, const std::vector<std::string> &items) {
    w.begin_array(true);
    for (const auto &s : items) w.value(s);
    w.end_array();
}

void write_rom(json::Writer &w, const RomFile &rom, bool samples, bool banked) {
    w.begin_object();
    w.member("file", rom.file);
    w.member("bytes", rom.bytes);
    if (samples) {
        w.member("words", rom.words);
        w.member("encoding", "u16le");
        w.member("crc32_region", crc_string(rom.crc32));
    } else {
        w.member("banked", banked);
        w.member("crc32", crc_string(rom.crc32));
    }
    w.key("chips").begin_array(true);
    for (const auto &chip : rom.chips) {
        w.begin_object();
        w.member("file", chip.file);
        w.member("crc32", crc_string(chip.crc32));
        w.end_object();
    }
    w.end_array();
    w.end_object();
}

const char *scope_name(ManifestSkipped::Scope scope) {
    switch (scope) {
    case ManifestSkipped::Scope::Bank: return "bank";
    case ManifestSkipped::Scope::Song: return "song";
    case ManifestSkipped::Scope::Code: return "code";
    }
    return "";
}

} // namespace

void require_ignored_output(const fs::path &out, bool allow_tracked) {
    if (allow_tracked) return;
    const fs::path absolute = fs::weakly_canonical(fs::absolute(out));
    const fs::path directory = nearest_existing_directory(absolute);
    const std::string where = directory.string();
    if (run_process({"git", "-C", where, "rev-parse", "--show-toplevel"}, nullptr) != 0)
        return; // outside any work tree (or no git): nothing here can be committed
    const int ignored = run_process({"git", "-C", where, "check-ignore", "-q", "--", absolute.string()}, nullptr);
    if (ignored != 0)
        throw std::runtime_error("Output directory " + absolute.string() +
                                 " is not git-ignored; it would hold ROM-derived data. Use a path under build/ or pass "
                                 "--allow-tracked-out");
}

void write_file(const fs::path &path, std::span<const uint8_t> bytes) {
    if (path.has_parent_path()) fs::create_directories(path.parent_path());
    fs::path temp = path;
    temp += ".tmp";
    {
        std::ofstream file(temp, std::ios::binary | std::ios::trunc);
        if (!file) throw std::runtime_error("Cannot write " + temp.string());
        file.write(reinterpret_cast<const char *>(bytes.data()), std::streamsize(bytes.size()));
        file.close();
        if (!file) throw std::runtime_error("Write failed: " + temp.string());
    }
    fs::rename(temp, path);
}

void write_file(const fs::path &path, std::string_view text) {
    write_file(path, std::span<const uint8_t>(reinterpret_cast<const uint8_t *>(text.data()), text.size()));
}

std::vector<uint8_t> read_file(const fs::path &path) {
    std::ifstream file(path, std::ios::binary | std::ios::ate);
    if (!file) throw std::runtime_error("Cannot read " + path.string());
    std::vector<uint8_t> bytes(size_t(file.tellg()));
    file.seekg(0);
    file.read(reinterpret_cast<char *>(bytes.data()), std::streamsize(bytes.size()));
    return bytes;
}

void copy_site(const fs::path &site_dir, const fs::path &out) {
    if (!fs::is_directory(site_dir))
        throw std::runtime_error("Site directory not found: " + site_dir.string() + " (pass --no-site to skip it)");
    fs::create_directories(out);
    for (const auto &entry : fs::recursive_directory_iterator(site_dir)) {
        if (!entry.is_regular_file()) continue;
        const fs::path target = out / fs::relative(entry.path(), site_dir);
        write_file(target, read_file(entry.path()));
    }
}

void copy_wasm(const fs::path &wasm, const fs::path &out) {
    if (!fs::is_regular_file(wasm))
        throw std::runtime_error("WASM module not found: " + wasm.string() +
                                 " (build it with `cmake --build build-wasm --target f3hle`, or pass --wasm FILE / "
                                 "--no-wasm)");
    write_file(out / "f3hle.wasm", read_file(wasm));
}

std::vector<uint16_t> sample_words(std::span<const uint8_t> big_endian_rom) {
    if (big_endian_rom.size() % 2) throw std::invalid_argument("sample ROM size must be even");
    std::vector<uint16_t> words(big_endian_rom.size() / 2);
    for (size_t i = 0; i < words.size(); ++i)
        words[i] = uint16_t((uint16_t(big_endian_rom[i * 2]) << 8) | big_endian_rom[i * 2 + 1]);
    return words;
}

std::vector<uint8_t> samples_image(std::span<const uint16_t> words) {
    std::vector<uint8_t> image(words.size() * 2);
    for (size_t i = 0; i < words.size(); ++i) {
        image[i * 2] = uint8_t(words[i]); // low byte first
        image[i * 2 + 1] = uint8_t(words[i] >> 8);
    }
    return image;
}

std::string hex_string(std::span<const uint8_t> bytes) {
    static constexpr char HEX[] = "0123456789abcdef";
    std::string s;
    s.reserve(bytes.size() * 2);
    for (const uint8_t b : bytes) {
        s += HEX[b >> 4];
        s += HEX[b & 15];
    }
    return s;
}

std::string crc_string(uint32_t crc) {
    char buf[9];
    std::snprintf(buf, sizeof buf, "%08x", crc);
    return buf;
}

std::string git_short_hash(const fs::path &source_dir) {
    std::string out;
    if (run_process({"git", "-C", source_dir.string(), "rev-parse", "--short", "HEAD"}, &out) != 0) return "unknown";
    while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
    return out.empty() ? "unknown" : out;
}

std::string manifest_json(const Manifest &m) {
    json::Writer w(/*pretty=*/true);
    w.begin_object();
    w.member("format", "f3-webplayer-manifest");
    w.member("version", 1);
    w.member("set", m.set);
    w.member("title", m.title);
    w.key("generator").begin_object();
    w.member("tool", "f3rt-tool webplayer-export");
    w.member("git", m.git);
    w.member("abi", m.abi);
    w.end_object();
    w.key("time").begin_object();
    w.member("sample_rate", 29762);
    w.member("main_clock", 16000000);
    w.member("timer_ticks", 16000);
    w.end_object();
    w.key("rom").begin_object();
    w.key("sound");
    write_rom(w, m.sound, false, m.sound_banked);
    w.key("samples");
    write_rom(w, m.samples, true, false);
    w.end_object();
    w.key("boot").begin_object();
    w.key("gain_raw").begin_array();
    w.value(m.gain_raw[0]).value(m.gain_raw[1]);
    w.end_array();
    w.key("packets");
    write_hex_array(w, m.boot_packets);
    w.end_object();
    w.key("discovery").begin_object();
    w.member("codes_scanned", m.discovery.codes_scanned);
    w.member("music_codes", m.discovery.music_codes);
    w.member("songs", m.discovery.songs);
    w.end_object();

    w.key("banks").begin_array(true);
    for (const auto &b : m.banks) {
        w.begin_object();
        w.member("bank", b.bank);
        w.member("page", b.page);
        w.key("aliases").begin_array();
        for (const uint32_t a : b.aliases) w.value(a);
        w.end_array();
        w.end_object();
    }
    w.end_array();

    w.key("songs").begin_array(true);
    for (const auto &s : m.songs) {
        w.begin_object();
        w.member("id", s.id);
        w.member("code", s.code);
        w.key("codes").begin_array();
        for (const uint32_t c : s.codes) w.value(c);
        w.end_array();
        w.member("sequence", s.sequence);
        if (s.bank) w.member("bank", *s.bank);
        else w.member_null("bank");
        if (s.page) w.member("page", *s.page);
        else w.member_null("page");
        w.member("label", s.label);
        w.key("calls").begin_array(true);
        for (const auto &c : s.calls) {
            w.begin_object();
            w.member("code", c.code);
            w.member("bytes", c.bytes);
            w.end_object();
        }
        w.end_array();
        w.member("replay", s.replay);
        w.member("trace", s.trace);
        w.member("trace_bin", s.trace_bin);
        w.member("frames", s.frames);
        w.member("audio_frames", s.audio_frames);
        w.member("loop", s.loop);
        w.member("end_reason", s.end_reason);
        w.member("bytes", s.bytes);
        w.end_object();
    }
    w.end_array();

    w.key("skipped").begin_array(true);
    for (const auto &s : m.skipped) {
        w.begin_object();
        w.member("scope", scope_name(s.scope));
        if (s.bank) w.member("bank", *s.bank);
        else w.member_null("bank");
        if (s.page) w.member("page", *s.page);
        else w.member_null("page");
        if (!s.id.empty()) w.member("id", s.id);
        if (s.scope != ManifestSkipped::Scope::Bank) {
            w.member("code", s.code);
            w.member("sequence", s.sequence);
        }
        if (s.scope == ManifestSkipped::Scope::Bank) w.member("songs", s.songs);
        w.member("error", s.error);
        w.end_object();
    }
    w.end_array();
    w.end_object();
    std::string text = w.take();
    text += '\n';
    return text;
}

void merge_games_json(const fs::path &out, std::string_view set, std::string_view title, uint32_t songs) {
    struct Entry {
        std::string set, title, manifest;
        int64_t songs;
    };
    std::vector<Entry> entries;
    const fs::path path = out / "games.json";
    if (fs::is_regular_file(path)) {
        const auto bytes = read_file(path);
        const auto doc = json::Value::parse(std::string_view(reinterpret_cast<const char *>(bytes.data()), bytes.size()));
        for (const auto &g : doc.at("games").as_array()) {
            Entry e{g.at("set").as_string(), g.at("title").as_string(), g.at("manifest").as_string(),
                    g.at("songs").as_int()};
            if (e.set != set) entries.push_back(std::move(e));
        }
    }
    entries.push_back({std::string(set), std::string(title), std::string(set) + "/manifest.json", songs});
    std::sort(entries.begin(), entries.end(), [](const Entry &a, const Entry &b) { return a.set < b.set; });

    json::Writer w(/*pretty=*/true);
    w.begin_object();
    w.member("format", "f3-webplayer-games");
    w.member("version", 1);
    w.key("games").begin_array(true);
    for (const auto &e : entries) {
        w.begin_object();
        w.member("set", e.set);
        w.member("title", e.title);
        w.member("manifest", e.manifest);
        w.member("songs", e.songs);
        w.end_object();
    }
    w.end_array();
    w.end_object();
    std::string text = w.take();
    text += '\n';
    write_file(path, text);
}

} // namespace f3rt::webplayer
