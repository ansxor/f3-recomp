// f3rt-tool netplay-replay: offline replay and diff of netplay desync bundles.
//
// A bundle (see docs/site/guide/netplay.md) holds this peer's machine state at match frame 0 (trigger.f3s) and every input
// word pair it applied (inputs.txt). Replaying those through run_lockstep_frame - the same function the live session
// uses - reproduces the peer's match locally, so:
//   * one bundle: verify the replay reproduces the recorded hash bytes / full state at the mismatch frame
//     (a failure means the local emulation is not a pure function of trigger.f3s + inputs.txt);
//   * two bundles: replay both side by side, compare the hashed bytes after EVERY frame and name the first frame and the
//     Machine addresses that differ.
#include "commands.hpp"
#include "../runner/runner.hpp"
#include "f3rt/game_netplay.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/netplay.hpp"
#include "f3rt/rom.hpp"
#include "netplay/recorder.hpp"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace f3rt::tool {

using namespace f3rt;
using namespace f3rt::netplay;

namespace {

namespace fs = std::filesystem;

void print_help() {
    std::cout << "Usage: f3rt-tool netplay-replay --bundle DIR [--bundle DIR2] [options]\n\n"
              << "Replays netplay desync bundles offline.\n\n"
              << "  --bundle DIR     Bundle folder written on DESYNC (give two to diff them frame by frame)\n"
              << "  --until N        Replay N match frames (default: the bundle's DESYNC frame)\n"
              << "  --rom-dir DIR    ROM directory (defaults to F3RT_DEFAULT_ROM_DIR)\n"
              << "  --set SET        ROM set name (default: " F3RT_DEFAULT_SET ")\n"
              << "  --max-diff N     Differing bytes to list (default 64)\n"
              << "  --gpu 0|1        Override the recorded machine: GPU game video (bundles from before info.json recorded it)\n"
              << "  --audio NAME     Override the recorded audio backend: enhanced | reference\n"
              << "  --dump-state FRAME OUT  Replay one bundle to match frame FRAME and write the machine state to OUT\n"
              << "  --help, -h       Show this help message\n";
}

std::vector<uint8_t> read_all(const fs::path &path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + path.string());
    return std::vector<uint8_t>(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
}

struct Bundle {
    fs::path dir;
    InputsFile inputs;
    std::vector<uint8_t> trigger, hashed;
    uint64_t seed = 0, frame = 0, interval = 60, hashed_len = 0, recorded_crc = 0, player = 0;
    int gpu = -1;             // machine_gpu from info.json, -1 = not recorded
    std::string audio;        // audio_backend from info.json, empty = not recorded
    bool legacy_hash = false; // no hash_layout in info.json
    std::unique_ptr<Machine> machine;
    uint64_t replayed = 0; // match frames run so far
};

Bundle load_bundle(const fs::path &dir) {
    Bundle b;
    b.dir = dir;
    const auto info_bytes = read_all(dir / "info.json");
    BundleInfo info;
    try {
        info = parse_info_json(std::string(info_bytes.begin(), info_bytes.end()));
    } catch (const std::exception &e) {
        throw std::runtime_error(dir.string() + ": " + e.what());
    }
    const auto inputs_bytes = read_all(dir / "inputs.txt");
    std::string error;
    if (!parse_inputs_txt(std::string(inputs_bytes.begin(), inputs_bytes.end()), b.inputs, error)) {
        throw std::runtime_error(dir.string() + ": " + error);
    }
    b.trigger = read_all(dir / "trigger.f3s");
    b.hashed = read_all(dir / "hashed.bin");
    if (!info.seed) throw std::runtime_error(dir.string() + ": info.json has no seed");
    b.seed = *info.seed;
    b.frame = info.frame.value_or(0);
    b.interval = info.crc_interval.value_or(60);
    b.hashed_len = info.hashed_len.value_or(0);
    b.recorded_crc = info.recorded_crc.value_or(0);
    b.player = info.player_index.value_or(0);
    if (info.machine_gpu) b.gpu = int(*info.machine_gpu);
    if (info.audio_backend) b.audio = *info.audio_backend;
    b.legacy_hash = !info.hash_layout.has_value();
    if (b.interval == 0) b.interval = 60;
    return b;
}

// Builds the machine the bundle was recorded on: GPU game video on/off and the audio backend change the state layout.
// Source order: command-line override, info.json, then (old bundles without config) whichever of the four combinations
// has the same state size as trigger.f3s.
std::unique_ptr<Machine> make_machine(const fs::path &rom_dir, const std::string &set, bool gpu, Audio::Backend backend) {
    auto m = runner::create_strict_machine(rom_dir, set, gpu, 1);
    if (gpu && !m->game_video) {
        throw std::runtime_error("this f3rt-tool build has no GPU game video (F3RT_GAME_VIDEO); cannot replay a GPU-machine bundle");
    }
    try {
        m->audio->set_backend(backend);
    } catch (const std::exception &e) {
        throw std::runtime_error(std::string("audio backend '") + Audio::backend_name(backend) +
                                 "' is unavailable in this build: " + e.what());
    }
    return m;
}

Audio::Backend parse_audio(const std::string &name) {
    Audio::Backend b;
    if (!Audio::parse_backend(name, b)) throw std::runtime_error("audio backend must be enhanced or reference, got '" + name + "'");
    return b;
}

std::unique_ptr<Machine> build_machine(const Bundle &b, const fs::path &rom_dir, const std::string &set, int gpu_override,
                                       const std::string &audio_override) {
    const int gpu = gpu_override >= 0 ? gpu_override : b.gpu;
    const std::string audio = !audio_override.empty() ? audio_override : b.audio;
    std::unique_ptr<Machine> m;
    if (gpu >= 0 && !audio.empty()) {
        m = make_machine(rom_dir, set, gpu == 1, parse_audio(audio));
        std::cout << "MACHINE gpu=" << gpu << " audio=" << audio << " source="
                  << ((gpu_override >= 0 || !audio_override.empty()) ? "command line" : "info.json") << "\n";
    } else {
        // No (complete) recorded config: pick the combination whose state size equals trigger.f3s.
        for (int g = 1; g >= 0 && !m; --g) {
            for (auto be : {Audio::Backend::Enhanced, Audio::Backend::Reference}) {
                if (gpu >= 0 && g != gpu) continue;
                if (!audio.empty() && be != parse_audio(audio)) continue;
                std::unique_ptr<Machine> cand;
                try {
                    cand = make_machine(rom_dir, set, g == 1, be);
                } catch (const std::exception &) {
                    continue; // combination unavailable in this build
                }
                if (cand->state_size() == b.trigger.size()) {
                    m = std::move(cand);
                    std::cout << "MACHINE gpu=" << g << " audio=" << Audio::backend_name(be) << " source=state-size-match\n";
                    break;
                }
            }
        }
        if (!m) {
            throw std::runtime_error(b.dir.string() + ": trigger.f3s is " + std::to_string(b.trigger.size()) +
                                     " bytes and no machine configuration available in this build has that state size; "
                                     "give --gpu 0|1 and --audio enhanced|reference");
        }
    }
    if (b.trigger.size() != m->state_size()) {
        throw std::runtime_error(b.dir.string() + ": trigger.f3s is " + std::to_string(b.trigger.size()) +
                                 " bytes, the requested machine's state is " + std::to_string(m->state_size()) +
                                 " (wrong --gpu/--audio, different build or ROM set?)");
    }
    return m;
}
size_t region_of(Machine &m, const GameNetplay *hook, std::array<uint8_t, 2048> &buf, uint8_t &phase) {
    return hook->sync_hash_region(m, buf, phase);
}

void run_one(Bundle &b, const GameNetplay *hook) {
    const uint32_t f = uint32_t(b.replayed);
    const auto &w = b.inputs.frames.at(f);
    if (!run_lockstep_frame(*b.machine, {w[0], w[1], 0, 0}, b.seed, f, hook)) {
        throw std::runtime_error("emulation failure at replay frame " + std::to_string(f));
    }
    ++b.replayed;
}

std::string hex(uint32_t v, int digits) {
    char buf[16];
    std::snprintf(buf, sizeof buf, "%0*x", digits, v);
    return buf;
}

// Replays a single bundle and checks it against what the live peer recorded.
bool verify_one(Bundle &b, const GameNetplay *hook, uint64_t until) {
    std::array<uint8_t, 2048> buf{};
    bool ok = true;
    while (b.replayed < until) {
        run_one(b, hook);
        const uint64_t completed = b.replayed;
        if (completed % b.interval == 0 || completed == until) {
            uint8_t phase = 0;
            const size_t n = region_of(*b.machine, hook, buf, phase);
            std::cout << "REPLAY bundle=" << b.dir.string() << " frame=" << completed << " crc=0x" << hex(f3rt::crc32(buf.data(), n), 8)
                      << " phase=" << unsigned(phase) << " len=" << n << "\n";
            if (completed == b.frame) {
                bool bytes_match, crc_match;
                if (b.legacy_hash) {
                    // Written by a build before hash_layout 2: gated phase-2 block (491 B), which is the leading part of
                    // the current hash region. The recorded CRC covers only that part, so compare bytes and its CRC.
                    bytes_match = b.hashed_len <= n && b.hashed.size() >= b.hashed_len &&
                                  std::memcmp(buf.data(), b.hashed.data(), b.hashed_len) == 0;
                    crc_match = f3rt::crc32(buf.data(), b.hashed_len) == uint32_t(b.recorded_crc);
                    std::cout << "LEGACY_HASH_LAYOUT bundle predates hash_layout 2: compared the leading " << b.hashed_len
                              << " hashed bytes\n";
                } else {
                    bytes_match = n == b.hashed_len && b.hashed.size() >= n && std::memcmp(buf.data(), b.hashed.data(), n) == 0;
                    crc_match = f3rt::crc32(buf.data(), n) == uint32_t(b.recorded_crc);
                }
                std::cout << "HASHED_REPLAY_MATCH bundle=" << b.dir.string() << " frame=" << completed
                          << " bytes=" << (bytes_match ? "yes" : "NO") << " crc=" << (crc_match ? "yes" : "NO") << "\n";
                ok = ok && bytes_match && crc_match;
            }
        }
    }
    return ok;
}

} // namespace

int cmd_netplay_replay(int argc, char **argv) try {
    std::vector<fs::path> bundle_dirs;
    fs::path rom_dir;
    std::string set = F3RT_DEFAULT_SET;
    uint64_t until_opt = 0;
    size_t max_diff = 64;
    int64_t dump_frame = -1;
    fs::path dump_path;
    int gpu_override = -1;
    std::string audio_override;
#ifdef F3RT_DEFAULT_ROM_DIR
    rom_dir = F3RT_DEFAULT_ROM_DIR;
#endif
    for (int i = 0; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> const char * {
            if (i + 1 >= argc) throw std::runtime_error("Missing value for argument: " + arg);
            return argv[++i];
        };
        if (arg == "--bundle") bundle_dirs.emplace_back(value());
        else if (arg == "--until") until_opt = std::stoull(value());
        else if (arg == "--rom-dir") rom_dir = value();
        else if (arg == "--set") set = value();
        else if (arg == "--max-diff") max_diff = std::stoul(value());
        else if (arg == "--gpu") {
            const std::string v = value();
            if (v != "0" && v != "1") throw std::runtime_error("--gpu must be 0 or 1");
            gpu_override = v == "1" ? 1 : 0;
        } else if (arg == "--audio") {
            audio_override = value();
        }
        else if (arg == "--dump-state") {
            dump_frame = std::stoll(value());
            dump_path = value();
        }
        else if (arg == "--help" || arg == "-h") {
            print_help();
            return 0;
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }
    if (bundle_dirs.empty() || bundle_dirs.size() > 2) throw std::runtime_error("give one or two --bundle DIR");
    if (rom_dir.empty()) throw std::runtime_error("ROM directory must be specified via --rom-dir or F3RT_DEFAULT_ROM_DIR");

    const GameNetplay *hook = game_netplay();
    if (!hook || !hook->sync_hash_region) throw std::runtime_error("this game provides no sync_hash_region hook");

    std::vector<Bundle> bundles;
    for (const auto &dir : bundle_dirs) bundles.push_back(load_bundle(dir));
    for (auto &b : bundles) {
        b.machine = build_machine(b, rom_dir, set, gpu_override, audio_override);
        b.machine->load_state(b.trigger);
        std::cout << "BUNDLE " << b.dir.string() << " player=" << b.player << " frame=" << b.frame << " seed=" << b.seed
                  << " inputs=" << b.inputs.frames.size() << " " << "frames_between_sync_and_start="
                  << header_value(b.inputs, "frames_between_sync_and_start", "?")
                  << " lobby_hold_frames=" << header_value(b.inputs, "lobby_hold_frames", "?") << "\n";
    }

    auto until_for = [&](const Bundle &b) {
        const uint64_t want = until_opt ? until_opt : b.frame;
        return std::min<uint64_t>(want, b.inputs.frames.size());
    };

    if (dump_frame >= 0) {
        if (bundles.size() != 1) throw std::runtime_error("--dump-state works on exactly one --bundle");
        Bundle &d = bundles[0];
        if (uint64_t(dump_frame) > d.inputs.frames.size()) {
            throw std::runtime_error("--dump-state " + std::to_string(dump_frame) + ": the bundle recorded only " +
                                     std::to_string(d.inputs.frames.size()) + " match frames");
        }
        while (d.replayed < uint64_t(dump_frame)) run_one(d, hook);
        std::vector<uint8_t> state(d.machine->state_size());
        d.machine->save_state(state);
        std::ofstream out(dump_path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char *>(state.data()), std::streamsize(state.size()));
        if (!out) throw std::runtime_error("cannot write " + dump_path.string());
        std::array<uint8_t, 2048> buf{};
        uint8_t phase = 0;
        const size_t n = region_of(*d.machine, hook, buf, phase);
        std::cout << "STATE_DUMPED frame=" << dump_frame << " bytes=" << state.size() << " path=" << dump_path.string()
                  << " hash_crc=0x" << hex(f3rt::crc32(buf.data(), n), 8) << " phase=" << unsigned(phase) << "\n";
        return 0;
    }

    if (bundles.size() == 1) {
        const bool ok = verify_one(bundles[0], hook, until_for(bundles[0]));
        std::cout << (ok ? "REPLAY_OK" : "REPLAY_MISMATCH") << "\n";
        return ok ? 0 : 1;
    }

    Bundle &a = bundles[0];
    Bundle &b = bundles[1];
    if (a.seed != b.seed) std::cout << "WARNING seeds differ: " << a.seed << " vs " << b.seed << " (different sessions?)\n";

    // 1. The two peers must have applied identical inputs for every frame both recorded.
    const size_t common_inputs = std::min(a.inputs.frames.size(), b.inputs.frames.size());
    size_t first_input_diff = SIZE_MAX;
    for (size_t f = 0; f < common_inputs; ++f) {
        if (a.inputs.frames[f] != b.inputs.frames[f]) {
            first_input_diff = f;
            break;
        }
    }
    if (first_input_diff == SIZE_MAX) {
        std::cout << "INPUTS_IDENTICAL yes frames=" << common_inputs << "\n";
    } else {
        const auto &x = a.inputs.frames[first_input_diff];
        const auto &y = b.inputs.frames[first_input_diff];
        std::cout << "INPUTS_IDENTICAL NO first_frame=" << first_input_diff << " A=" << hex(x[0], 4) << "/" << hex(x[1], 4)
                  << " B=" << hex(y[0], 4) << "/" << hex(y[1], 4) << "\n";
    }

    // 2. Replay both in lockstep and compare the hashed bytes after every frame.
    const uint64_t until = std::min(until_for(a), until_for(b));
    std::array<uint8_t, 2048> buf_a{}, buf_b{};
    while (a.replayed < until) {
        run_one(a, hook);
        run_one(b, hook);
        uint8_t phase_a = 0, phase_b = 0;
        const size_t na = region_of(*a.machine, hook, buf_a, phase_a);
        const size_t nb = region_of(*b.machine, hook, buf_b, phase_b);
        const uint64_t completed = a.replayed;
        const bool same = phase_a == phase_b && na == nb && std::memcmp(buf_a.data(), buf_b.data(), na) == 0;
        if (same) continue;

        std::cout << "FIRST_DIVERGENCE frame=" << completed << " phase_a=" << unsigned(phase_a) << " phase_b=" << unsigned(phase_b)
                  << " len_a=" << na << " len_b=" << nb << "\n";
        if (phase_a != phase_b || na != nb) {
            std::cout << "  the two peers are hashing different regions here (the hash gate fired on one peer only)\n";
        }
        std::vector<uint32_t> addr_a(2048), addr_b(2048);
        const size_t aa = hook->sync_hash_addresses ? hook->sync_hash_addresses(phase_a, addr_a) : 0;
        const size_t ab = hook->sync_hash_addresses ? hook->sync_hash_addresses(phase_b, addr_b) : 0;
        size_t listed = 0;
        // Byte-for-byte compare by Machine address when both phases map the same address, else by index.
        if (phase_a == phase_b && na == nb) {
            for (size_t i = 0; i < na && listed < max_diff; ++i) {
                if (buf_a[i] == buf_b[i]) continue;
                std::cout << "  ";
                if (i < aa) std::cout << "0x" << std::hex << addr_a[i] << std::dec;
                else std::cout << "index " << i;
                std::cout << ": A=0x" << hex(buf_a[i], 2) << " B=0x" << hex(buf_b[i], 2) << "\n";
                ++listed;
            }
        } else {
            for (size_t i = 0; i < ab && i < nb && listed < max_diff; ++i) {
                // Compare B's byte against A's machine at the same address.
                const uint32_t addr = addr_b[i];
                const uint8_t va = a.machine->read8(addr);
                if (va == buf_b[i]) continue;
                std::cout << "  0x" << std::hex << addr << std::dec << ": A=0x" << hex(va, 2) << " B=0x" << hex(buf_b[i], 2) << "\n";
                ++listed;
            }
        }
        std::cout << "DIVERGENCE_ADDRESSES_LISTED " << listed << "\n";
        return 0;
    }
    std::cout << "NO_DIVERGENCE frames=" << until << "\n";
    return 0;
} catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 2;
}

} // namespace f3rt::tool
