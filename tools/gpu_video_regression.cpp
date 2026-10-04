#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/game_video.hpp"
#include "f3rt/video.hpp"
#include "f3rt/rom.hpp"
#include "gpu_video.hpp"
#include "capture_io.hpp"
#include "gameplay_inputs.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef F3RT_GENERATED
#include "program.h"
#endif
#ifdef F3RT_SOUND_GENERATED
#include "sound_program.h"
#endif

namespace {
using Clock = std::chrono::steady_clock;
constexpr std::array<const char *, 10> names{"pf0", "pf1", "pf2", "pf3", "sp0", "sp1", "sp2", "sp3", "text", "composite"};
struct Options {
    std::filesystem::path rom_dir, dump_dir;
    uint64_t seed = 12345, frames = 4000, every = 1, inject_frame = 1407;
    f3rt::GameVideoOptions video;
    bool layers = false, bench = false;
    std::array<bool, 5> injections{};
};
uint64_t number(const std::string &text) {
    if (text.empty() || text.front() == '-') throw std::runtime_error("Expected unsigned integer: " + text);
    size_t used = 0;
    auto value = std::stoull(text, &used, 0);
    if (used != text.size()) throw std::runtime_error("Invalid integer: " + text);
    return value;
}
Options parse(int argc, char **argv) {
    Options o;
#ifdef F3RT_DEFAULT_ROM_DIR
    o.rom_dir = F3RT_DEFAULT_ROM_DIR;
#endif
    if (const char *seed = std::getenv("SEED"); seed && *seed) o.seed = number(seed);
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (++i >= argc) throw std::runtime_error("Missing value for " + arg);
            return argv[i];
        };
        if (arg == "--rom-dir") o.rom_dir = value();
        else if (arg == "--dump-dir") o.dump_dir = value();
        else if (arg == "--seed") o.seed = number(value());
        else if (arg == "--frames") o.frames = number(value());
        else if (arg == "--every") o.every = number(value());
        else if (arg == "--scale") {
            auto n = number(value());
            if (!n || n > f3rt::GameVideoOptions::max_scale) throw std::runtime_error("Scale must be 1..4");
            o.video.scale = unsigned(n);
        } else if (arg == "--border") {
            auto n = number(value());
            if (n > f3rt::GameVideoOptions::max_border) throw std::runtime_error("Border must be 0..160");
            o.video.border = unsigned(n);
        } else if (arg == "--layers") o.layers = true;
        else if (arg == "--bench") o.bench = true;
        else if (arg == "--inject-frame") o.inject_frame = number(value());
        else if (arg == "--inject-bitmap") o.injections[0] = true;
        else if (arg == "--inject-trails") o.injections[1] = true;
        else if (arg == "--inject-globalflip") o.injections[2] = true;
        else if (arg == "--inject-unknown") o.injections[3] = true;
        else if (arg == "--inject-ending") o.injections[4] = true;
        else if (arg == "--sound-driver") {
            if (value() != "native") throw std::runtime_error("GPU regression requires --sound-driver native");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Strict-native Land Maker GPU parity (no interpolation).\n"
                "--rom-dir DIR --seed N --frames N (4000) --scale N (1..4) --border N (0..160)\n"
                "--every N (1) --layers (all nine isolated contributions plus composite)\n"
                "--bench (600-frame varied-scene warmup; 100 repeats on final supported snapshot)\n"
                "--dump-dir DIR (external PNG captures) --sound-driver native\n"
                "--inject-frame N (1407) --inject-bitmap --inject-trails --inject-globalflip\n"
                "--inject-unknown --inject-ending (induced producer boundary, NOT played ending)\n"
                "Run each scale 1..4 with border 0 and 48 for the parity matrix.\n";
            std::exit(0);
        } else throw std::runtime_error("Unknown argument: " + arg);
    }
    if (o.rom_dir.empty() || !o.frames || !o.every) throw std::runtime_error("ROM directory, positive frames and positive every are required");
    if (std::any_of(o.injections.begin(), o.injections.end(), [](bool b) { return b; }) &&
        (!o.inject_frame || o.inject_frame > o.frames)) throw std::runtime_error("Injection frame must be inside run (1..frames)");
    return o;
}
void apply_inputs(f3rt::Machine &m, uint16_t word) {
    constexpr std::array<uint32_t, 7> masks{1, 2, 4, 8, 1, 2, 4};
    for (unsigned i = 0; i < 7; ++i) m.set_input(i < 4 ? 1 : 0, masks[i], (word & (1u << i)) != 0);
    m.set_input(0, 0x1000, (word & 0x80) != 0);
    if (word & 0x100) m.system_inputs &= ~0x10u;
    else m.system_inputs |= 0x10u;
}
void advance(f3rt::Machine &m) {
    if (!m.run_frame(true) || m.cpu.halted || m.fallback_instructions)
        throw std::runtime_error("Strict-native frame failed at frame " + std::to_string(m.frame) + " PC " + std::to_string(m.cpu.pc));
}
std::vector<uint8_t> snapshot(const f3rt::Machine &m) {
    std::vector<uint8_t> state(m.state_size());
    m.save_state(state);
    return state;
}
void capture_png(const std::filesystem::path &path, std::span<const uint32_t> pixels, f3rt::GameVideoOptions o) {
    auto *surface = SDL_CreateSurfaceFrom(int(o.width()), int(o.height()), SDL_PIXELFORMAT_ARGB8888,
        const_cast<uint32_t *>(pixels.data()), int(o.width() * 4));
    if (!surface) throw std::runtime_error(SDL_GetError());
    const bool saved = SDL_SavePNG(surface, path.string().c_str());
    SDL_DestroySurface(surface);
    if (!saved) throw std::runtime_error("PNG write failed: " + path.string() + ": " + SDL_GetError());
}
// Streaming standard CRC32, in explicit little-endian order for audio samples.
void crc_byte(uint32_t &crc, uint8_t byte) {
    crc ^= byte;
    for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ ((crc & 1) ? 0xedb88320u : 0u);
}
struct Harness {
    f3rt::Machine &m;
    f3rt::GpuVideo &gpu;
    const Options &o;
    std::vector<uint32_t> cpu, device;
    std::vector<uint8_t> state_before, state_after;
    std::vector<double> cpu_frame_ms, gpu_frame_ms;
    std::vector<double> cpu_budget_ms, gpu_budget_ms;
    double native_ms = 0;
    std::array<uint64_t, 10> samples{}, mismatches{};
    uint64_t sampled_frames = 0, fallback_samples = 0, supported_samples = 0;
    uint64_t injected_samples = 0, captures = 0;
    Harness(f3rt::Machine &machine, f3rt::GpuVideo &video, const Options &options)
        : m(machine), gpu(video), o(options), cpu(size_t(o.video.width()) * o.video.height()), device(cpu.size()),
          state_before(m.state_size()), state_after(state_before.size()) {
        if (o.bench) {
            cpu_frame_ms.reserve(o.frames + 1); gpu_frame_ms.reserve(o.frames + 1);
            cpu_budget_ms.reserve(o.frames); gpu_budget_ms.reserve(o.frames);
        }
    }
    void capture(const std::string &tag) {
        if (o.dump_dir.empty()) return;
        const auto stem = "frame_" + std::to_string(m.frame) + "_s" + std::to_string(o.video.scale) + "_b" + std::to_string(o.video.border) + "_" + tag;
        capture_png(o.dump_dir / (stem + "_cpu.png"), cpu, o.video);
        capture_png(o.dump_dir / (stem + "_gpu.png"), device, o.video);
        f3rt::write_bmp(o.dump_dir / (stem + "_native.bmp"), m.pixels);
    }
    void compare(unsigned index, const std::string &tag, bool injected) {
        const unsigned mask = index == 9 ? 511 : 1u << index;
        m.save_state(state_before);
        const auto cpu_start = Clock::now();
        m.game_video->render_reference(cpu, o.video, mask);
        const double cpu_ms = std::chrono::duration<double, std::milli>(Clock::now() - cpu_start).count();
        m.save_state(state_after);
        if (state_after != state_before) throw std::runtime_error("CPU reference mutated native state at frame " + std::to_string(m.frame));
        const auto gpu_start = Clock::now();
        gpu.draw(m.game_video->gpu_scene(), device, mask);
        const double gpu_ms = std::chrono::duration<double, std::milli>(Clock::now() - gpu_start).count();
        if (o.bench && m.frame > 600 && index == 9 && !injected && !m.game_video->gpu_scene().fallback) {
            cpu_frame_ms.push_back(cpu_ms); gpu_frame_ms.push_back(gpu_ms);
            if (native_ms > 0) {
                cpu_budget_ms.push_back(native_ms + cpu_ms);
                gpu_budget_ms.push_back(native_ms + gpu_ms);
            }
        }
        m.save_state(state_after);
        if (state_after != state_before) throw std::runtime_error("GPU presentation mutated native state at frame " + std::to_string(m.frame));
        if (index == 9 && m.game_video->gpu_scene().fallback) {
            // Independently check the fallback contract, not just two implementations
            // that might both expand an incoherent or incorrectly bordered snapshot.
            const unsigned left = o.video.border * o.video.scale;
            for (unsigned y = 0; y < o.video.height(); ++y)
                for (unsigned x = 0; x < o.video.width(); ++x) {
                    const uint32_t expected = x < left || x >= left + 320 * o.video.scale
                        ? 0xff000000u : m.pixels[(y / o.video.scale) * 320 + (x - left) / o.video.scale];
                    if (cpu[size_t(y) * o.video.width() + x] != expected)
                        throw std::runtime_error("CPU fallback reference disagrees with actual native oracle at frame " +
                            std::to_string(m.frame) + " x=" + std::to_string(x) + " y=" + std::to_string(y));
                }
        }
        uint64_t count = 0;
        size_t first = 0;
        for (size_t i = 0; i < cpu.size(); ++i) if (cpu[i] != device[i]) {
            if (!count) first = i;
            ++count;
        }
        ++samples[index]; mismatches[index] += count;
        if (count) {
            std::cerr << "FIRST MISMATCH seed=" << o.seed << " frame=" << m.frame << " layer=" << names[index]
                << " scenario=" << tag << " injected=" << injected << " fallback=" << m.game_video->gpu_scene().fallback
                << " count=" << count << " x=" << first % o.video.width() << " y=" << first / o.video.width()
                << " cpu_argb=0x" << std::hex << std::setw(8) << std::setfill('0') << cpu[first]
                << " gpu_argb=0x" << std::setw(8) << device[first] << std::dec << std::setfill(' ') << '\n';
            capture(tag + "_mismatch_" + names[index]);
            throw std::runtime_error("GPU parity mismatch");
        }
    }
    void sample(const std::string &tag = "gameplay", bool injected = false, bool force_capture = false) {
        ++sampled_frames;
        if (injected) ++injected_samples;
        if (m.game_video->gpu_scene().fallback) ++fallback_samples;
        else ++supported_samples;
        if (o.layers && !m.game_video->gpu_scene().fallback)
            for (unsigned i = 0; i < 9; ++i) compare(i, tag, injected);
        compare(9, tag, injected);
        if (force_capture || (captures < 3 && !m.game_video->gpu_scene().fallback)) { capture(tag); ++captures; }
    }
    void report() const {
        std::cout << "PARITY sampled_frames=" << sampled_frames << " supported=" << supported_samples
            << " fallback=" << fallback_samples << " injected=" << injected_samples << '\n';
        for (unsigned i = 0; i < 10; ++i)
            std::cout << "  " << names[i] << " samples=" << samples[i] << " mismatching_pixels=" << mismatches[i] << '\n';
    }
};
void observe(f3rt::Machine &m, uint32_t pc) {
    const auto old = m.cpu.pc;
    m.cpu.pc = pc;
    m.game_video->observe();
    m.cpu.pc = old;
}
void put16(std::span<uint8_t> bytes, size_t offset, uint16_t value) {
    bytes[offset] = uint8_t(value >> 8); bytes[offset + 1] = uint8_t(value);
}
void inject(Harness &h, unsigned kind) {
    auto &m = h.m;
    if (kind == 0) {
        // The known default-profile producer reads the actual ROM profile.
        // Temporarily give it bitmap mode, and upload the same control to FDP.
        constexpr size_t profile = 0x5d74 + 16;
        const uint8_t old = m.roms.main.at(profile);
        m.roms.main[profile] |= 0x20;
        observe(m, 0x5cd8);
        m.roms.main[profile] = old;
        for (unsigned y = 24; y < 256; ++y) {
            put16(m.graphics, 0x20400 + y * 2, 1);
            put16(m.graphics, 0x26000 + y * 2, 0x2000);
        }
    } else if (kind == 1 || kind == 2) {
        const uint16_t command = kind == 1 ? 2 : 0x2000;
        put16(m.ram, 0x7a1e, command);
        observe(m, 0x43e0);
        // Feed the same actual command to the oracle's sprite descriptor reader.
        // Both banks are covered because the active bank is retained hardware state.
        for (size_t bank : {size_t(0), size_t(0x8000)}) {
            put16(m.graphics, bank + 6, 0x8000);
            put16(m.graphics, bank + 10, command);
            for (size_t slot = 1; slot < 1024; ++slot) {
                const size_t descriptor = bank + slot * 16;
                if (m.graphics[descriptor + 6] & 0x80)
                    put16(m.graphics, descriptor + 10, command);
            }
        }
        m.game_video->render_frame(); // Latch command; current scanout remains old.
    } else {
        // Genuine PC/address ownership guard, not a synthetic scene fallback bit.
        m.game_video->observe_write(kind == 4 ? 0xfe620 : 0xdead00, kind == 4 ? 0x626000 : 0x610000);
    }
    m.game_video->render_frame();
    if (!m.game_video->gpu_scene().fallback) throw std::runtime_error("Injected producer did not reach oracle fallback");
}
// Saving after several trail frames must retain every intervening sprite list.
void verify_trail_history(Harness &h) {
    auto &m = h.m;
    const auto baseline = snapshot(m);
    const auto baseline_crc = m.state_crc();
    auto peer_owner = std::make_unique<f3rt::Machine>(m.roms);
    auto &peer = *peer_owner;
#ifdef F3RT_SOUND_GENERATED
    peer.use_native_sound(f3_sound_blocks, f3_sound_block_count);
#endif
    peer.game_video = std::make_unique<f3rt::GameVideo>(peer, f3rt::GameVideoMode::Game, h.o.video);
    peer.load_state(baseline);
    for (unsigned frame = 0; frame < 4; ++frame) {
        for (auto *machine : {&m, &peer}) {
            put16(machine->ram, 0x7a1e, 2);
            observe(*machine, 0x43e0);
            put16(machine->ram, 0x7a16, uint16_t(frame * 9));
            observe(*machine, 0x43b0);
            machine->game_video->render_frame();
        }
    }
    const auto gpu_state = snapshot(m), cpu_state = snapshot(peer);
    size_t differences = 0;
    for (size_t i = 0; i < gpu_state.size(); ++i) differences += gpu_state[i] != cpu_state[i];
    m.load_state(baseline);
    if (m.state_crc() != baseline_crc) throw std::runtime_error("Trail branch restore changed canonical state");
    if (differences) throw std::runtime_error("Deferred GPU trail snapshots differ from CPU: " + std::to_string(differences) + " bytes");
    std::cout << "SNAPSHOT trails_history_frames=4 deferred_save=1 byte_mismatches=0 (induced branch)\n";
}
void verify_cpu_backend(Harness &h, std::span<const uint8_t> pre) {
    auto peer_owner = std::make_unique<f3rt::Machine>(h.m.roms);
    auto &peer = *peer_owner;
    peer.allow_main_fallback = false;
#ifdef F3RT_GENERATED
    if (!f3_generated_register(&peer.cpu)) throw std::runtime_error("CPU peer registration failed");
#endif
#ifdef F3RT_SOUND_GENERATED
    peer.use_native_sound(f3_sound_blocks, f3_sound_block_count);
#endif
    peer.game_video = std::make_unique<f3rt::GameVideo>(peer, f3rt::GameVideoMode::Game, h.o.video);
    peer.load_state(pre);
    advance(peer);
    const auto gpu_state = snapshot(h.m), cpu_state = snapshot(peer);
    size_t bytes = 0, pixels = 0;
    for (size_t i = 0; i < gpu_state.size(); ++i) bytes += gpu_state[i] != cpu_state[i];
    const auto original_cpu = peer.game_video->presentation();
    for (size_t i = 0; i < h.device.size(); ++i) pixels += h.device[i] != original_cpu[i];
    if (bytes || pixels)
        throw std::runtime_error("Independent CPU backend differs: " + std::to_string(bytes) +
                                 " canonical bytes / " + std::to_string(pixels) + " GPU pixels");
    std::cout << "SNAPSHOT supported_cpu_backend frame=" << peer.frame
              << " byte_mismatches=0 gpu_argb_mismatches=0\n";
}

void timing(const char *name, std::vector<double> values) {
    if (values.empty()) {
        std::cout << "BENCH " << name << " repeats=0 (no supported post-warmup samples)\n";
        return;
    }
    const double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    std::sort(values.begin(), values.end());
    std::cout << "BENCH " << name << " repeats=" << values.size() << " mean_ms=" << mean
        << " p95_ms=" << values[size_t(std::ceil(values.size() * 0.95)) - 1] << " worst_ms=" << values.back() << '\n';
}
void benchmark(Harness &h) {
    constexpr unsigned repeats = 100;
    const auto crc = h.m.state_crc();
    std::array<std::vector<double>, 3> times;
    for (auto &v : times) v.reserve(repeats);
    for (unsigned i = 0; i < repeats + 5; ++i) {
        for (unsigned mode = 0; mode < 3; ++mode) {
            const auto start = Clock::now();
            if (mode < 2) h.m.game_video->render_reference(h.cpu, h.o.video, 511, mode == 0);
            else h.gpu.draw(h.m.game_video->gpu_scene(), h.device);
            const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            if (i >= 5) times[mode].push_back(elapsed);
        }
    }
    if (h.m.state_crc() != crc) throw std::runtime_error("Benchmark mutated native state");
    h.compare(9, "benchmark", false);
    std::cout << "BENCH snapshot_frame=" << h.m.frame << " supported=1 warmup=5\n";
    timing("CPU_serial_reference", times[0]); timing("CPU_threaded_reference", times[1]);
    timing("GPU_submit_fence_readback_including_driver_and_transfer", times[2]);
}
struct SdlLifetime {
    SdlLifetime() { if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(std::string("SDL video initialization: ") + SDL_GetError()); }
    ~SdlLifetime() { SDL_Quit(); }
};
} // namespace

int main(int argc, char **argv) try {
    const auto o = parse(argc, argv);
    SdlLifetime sdl;
    auto owner = std::make_unique<f3rt::Machine>(f3rt::RomSet::load(o.rom_dir, "landmakrj"));
    auto &m = *owner;
    m.allow_main_fallback = false;
#ifdef F3RT_GENERATED
    if (!f3_generated_register(&m.cpu)) throw std::runtime_error("Generated native registration failed");
#else
    throw std::runtime_error("GPU regression requires generated strict-native main blocks");
#endif
#ifdef F3RT_SOUND_GENERATED
    m.use_native_sound(f3_sound_blocks, f3_sound_block_count);
#else
    throw std::runtime_error("GPU regression requires generated native sound blocks");
#endif
    m.game_video = std::make_unique<f3rt::GameVideo>(m, f3rt::GameVideoMode::Game, o.video);
    m.game_video->enable_gpu_presentation(true, true);
    f3rt::GpuVideo gpu(nullptr, o.video, m.video->playfield_tiles(), m.video->sprite_tiles());
    if (!o.dump_dir.empty()) std::filesystem::create_directories(o.dump_dir);
    std::cout << "GPU driver=" << gpu.driver() << " seed=" << o.seed << " scale=" << o.video.scale << " border=" << o.video.border << '\n';
    Harness h(m, gpu, o);
    f3rt::test::GameplaySchedule schedule(o.seed, f3rt::test::ScheduleConfig{.versus = false});
    std::array<int16_t, 8192> audio{};
    uint64_t audio_frames = 0, nonzero = 0, fallback_frames = 0, supported_frames = 0, transitions = 0;
    uint32_t audio_crc = 0xffffffffu;
    int peak = 0;
    bool previous_fallback = true;
    std::vector<double> native_frame_ms;
    if (o.bench) native_frame_ms.reserve(o.frames);
    std::vector<uint8_t> last_supported_pre;
    constexpr std::array<const char *, 5> scenarios{"bitmap", "trails", "globalflip", "unknown-producer", "ending-producer-boundary-NOT-played-ending"};
    const auto start = Clock::now();
    try {
        while (m.frame < o.frames) {
            apply_inputs(m, schedule.step(m.frame)[0]);
            const bool injection_frame = m.frame + 1 == o.inject_frame && std::any_of(o.injections.begin(), o.injections.end(), [](bool b) { return b; });
            auto pre = (o.bench || injection_frame) ? snapshot(m) : std::vector<uint8_t>{};
            const auto native_start = Clock::now();
            advance(m);
            const bool fallback = m.game_video->gpu_scene().fallback;
            h.native_ms = o.bench ? std::chrono::duration<double, std::milli>(Clock::now() - native_start).count() : 0;
            if (o.bench && !fallback && m.frame > 600) native_frame_ms.push_back(h.native_ms);
            if (fallback) ++fallback_frames; else ++supported_frames;
            if (fallback != previous_fallback) ++transitions;
            previous_fallback = fallback;
            if (o.bench && !fallback) last_supported_pre = pre;
            if ((m.frame - 1) % o.every == 0 || m.frame == o.frames || injection_frame) h.sample("gameplay", false, m.frame == o.frames);
            if (injection_frame) {
                if (fallback) throw std::runtime_error("Scheduled injection needs a supported baseline; choose --inject-frame in gameplay");
                const auto baseline = snapshot(m);
                const uint32_t baseline_crc = m.state_crc();
                const auto baseline_blocks = m.native_blocks;
                if (o.injections[1]) {
                    verify_trail_history(h);
                    m.load_state(pre); advance(m);
                    if (m.state_crc() != baseline_crc) throw std::runtime_error("Trail history recovery changed native state");
                }
                for (unsigned kind = 0; kind < scenarios.size(); ++kind) if (o.injections[kind]) {
                    m.load_state(baseline);
                    inject(h, kind);
                    h.sample(scenarios[kind], true, true);
                    // Restore pre-scanout and execute the original frame, rebuilding
                    // host snapshots with the correct sprite lag and producer state.
                    m.load_state(pre); advance(m);
                    if (m.game_video->gpu_scene().fallback || m.state_crc() != baseline_crc)
                        throw std::runtime_error("Oracle-to-supported recovery changed baseline native state");
                    h.sample(std::string(scenarios[kind]) + "_restored", true);
                    std::cout << "INJECTED scenario=" << scenarios[kind] << " frame=" << m.frame << " fallback_and_recovery=exact (branch only)\n";
                }
                m.native_blocks = baseline_blocks; // Diagnostic replays are not gameplay execution.
            }
            size_t count;
            while ((count = m.audio->render(audio.data(), audio.size() / 2)) != 0) {
                audio_frames += count;
                for (size_t i = 0; i < count * 2; ++i) {
                    peak = std::max(peak, std::abs(int(audio[i]))); nonzero += audio[i] != 0;
                    crc_byte(audio_crc, uint8_t(audio[i])); crc_byte(audio_crc, uint8_t(uint16_t(audio[i]) >> 8));
                }
            }
        }
        if (o.bench) {
            if (last_supported_pre.empty()) throw std::runtime_error("No supported snapshot available for benchmark");
            const auto final_state = snapshot(m);
            const auto final_crc = m.state_crc();
            const auto final_blocks = m.native_blocks;
            m.load_state(last_supported_pre); advance(m);
            if (m.game_video->gpu_scene().fallback) throw std::runtime_error("Supported benchmark replay became fallback");
            h.native_ms = 0; // Frozen-snapshot repetitions are not native frame budgets.
            benchmark(h);
            timing("CPU_native_emulation_plus_native_compositor_and_scene_export", native_frame_ms);
            timing("CPU_threaded_varied_supported_compositor", h.cpu_frame_ms);
            timing("GPU_varied_supported_submit_fence_readback", h.gpu_frame_ms);
            timing("CPU_native_plus_threaded_render_budget", h.cpu_budget_ms);
            timing("GPU_native_plus_fenced_render_budget", h.gpu_budget_ms);
            verify_cpu_backend(h, last_supported_pre);
            m.load_state(final_state);
            m.native_blocks = final_blocks;
            if (m.state_crc() != final_crc) throw std::runtime_error("Benchmark replay failed to restore final native state");
        }
    } catch (...) { h.report(); throw; }
    h.report();
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    const uint32_t frame_crc = f3rt::crc32(reinterpret_cast<const uint8_t *>(m.pixels.data()), m.pixels.size() * sizeof(uint32_t));
    std::cout << "SUCCESS seed=" << o.seed << " frames=" << m.frame << " supported_frames=" << supported_frames
        << " actual_fallback_frames=" << fallback_frames << " actual_oracle_transitions=" << transitions
        << " native_blocks=" << m.native_blocks << " fallback_instructions=" << m.fallback_instructions
        << " cycles=" << m.cpu.cycles << " sound_driver=native audio_frames=" << audio_frames
        << " audio_peak=" << peak << " nonzero_audio_samples=" << nonzero
        << " audio_crc=0x" << std::hex << (audio_crc ^ 0xffffffffu) << " frame_crc=0x" << frame_crc
        << " state_crc=0x" << m.state_crc() << std::dec << " elapsed_seconds=" << seconds
        << " harness_fps_including_diagnostics=" << m.frame / seconds << '\n';
    return 0;
} catch (const std::exception &e) {
    std::cerr << "GPU REGRESSION ERROR: " << e.what() << '\n';
    return 1;
}
