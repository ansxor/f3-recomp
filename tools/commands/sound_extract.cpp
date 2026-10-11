#include "commands.hpp"
#include "../runner/runner.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

namespace f3rt::tool {

namespace {

void print_help(const char *prog) {
    std::cout << "Usage: " << prog << " sound-extract [options]\n\n"
              << "Standalone sound extraction and stimulus injection tool for Land Maker (Taito F3).\n\n"
              << "Options:\n"
              << "  --rom-dir DIR          Path to ROM directory\n"
              << "  --set SET              ROM set name (default: landmakrj)\n"
              << "  --packet HEX           Inject length-prefixed packet at event origin (can be repeated)\n"
              << "  --at SECONDS:HEX       Schedule packet injection at SECONDS from event origin (can be repeated)\n"
              << "  --seconds DURATION     Duration in seconds to advance audio after event origin (default: 5.0)\n"
              << "  --boot-frames N        Interpreted main CPU boot frames before freezing (default: 900, ~15.3s)\n"
              << "  --sound-trace FILE     Record lossless bus sound trace from cold boot (F3SND2 format)\n"
              << "  --wav FILE             Save generated stereo 16-bit PCM audio as WAV\n"
              << "  --wav-window full|event\n"
              << "                         full: record cold boot through extraction (default)\n"
              << "                         event: record only from event time origin onward\n"
              << "  --sound-driver oracle|native\n"
              << "                         oracle: interpreted 68000 sound CPU (default)\n"
              << "                         native: statically compiled 68000 driver via f3_sound_blocks\n"
              << "  --audio-backend reference|enhanced (default reference; enhanced uses no sound CPU)\n"
              << "  --hle-events FILE      Save HLE voice start/release/stop/parameter CSV\n"
              << "  --json                 Output machine-readable JSON result\n"
              << "  --help, -h             Show this help message\n";
}

} // namespace

int cmd_sound_extract(int argc, char **argv) try {
    runner::RunnerConfig cfg;
#ifdef F3RT_DEFAULT_ROM_DIR
    cfg.rom_dir = F3RT_DEFAULT_ROM_DIR;
#endif

    std::vector<runner::ScheduledPacket> scheduled;
    double duration_seconds = 5.0;
    uint64_t boot_frames = 900;
    bool sound_explicit = false;
    bool json_output = false;

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
        } else if (arg == "--packet") {
            auto pkt = runner::parse_hex(value());
            scheduled.push_back(runner::ScheduledPacket{0.0, std::move(pkt), 0});
        } else if (arg == "--at") {
            scheduled.push_back(runner::parse_at_schedule(value()));
        } else if (arg == "--seconds") {
            const std::string text = value();
            size_t end = 0;
            duration_seconds = std::stod(text, &end);
            if (end != text.size() || duration_seconds <= 0.0 || std::isnan(duration_seconds) || std::isinf(duration_seconds)) {
                throw std::runtime_error("--seconds must be a positive number");
            }
        } else if (arg == "--boot-frames") {
            const std::string text = value();
            size_t end = 0;
            boot_frames = std::stoull(text, &end);
            if (end != text.size() || text[0] == '-' || boot_frames == 0) {
                throw std::runtime_error("--boot-frames must be positive");
            }
        } else if (arg == "--sound-trace") {
            cfg.sound_trace_path = value();
        } else if (arg == "--wav") {
            cfg.wav_path = value();
        } else if (arg == "--wav-window") {
            cfg.wav_window = value();
            if (cfg.wav_window != "full" && cfg.wav_window != "event") {
                throw std::runtime_error("--wav-window must be 'full' or 'event'");
            }
        } else if (arg == "--sound-driver") {
            sound_explicit = true;
            cfg.sound_driver = value();
            if (cfg.sound_driver != "oracle" && cfg.sound_driver != "native") {
                throw std::runtime_error("Invalid --sound-driver '" + cfg.sound_driver + "': must be 'oracle' or 'native'");
            }
        } else if (arg == "--audio-backend") {
            const std::string name = value();
            if (!Audio::parse_backend(name, cfg.audio_backend)) {
                throw std::runtime_error("--audio-backend must be enhanced or reference");
            }
        } else if (arg == "--hle-events") {
            cfg.hle_events_path = value();
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
        throw std::runtime_error("ROM directory must be specified via --rom-dir");
    }
    const bool enhanced = (cfg.audio_backend == Audio::Backend::Enhanced);
    if (enhanced && cfg.set != "landmakrj") {
        throw std::runtime_error("Enhanced audio requires landmakrj");
    }
    if (enhanced && (sound_explicit || !cfg.sound_trace_path.empty())) {
        throw std::runtime_error("Enhanced audio does not execute a sound driver or emit CPU bus traces");
    }
    if (!enhanced && !cfg.hle_events_path.empty()) {
        throw std::runtime_error("--hle-events requires --audio-backend enhanced");
    }

    std::stable_sort(scheduled.begin(), scheduled.end(), [](const auto &a, const auto &b) {
        return a.time_seconds < b.time_seconds;
    });

    if (duration_seconds > double(UINT64_MAX / Machine::main_clock)) {
        throw std::runtime_error("Duration exceeds the trace clock range");
    }
    const uint64_t duration_ticks =
        static_cast<uint64_t>(std::round(duration_seconds * double(Machine::main_clock)));
    if (!duration_ticks) throw std::runtime_error("Duration is shorter than one main clock tick");
    if (!scheduled.empty() && scheduled.back().tick >= duration_ticks) {
        throw std::runtime_error("Every scheduled packet must be earlier than --seconds after clock rounding");
    }

    const auto start_time = std::chrono::steady_clock::now();

    cfg.translated = false;
    cfg.allow_fallback = true;
    runner::MachineRunner runner(cfg);
    auto &m = runner.machine();

    const bool record_boot_wav = (cfg.wav_window == "full");

    for (uint64_t f = 0; f < boot_frames; ++f) {
        if (!m.run_frame(/*translated=*/false) || m.cpu.halted) {
            std::ostringstream error;
            error << "Main CPU halted during boot at frame " << m.frame << " PC 0x" << std::hex << m.cpu.pc;
            throw std::runtime_error(error.str());
        }
        runner.drain_audio(record_boot_wav);
    }

    if (m.audio->is_reset()) {
        throw std::runtime_error("Sound CPU is still held in reset; increase --boot-frames");
    }

    m.cpu.cycles = m.audio->clock_ticks();

    // Drain pending in-flight packets
    {
        uint32_t wait_cycles = 0;
        const uint32_t max_wait = 16000000;
        while (runner::RingBufferState::read_consumer(m) != runner::RingBufferState::read_producer(m) && wait_cycles < max_wait) {
            constexpr uint32_t step = 1000;
            m.audio->advance(step);
            m.cpu.cycles += step;
            wait_cycles += step;
            if (wait_cycles % 16000 == 0) runner.drain_audio(record_boot_wav);
        }
        runner.drain_audio(record_boot_wav);
        if (runner::RingBufferState::read_consumer(m) != runner::RingBufferState::read_producer(m)) {
            throw std::runtime_error("Sound CPU consumer did not synchronize with producer before freeze");
        }
    }

    const uint64_t event_origin_cycles = m.audio->clock_ticks();
    const double event_origin_seconds = double(event_origin_cycles) / double(Machine::main_clock);
    const uint64_t event_origin_sample = m.audio->generated_frames();

    if (!json_output) {
        std::cout << "Event time origin: " << std::fixed << std::setprecision(6) << event_origin_seconds
                  << " s (cycles=" << event_origin_cycles
                  << ", sample=" << event_origin_sample << ")\n";
    }

    uint64_t current_cycles = event_origin_cycles;
    if (duration_ticks > UINT64_MAX - event_origin_cycles) {
        throw std::runtime_error("Duration is outside the trace clock range");
    }
    const uint64_t target_end_cycles = event_origin_cycles + duration_ticks;

    size_t next_packet_idx = 0;
    size_t packets_published = 0;
    uint64_t cycles_since_last_drain = 0;
    constexpr uint64_t drain_interval = 16000;

    while (current_cycles < target_end_cycles) {
        while (next_packet_idx < scheduled.size()) {
            auto &sp = scheduled[next_packet_idx];
            uint64_t due_cycle = event_origin_cycles + sp.tick;
            if (current_cycles < due_cycle) break;
            if (runner::RingBufferState::available_bytes(m) < sp.packet.size()) {
                throw std::runtime_error("Mailbox full at scheduled tick; space commands farther apart");
            }
            runner::publish_packet(m, sp.packet, current_cycles);
            ++packets_published;
            ++next_packet_idx;
        }

        uint64_t remaining = target_end_cycles - current_cycles;
        uint64_t slice = std::min(uint64_t(1000), remaining);
        if (next_packet_idx < scheduled.size()) {
            const uint64_t due_cycle = event_origin_cycles + scheduled[next_packet_idx].tick;
            slice = std::min(slice, due_cycle - current_cycles);
        }
        slice = std::max(slice, uint64_t(1));

        m.audio->advance(static_cast<uint32_t>(slice));
        current_cycles += slice;
        m.cpu.cycles = current_cycles;
        cycles_since_last_drain += slice;

        if (cycles_since_last_drain >= drain_interval) {
            runner.drain_audio(true);
            cycles_since_last_drain = 0;
        }
    }

    runner.drain_audio(true);
    runner.finish();

    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    const double elapsed_seconds = std::chrono::duration<double>(elapsed).count();

    if (json_output) {
        std::cout << "{\"status\":\"SUCCESS\",\"backend\":\"" << Audio::backend_name(cfg.audio_backend)
                  << "\",\"driver\":\"" << (enhanced ? "none" : cfg.sound_driver)
                  << "\",\"set\":\"" << cfg.set
                  << "\",\"packets\":" << packets_published
                  << ",\"duration\":" << duration_seconds
                  << ",\"audio_frames\":" << runner.audio_frames()
                  << ",\"audio_peak\":" << runner.audio_peak()
                  << ",\"nonzero_samples\":" << runner.nonzero_samples()
                  << ",\"sound_pc\":\"0x" << std::hex << m.sound_pc() << std::dec << "\""
                  << ",\"wall_time\":" << elapsed_seconds
                  << ",\"event_origin_seconds\":" << event_origin_seconds
                  << ",\"event_origin_cycles\":" << event_origin_cycles
                  << "}\n";
    } else {
        std::cout << "SUCCESS backend=" << Audio::backend_name(cfg.audio_backend)
                  << " driver=" << (enhanced ? "none" : cfg.sound_driver)
                  << " set=" << cfg.set
                  << " packets=" << packets_published
                  << " duration=" << std::fixed << std::setprecision(3) << duration_seconds << "s"
                  << " audio_frames=" << runner.audio_frames()
                  << " audio_peak=" << runner.audio_peak()
                  << " nonzero_samples=" << runner.nonzero_samples()
                  << " sound_pc=0x" << std::hex << m.sound_pc() << std::dec
                  << " wall_time=" << std::fixed << std::setprecision(2) << elapsed_seconds << "s\n";
    }

    return 0;
} catch (const std::exception &e) {
    std::cerr << "SOUND_EXTRACT ERROR: " << e.what() << '\n';
    return 1;
}

} // namespace f3rt::tool
