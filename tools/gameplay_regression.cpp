#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/game_video.hpp"
#include "interpreter.hpp"
#include "f3rt/rom.hpp"
#include "capture_io.hpp"
#include "sound_trace.hpp"
#include "gameplay_inputs.hpp"
#include "block_profile.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

#ifdef F3RT_GENERATED
#include "program.h"
#endif
#ifdef F3RT_SOUND_GENERATED
#include "sound_program.h"
#endif

namespace {

enum KeyAction {
    KEY_UP = 0,
    KEY_DOWN,
    KEY_LEFT,
    KEY_RIGHT,
    KEY_Z,
    KEY_X,
    KEY_C,
    KEY_COIN,
    KEY_START
};

void apply_key(f3rt::Machine &m, KeyAction action, bool pressed) {
    switch (action) {
    case KEY_UP:    m.set_input(1, 1, pressed); break;
    case KEY_DOWN:  m.set_input(1, 2, pressed); break;
    case KEY_LEFT:  m.set_input(1, 4, pressed); break;
    case KEY_RIGHT: m.set_input(1, 8, pressed); break;
    case KEY_Z:     m.set_input(0, 1, pressed); break;
    case KEY_X:     m.set_input(0, 2, pressed); break;
    case KEY_C:     m.set_input(0, 4, pressed); break;
    case KEY_START: m.set_input(0, 0x1000, pressed); break;
    case KEY_COIN:
        // Coin 1 on F3 is active low in system_inputs bit 4 (0x10)
        if (pressed) m.system_inputs &= ~0x10u;
        else m.system_inputs |= 0x10u;
        break;
    }
}

void print_failure_state(const f3rt::Machine &m, uint64_t seed, uint64_t frame, const std::string &error) {
    std::cerr << "REGRESSION FAILURE: " << error << "\n"
              << "  seed: " << seed << "\n"
              << "  frame: " << frame << "\n"
              << "  pc: 0x" << std::hex << m.cpu.pc << std::dec << "\n"
              << "  sound_pc: 0x" << std::hex << m.sound_pc() << std::dec << "\n"
              << "  sr: 0x" << std::hex << m.cpu.sr << std::dec
              << " halted: " << int(m.cpu.halted)
              << " stopped: " << int(m.cpu.stopped) << "\n"
              << "  cycles: " << m.cpu.cycles
              << " native_blocks: " << m.native_blocks
              << " fallback_instructions: " << m.fallback_instructions << "\n";

    std::cerr << "  registers:\n";
    for (int i = 0; i < 8; ++i) {
        std::cerr << "    d" << i << "=0x" << std::hex << std::setw(8) << std::setfill('0') << m.cpu.d[i]
                  << " a" << i << "=0x" << std::setw(8) << std::setfill('0') << m.cpu.a[i] << std::dec << "\n";
    }

    std::cerr << "  stack dump (a7=0x" << std::hex << m.cpu.a[7] << std::dec << "):\n";
    for (int i = 0; i < 8; ++i) {
        uint32_t addr = m.cpu.a[7] + uint32_t(i * 4);
        std::cerr << "    stk+" << std::setw(2) << std::setfill(' ') << (i * 4) << " (0x" << std::hex << addr << "): 0x"
                  << std::setw(8) << std::setfill('0') << f3_read32(const_cast<f3_cpu *>(&m.cpu), addr) << std::dec << "\n";
    }
}

} // namespace

int main(int argc, char **argv) try {
    std::filesystem::path romdir;
#ifdef F3RT_DEFAULT_ROM_DIR
    romdir = F3RT_DEFAULT_ROM_DIR;
#endif

    std::string set = "landmakrj";
    uint64_t seed = 12345;
    bool seed_specified = false;
    uint64_t target_frames = 40000;
    bool video_diff = false;
    unsigned video_layer_mask = 511;
    uint64_t video_diff_every = 120;
    std::filesystem::path dump_dir;
    std::filesystem::path capture_surface;
    std::filesystem::path sound_trace_path, wav_path, profile_path;

    std::string sound_driver = "oracle";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> const char * {
            if (i + 1 >= argc) throw std::runtime_error("Missing value for argument: " + arg);
            return argv[++i];
        };

        if (arg == "--rom-dir") {
            romdir = value();
        } else if (arg == "--set") {
            set = value();
        } else if (arg == "--seed") {
            seed = std::stoull(value());
            seed_specified = true;
        } else if (arg == "--frames") {
            target_frames = std::stoull(value());
        } else if (arg == "--dump-dir") {
            dump_dir = value();
        } else if (arg == "--capture-surface" || arg == "--surface") {
            capture_surface = value();
        } else if (arg == "--sound-trace") {
            sound_trace_path = value();
        } else if (arg == "--profile-out") {
            profile_path = value();
        } else if (arg == "--sound-driver") {
            sound_driver = value();
        } else if (arg == "--wav") {
            wav_path = value();
        } else if (arg == "--video-diff") {
            video_diff = true;
        } else if (arg == "--video-layer-mask") {
            video_layer_mask = unsigned(std::stoul(value(), nullptr, 0));
        } else if (arg == "--video-diff-every") {
            video_diff_every = std::stoull(value());
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: " << argv[0] << " [options]\n\n"
                      << "Deterministic strict-native seeded gameplay regression harness.\n\n"
                      << "Options:\n"
                      << "  --rom-dir DIR          Path to ROM directory (defaults to F3RT_DEFAULT_ROM_DIR if compiled)\n"
                      << "  --set SET              ROM set name (default: landmakrj)\n"
                      << "  --seed N               PRNG seed for input schedule (default: 12345, or SEED env)\n"
                      << "  --frames N             Number of frames to advance (default: 40000)\n"
                      << "  --dump-dir DIR         Dump final state or the first video mismatch\n"
                      << "  --capture-surface BMP  Save final frame BMP capture using f3rt::write_bmp\n"
                      << "  --sound-trace FILE     Record sound CPU and main mailbox bus events\n"
                      << "  --sound-driver MODE    oracle (default) or native (generated driver)\n"
                      << "  --wav FILE             Save audio\n"
                      << "  --profile-out FILE     Merge main/sound entry counts (instrumented build; flush every 30s and at exit)\n"
                      << "  --video-diff           Compare game-owned layers and final RGB against FDP from frame 600\n"
                      << "  --video-layer-mask N   Bits 0..3 PF, 4..7 sprites, 8 text (default: 511, all + RGB)\n"
                      << "  --video-diff-every N   Sample interval (default: 120 frames)\n"
                      << "  --help, -h             Show this help message\n";
            return 0;
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    if (!seed_specified) {
        const char *env_seed = std::getenv("SEED");
        if (env_seed && *env_seed) {
            seed = std::strtoull(env_seed, nullptr, 0);
        }
    }

    if (romdir.empty()) {
        throw std::runtime_error("ROM directory must be specified via --rom-dir or compiled F3RT_DEFAULT_ROM_DIR");
    }
    if (set != "landmakrj") {
        throw std::runtime_error("Generated regression executable requires landmakrj");
    }

    if (target_frames == 0) {
        throw std::runtime_error("--frames must be positive");
    }
    if (sound_driver != "oracle" && sound_driver != "native")
        throw std::runtime_error("--sound-driver must be oracle or native");
    if (!profile_path.empty() && sound_driver != "native")
        throw std::runtime_error("Profiling requires --sound-driver native");

    auto machine = std::make_unique<f3rt::Machine>(f3rt::RomSet::load(romdir, set));
    auto &m = *machine;
    f3rt::BlockProfileSession profile(m.roms, profile_path);
    if (!sound_trace_path.empty()) m.sound_trace=std::make_unique<f3rt::SoundTrace>(sound_trace_path);
    if (sound_driver == "native") {
#ifdef F3RT_SOUND_GENERATED
        m.use_native_sound(f3_sound_blocks, f3_sound_block_count,
                           {f3_sound_excluded_ranges, f3_sound_excluded_count});
#else
        throw std::runtime_error("Native sound requires a generated sound program (F3_ROM_DIR)");
#endif
    }
    std::unique_ptr<f3rt::WavWriter> wav;
    if (!wav_path.empty()) wav=std::make_unique<f3rt::WavWriter>(wav_path,m.audio->sample_rate());
    if (video_diff) {
        if (!video_diff_every || !video_layer_mask || (video_layer_mask & ~511u))
            throw std::runtime_error("Video diff requires a positive interval and nine-layer mask");
        m.game_video = std::make_unique<f3rt::GameVideo>(m);
    }

    // Strict native mode: absolutely no fallback allowed
    m.allow_main_fallback = false;

#ifdef F3RT_GENERATED
    if (!f3_generated_register(&m.cpu)) {
        throw std::runtime_error("f3_generated_register failed to register recompiled blocks");
    }
#else
    throw std::runtime_error("gameplay_regression requires compiled generated blocks (F3RT_GENERATED)");
#endif

    static const KeyAction keys[] = {
        KEY_UP, KEY_DOWN, KEY_LEFT, KEY_RIGHT, KEY_Z, KEY_X, KEY_C
    };

    uint64_t rng = seed;
    std::array<int16_t, 8192> audio_buffer{};
    uint64_t audio_frames = 0;
    uint64_t nonzero_samples = 0;
    int audio_peak = 0;

    const auto start_time = std::chrono::steady_clock::now();

    while (m.frame < target_frames) {
        profile.tick();
        uint64_t f = m.frame;
        // Deterministic schedule using shared config (retained exact timing)
        constexpr f3rt::test::ScheduleConfig cfg{.versus = false};
        if (f == cfg.p1_coin_frame) apply_key(m, KEY_COIN, true);
        if (f == cfg.p1_coin_frame + cfg.p1_coin_duration) apply_key(m, KEY_COIN, false);

        // Frames [800, 2400): start pulse every 90 frames for 5 frames
        if (f >= cfg.start_begin && f < cfg.start_end && f % cfg.start_period == 0) apply_key(m, KEY_START, true);
        if (f >= cfg.start_begin && f < cfg.start_end && f % cfg.start_period == cfg.start_pulse_len) apply_key(m, KEY_START, false);

        if (f >= cfg.mashing_begin && f % cfg.mashing_period == 0) {
            rng = rng * 6364136223846793005ULL + 1442695040888963407ULL;
            auto k = keys[(rng >> 33) % 7];
            apply_key(m, k, ((rng >> 20) & 1) != 0);
        }
        bool ok = false;
        try {
            ok = m.run_frame(/*translated=*/true);
        } catch (const std::exception &e) {
            print_failure_state(m, seed, f, e.what());
            return 1;
        }

        if (!ok || m.cpu.halted) {
            std::ostringstream ss;
            ss << "CPU halted at PC 0x" << std::hex << m.cpu.pc;
            print_failure_state(m, seed, f, ss.str());
            return 1;
        }

        if (m.fallback_instructions > 0) {
            print_failure_state(m, seed, f, "Fallback instruction executed under strict native mode");
            return 1;
        }
        if (video_diff && m.frame >= 600 && (m.frame - 600) % video_diff_every == 0) {
            try {
                m.game_video->compare_layers(m.frame, video_layer_mask);
            } catch (...) {
                m.game_video->report(std::cerr);
                if (!dump_dir.empty()) f3rt::dump_machine(m, dump_dir);
                throw;
            }
        }

        // Drain audio output
        size_t count = 0;
        while ((count = m.audio->render(audio_buffer.data(), audio_buffer.size() / 2)) != 0) {
            audio_frames += count;
            if (wav) wav->append(std::span(audio_buffer.data(),count*2));
            for (size_t i = 0; i < count * 2; ++i) {
                audio_peak = std::max(audio_peak, std::abs(int(audio_buffer[i])));
                nonzero_samples += (audio_buffer[i] != 0);
            }
        }
    }
    profile.flush();

    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    const double elapsed_sec = std::chrono::duration<double>(elapsed).count();
    const double fps = elapsed_sec > 0 ? double(m.frame) / elapsed_sec : 0.0;

    const uint32_t frame_crc = f3rt::crc32(reinterpret_cast<const uint8_t *>(m.pixels.data()), m.pixels.size() * 4);

    if (!dump_dir.empty()) {
        f3rt::dump_machine(m, dump_dir);
    }
    if (!capture_surface.empty()) {
        if (capture_surface.has_parent_path()) {
            std::filesystem::create_directories(capture_surface.parent_path());
        }
        f3rt::write_bmp(capture_surface, m.pixels);
    }
    if (m.game_video) m.game_video->report(std::cout);
    if (m.sound_trace) m.sound_trace->finish(m);

    std::cout << "SUCCESS set=" << set
              << " seed=" << seed
              << " frames=" << m.frame
              << " pc=0x" << std::hex << m.cpu.pc
              << " sound_pc=0x" << m.sound_pc()
              << " sound_driver=" << sound_driver
              << " frame_crc=0x" << frame_crc << std::dec
              << " cycles=" << m.cpu.cycles
              << " native_blocks=" << m.native_blocks
              << " fallback_instructions=" << m.fallback_instructions
              << " audio_frames=" << audio_frames
              << " audio_peak=" << audio_peak
              << " nonzero_samples=" << nonzero_samples
              << " fps=" << std::fixed << std::setprecision(1) << fps << std::defaultfloat
              << '\n';

    return 0;
} catch (const std::exception &e) {
    std::cerr << "REGRESSION ERROR: " << e.what() << '\n';
    return 1;
}
