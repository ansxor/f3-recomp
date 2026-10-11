#include "commands.hpp"
#include "../runner/runner.hpp"
#include "../runner/inputs.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/rom.hpp"
#include "capture_io.hpp"
#include "interpreter.hpp"

#if defined(F3RT_GAME_VIDEO)
#include "f3rt/game_video.hpp"
#endif

#include <chrono>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>

namespace f3rt::tool {

namespace {

void print_failure_state(const Machine &m, uint64_t seed, uint64_t frame, const std::string &error) {
    f3_cc_flush(const_cast<f3_cpu *>(&m.cpu));
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

void print_help(const char *prog) {
    std::cout << "Usage: " << prog << " gameplay [options]\n\n"
              << "Deterministic strict-native seeded gameplay regression harness.\n\n"
              << "Options:\n"
              << "  --rom-dir DIR          Path to ROM directory (defaults to F3RT_DEFAULT_ROM_DIR)\n"
              << "  --set SET              ROM set name (default: landmakrj)\n"
              << "  --seed N               PRNG seed for input schedule (default: 12345, or SEED env)\n"
              << "  --frames N             Number of frames to advance (default: 40000)\n"
              << "  --dump-dir DIR         Dump final state or the first video mismatch\n"
              << "  --capture-surface BMP  Save final frame BMP capture using f3rt::write_bmp\n"
              << "  --surface BMP          Alias for --capture-surface\n"
              << "  --sound-trace FILE     Record sound CPU and main mailbox bus events\n"
              << "  --sound-driver MODE    oracle (default) or native (generated driver)\n"
              << "  --audio-backend MODE   reference (default) or enhanced (threaded ROM sequencer)\n"
              << "  --wav FILE             Save audio\n"
              << "  --profile-out FILE     Merge main/sound entry counts (instrumented build)\n"
              << "  --video-diff           Compare game-owned layers and final RGB against FDP from frame 600\n"
              << "  --video-layer-mask N   Bits 0..3 PF, 4..7 sprites, 8 text (default: 511, all + RGB)\n"
              << "  --video-diff-every N   Sample interval (default: 120 frames)\n"
              << "  --json                 Output machine-readable JSON result\n"
              << "  --help, -h             Show this help message\n";
}

} // namespace

int cmd_gameplay(int argc, char **argv) try {
    runner::RunnerConfig cfg;
#ifdef F3RT_DEFAULT_ROM_DIR
    cfg.rom_dir = F3RT_DEFAULT_ROM_DIR;
#endif

    uint64_t seed = 12345;
    bool seed_specified = false;
    uint64_t target_frames = 40000;
    std::filesystem::path capture_surface;
    bool json_output = false;

    if (const char *env_seed = std::getenv("SEED")) {
        seed = std::stoull(env_seed);
        seed_specified = true;
    }

    for (int i = 0; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> const char * {
            if (i + 1 >= argc) throw std::runtime_error("Missing value for argument: " + arg);
            return argv[++i];
        };

        if (arg == "--rom-dir") {
            cfg.rom_dir = value();
        } else if (arg == "--set") {
            cfg.set = value();
        } else if (arg == "--seed") {
            seed = std::stoull(value());
            seed_specified = true;
        } else if (arg == "--frames") {
            target_frames = std::stoull(value());
        } else if (arg == "--dump-dir") {
            cfg.dump_dir = value();
        } else if (arg == "--capture-surface" || arg == "--surface") {
            capture_surface = value();
        } else if (arg == "--sound-trace") {
            cfg.sound_trace_path = value();
        } else if (arg == "--profile-out") {
            cfg.profile_path = value();
        } else if (arg == "--sound-driver") {
            cfg.sound_driver = value();
            if (cfg.sound_driver != "oracle" && cfg.sound_driver != "native") {
                throw std::runtime_error("--sound-driver must be oracle or native");
            }
        } else if (arg == "--audio-backend") {
            const std::string name = value();
            if (!Audio::parse_backend(name, cfg.audio_backend)) {
                throw std::runtime_error("--audio-backend must be reference or enhanced");
            }
        } else if (arg == "--wav") {
            cfg.wav_path = value();
        } else if (arg == "--video-diff") {
            cfg.video_diff = true;
        } else if (arg == "--video-layer-mask") {
            cfg.video_layer_mask = static_cast<unsigned>(std::stoul(value(), nullptr, 0));
        } else if (arg == "--video-diff-every") {
            cfg.video_diff_every = static_cast<unsigned>(std::stoul(value()));
        } else if (arg == "--json") {
            json_output = true;
        } else if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            return 0;
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    if (cfg.rom_dir.empty()) {
        throw std::runtime_error("ROM directory must be specified via --rom-dir or compiled F3RT_DEFAULT_ROM_DIR");
    }
    const bool enhanced = (cfg.audio_backend == Audio::Backend::Enhanced);
    if (enhanced && !cfg.sound_trace_path.empty()) {
        throw std::runtime_error("Enhanced audio does not emit sound CPU bus traces");
    }

    cfg.allow_fallback = false;
    cfg.translated = true;
    cfg.input_source = std::make_shared<runner::SeededScheduleInputSource>(
        seed, runner::ScheduleConfig{.versus = false});

    const auto start_time = std::chrono::steady_clock::now();
    runner::MachineRunner runner(cfg);
    auto &m = runner.machine();

    bool run_ok = runner.run_frames(target_frames, [&](uint64_t frame) -> bool {
        if (m.cpu.halted) {
            std::ostringstream ss;
            ss << "CPU halted at PC 0x" << std::hex << m.cpu.pc;
            print_failure_state(m, seed, frame, ss.str());
            return false;
        }
        if (m.fallback_instructions > 0) {
            print_failure_state(m, seed, frame, "Fallback instruction executed under strict native mode");
            return false;
        }
#if defined(F3RT_GAME_VIDEO)
        if (cfg.video_diff && frame >= 600 && (frame - 600) % cfg.video_diff_every == 0) {
            try {
                m.game_video->compare_layers(frame, cfg.video_layer_mask);
            } catch (...) {
                m.game_video->report(std::cerr);
                if (!cfg.dump_dir.empty()) dump_machine(m, cfg.dump_dir);
                throw;
            }
        }
#endif
        return true;
    });

    if (!run_ok) {
        return 1;
    }

    runner.finish();

    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    const double elapsed_sec = std::chrono::duration<double>(elapsed).count();
    const double fps = elapsed_sec > 0 ? double(m.frame) / elapsed_sec : 0.0;

    const uint32_t frame_crc = crc32(reinterpret_cast<const uint8_t *>(m.native_pixels().data()),
                                     m.native_pixels().size() * 4);

    if (!cfg.dump_dir.empty()) {
        dump_machine(m, cfg.dump_dir);
    }
    if (!capture_surface.empty()) {
        runner.save_bmp(capture_surface);
    }
#if defined(F3RT_GAME_VIDEO)
    if (m.game_video) {
        m.game_video->report(std::cout);
    }
#endif

    if (json_output) {
        std::cout << "{\"status\":\"SUCCESS\",\"set\":\"" << cfg.set << "\",\"seed\":" << seed
                  << ",\"frames\":" << m.frame << ",\"pc\":\"0x" << std::hex << m.cpu.pc
                  << "\",\"sound_pc\":\"0x" << m.sound_pc() << "\",\"frame_crc\":\"0x" << frame_crc
                  << "\",\"cycles\":" << std::dec << m.cpu.cycles
                  << ",\"native_blocks\":" << m.native_blocks
                  << ",\"fallback_instructions\":" << m.fallback_instructions
                  << ",\"audio_frames\":" << runner.audio_frames()
                  << ",\"audio_peak\":" << runner.audio_peak()
                  << ",\"nonzero_samples\":" << runner.nonzero_samples()
                  << ",\"fps\":" << fps << "}\n";
    } else {
        std::cout << "SUCCESS set=" << cfg.set
                  << " seed=" << seed
                  << " frames=" << m.frame
                  << " pc=0x" << std::hex << m.cpu.pc
                  << " sound_pc=0x" << m.sound_pc()
                  << " audio_backend=" << Audio::backend_name(cfg.audio_backend)
                  << " sound_driver=" << (enhanced ? "none" : cfg.sound_driver)
                  << " frame_crc=0x" << frame_crc << std::dec
                  << " cycles=" << m.cpu.cycles
                  << " native_blocks=" << m.native_blocks
                  << " fallback_instructions=" << m.fallback_instructions
                  << " audio_frames=" << runner.audio_frames()
                  << " audio_peak=" << runner.audio_peak()
                  << " nonzero_samples=" << runner.nonzero_samples()
                  << " fps=" << std::fixed << std::setprecision(1) << fps << std::defaultfloat
                  << '\n';
    }

    return 0;
} catch (const std::exception &e) {
    std::cerr << "REGRESSION ERROR: " << e.what() << '\n';
    return 1;
}

} // namespace f3rt::tool
