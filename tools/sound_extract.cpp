#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/rom.hpp"
#include "capture_io.hpp"
#include "sound_trace.hpp"

#ifdef F3RT_SOUND_GENERATED
#include "sound_program.h"
#endif

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace {

struct ScheduledPacket {
    double time_seconds = 0.0;
    std::vector<uint8_t> packet;
    uint64_t tick = 0;
};

// Parses a hex string into byte vector and validates length prefix:
// Land Maker driver packets are length-prefixed where byte 0 equals the total
// packet size (including the length byte and opcode). Minimum size is 2 bytes.
std::vector<uint8_t> parse_hex(std::string_view hex_str) {
    std::string clean;
    clean.reserve(hex_str.size());
    for (char c : hex_str) {
        if (std::isspace(static_cast<unsigned char>(c))) continue;
        clean.push_back(c);
    }
    if (clean.empty()) {
        throw std::runtime_error("Empty packet hex string");
    }
    if (clean.size() % 2 != 0) {
        throw std::runtime_error("Packet hex string length must be even: " + std::string(hex_str));
    }

    std::vector<uint8_t> bytes;
    bytes.reserve(clean.size() / 2);
    auto hex_val = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < clean.size(); i += 2) {
        int hi = hex_val(clean[i]);
        int lo = hex_val(clean[i + 1]);
        if (hi < 0 || lo < 0) {
            throw std::runtime_error("Invalid hex character in packet: " + std::string(hex_str));
        }
        bytes.push_back(static_cast<uint8_t>((hi << 4) | lo));
    }

    if (bytes.size() < 2) {
        throw std::runtime_error("Packet too short (minimum 2 bytes: length prefix and opcode): " + std::string(hex_str));
    }
    if (bytes[0] != bytes.size()) {
        std::ostringstream ss;
        ss << "Packet length prefix mismatch in '" << hex_str << "': byte 0 is " << int(bytes[0])
           << " but packet byte count is " << bytes.size();
        throw std::runtime_error(ss.str());
    }
    return bytes;
}

ScheduledPacket parse_at_schedule(std::string_view spec) {
    auto colon = spec.find(':');
    if (colon == std::string_view::npos) {
        throw std::runtime_error("Malformed --at specification (expected SECONDS:HEX): " + std::string(spec));
    }
    std::string sec_str(spec.substr(0, colon));
    std::string hex_str(spec.substr(colon + 1));
    double sec = 0.0;
    try {
        size_t idx = 0;
        sec = std::stod(sec_str, &idx);
        if (idx != sec_str.size()) throw std::invalid_argument("trailing characters");
    } catch (const std::exception &e) {
        throw std::runtime_error("Invalid seconds in --at schedule '" + std::string(spec) + "': " + e.what());
    }
    if (sec < 0.0 || std::isnan(sec) || std::isinf(sec)) {
        throw std::runtime_error("Scheduled time in --at must be non-negative: " + std::string(spec));
    }
    auto packet = parse_hex(hex_str);
    if (sec > double(UINT64_MAX / f3rt::Machine::main_clock))
        throw std::runtime_error("Scheduled time exceeds the trace clock range");
    return ScheduledPacket{sec, std::move(packet), uint64_t(std::round(sec * f3rt::Machine::main_clock))};
}

struct RingBufferState {
    static constexpr size_t RING_SIZE = 1024; // 0x400 bytes in main shared memory (0xc00000..0xc003ff)

    // The producer and consumer words in shared memory are doubled (twice the byte offset).
    // Producer is at main 0xc00480..0xc00481 (shared[0x480..0x481]);
    // Consumer is at main 0xc00482..0xc00483 (shared[0x482..0x483]).
    static uint16_t read_producer(const f3rt::Machine &m) {
        return (uint16_t(m.shared[0x480]) << 8) | m.shared[0x481];
    }
    static uint16_t read_consumer(const f3rt::Machine &m) {
        return (uint16_t(m.shared[0x482]) << 8) | m.shared[0x483];
    }

    // Available byte count in the ring buffer, reserving 1 byte so full != empty
    static size_t available_bytes(const f3rt::Machine &m) {
        uint16_t prod_d = read_producer(m);
        uint16_t cons_d = read_consumer(m);
        uint16_t occupied_doubled = (prod_d - cons_d) & 0x7fe;
        size_t occupied = occupied_doubled / 2;
        if (occupied >= RING_SIZE - 1) return 0;
        return (RING_SIZE - 1) - occupied;
    }
};

void publish_packet(f3rt::Machine &m, const std::vector<uint8_t> &pkt, uint64_t current_cycles) {
    uint16_t prod_d = RingBufferState::read_producer(m);
    size_t prod_byte = (prod_d / 2) & 0x3ff;

    // Synthetic writer PC explicit 0xffffffff in trace, synchronized with audio time
    m.cpu.pc = 0xffffffff;
    m.cpu.cycles = current_cycles;

    for (size_t i = 0; i < pkt.size(); ++i) {
        uint32_t ring_addr = 0xc00000 + ((prod_byte + i) & 0x3ff);
        m.write8(ring_addr, pkt[i]);
    }

    size_t next_prod_byte = (prod_byte + pkt.size()) & 0x3ff;
    uint16_t new_prod_d = uint16_t((next_prod_byte * 2) & 0x7fe);

    // Doubled producer word written big-endian to c00480 (high) then c00481 (low)
    m.write8(0xc00480, uint8_t(new_prod_d >> 8));
    m.write8(0xc00481, uint8_t(new_prod_d & 0xff));
}

void print_help(const char *prog) {
    std::cout << "Usage: " << prog << " [options]\n\n"
              << "Standalone sound extraction and stimulus injection tool for Land Maker (Taito F3).\n\n"
              << "Options:\n"
              << "  --rom-dir DIR          Path to ROM directory (default: F3RT_DEFAULT_ROM_DIR if defined)\n"
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
              << "  --help, -h             Show this help message\n\n"
              << "Driver Startup Behavior and Initialization:\n"
              << "  During boot, the interpreted main CPU runs through the sound reset handshake.\n"
              << "  Default boot is 900 frames, after the game's output-gain writes at ~13.23s.\n"
              << "  No hidden initialization commands are synthesized. Earlier boot points can retain\n"
              << "  startup attenuation or need sequence-start packets. Sound reset must be released.\n"
              << "  Packets must be length-prefixed hex strings matching the sound driver protocol, where\n"
              << "  byte 0 is the total packet size (e.g. '038001' has size 3, opcode 0x80, param 0x01).\n";
}

} // namespace

int main(int argc, char **argv) try {
    std::filesystem::path romdir;
#ifdef F3RT_DEFAULT_ROM_DIR
    romdir = F3RT_DEFAULT_ROM_DIR;
#endif

    std::string set = "landmakrj";
    std::string sound_driver = "oracle";
    std::filesystem::path sound_trace_path;
    std::filesystem::path wav_path;
    std::string wav_window = "full";

    std::vector<ScheduledPacket> scheduled;

    double duration_seconds = 5.0;

    uint64_t boot_frames = 900; // Includes the main game's output-gain initialization.

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
        } else if (arg == "--packet") {
            auto pkt = parse_hex(value());
            scheduled.push_back(ScheduledPacket{0.0, std::move(pkt), 0});
        } else if (arg == "--at") {
            scheduled.push_back(parse_at_schedule(value()));
        } else if (arg == "--seconds") {
            const std::string text = value();
            size_t end = 0;
            duration_seconds = std::stod(text, &end);
            if (end != text.size()) throw std::runtime_error("Invalid --seconds value");
            if (duration_seconds <= 0.0 || std::isnan(duration_seconds) || std::isinf(duration_seconds)) {
                throw std::runtime_error("--seconds must be a positive number");
            }
        } else if (arg == "--boot-frames") {
            const std::string text = value();
            size_t end = 0;
            boot_frames = std::stoull(text, &end);
            if (end != text.size() || text[0] == '-') throw std::runtime_error("Invalid --boot-frames value");
            if (boot_frames == 0) {
                throw std::runtime_error("--boot-frames must be positive");
            }
        } else if (arg == "--sound-trace") {
            sound_trace_path = value();
        } else if (arg == "--wav") {
            wav_path = value();
        } else if (arg == "--wav-window") {
            wav_window = value();
            if (wav_window != "full" && wav_window != "event") {
                throw std::runtime_error("--wav-window must be 'full' or 'event'");
            }
        } else if (arg == "--sound-driver") {
            sound_driver = value();
            if (sound_driver != "oracle" && sound_driver != "native") {
                throw std::runtime_error("Invalid --sound-driver '" + sound_driver + "': must be 'oracle' or 'native'");
            }
        } else if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            return 0;
        } else {
            throw std::runtime_error("Unknown argument: " + arg + " (use --help for usage)");
        }
    }

    if (romdir.empty()) {
        throw std::runtime_error("ROM directory must be specified via --rom-dir or compiled F3RT_DEFAULT_ROM_DIR");
    }

    // Sort scheduled packets stably by scheduled timestamp
    std::stable_sort(scheduled.begin(), scheduled.end(), [](const ScheduledPacket &a, const ScheduledPacket &b) {
        return a.time_seconds < b.time_seconds;
    });
    if (duration_seconds > double(UINT64_MAX / f3rt::Machine::main_clock))
        throw std::runtime_error("Duration exceeds the trace clock range");
    const uint64_t duration_ticks =
        static_cast<uint64_t>(std::round(duration_seconds * double(f3rt::Machine::main_clock)));
    if (!duration_ticks) throw std::runtime_error("Duration is shorter than one main clock tick");
    if (!scheduled.empty() && scheduled.back().tick >= duration_ticks)
        throw std::runtime_error("Every scheduled packet must be earlier than --seconds after clock rounding");


    const auto start_time = std::chrono::steady_clock::now();

    // 1. Initialize Machine with loaded ROM set
    auto machine = std::make_unique<f3rt::Machine>(f3rt::RomSet::load(romdir, set));
    auto &m = *machine;

    // Attach native sound driver before audio clock advances if requested
    if (sound_driver == "native") {
#ifdef F3RT_SOUND_GENERATED
        m.use_native_sound(f3_sound_blocks, f3_sound_block_count);
#else
        throw std::runtime_error("Native sound driver (--sound-driver native) requested, but binary was built without F3RT_SOUND_GENERATED");
#endif
    }

    // Lossless sound trace captures cold boot through end of extraction
    if (!sound_trace_path.empty()) {
        m.sound_trace = std::make_unique<f3rt::SoundTrace>(sound_trace_path);
    }

    std::unique_ptr<f3rt::WavWriter> wav;
    if (!wav_path.empty()) {
        wav = std::make_unique<f3rt::WavWriter>(wav_path, m.audio->sample_rate());
    }

    std::array<int16_t, 8192> audio_buffer{};
    uint64_t total_audio_frames = 0;
    uint64_t nonzero_samples = 0;
    int audio_peak = 0;

    auto drain_audio = [&](bool record_to_wav) {
        size_t count = 0;
        while ((count = m.audio->render(audio_buffer.data(), audio_buffer.size() / 2)) != 0) {
            total_audio_frames += count;
            if (wav && record_to_wav) {
                wav->append(std::span(audio_buffer.data(), count * 2));
            }
            for (size_t i = 0; i < count * 2; ++i) {
                audio_peak = std::max(audio_peak, std::abs(int(audio_buffer[i])));
                nonzero_samples += (audio_buffer[i] != 0);
            }
        }
    };

    const bool record_boot_wav = (wav_window == "full");

    // 2. Main boot phase: step interpreted main CPU safely to stable initialized sound state
    for (uint64_t f = 0; f < boot_frames; ++f) {
        if (!m.run_frame(/*translated=*/false) || m.cpu.halted) {
            std::ostringstream error;
            error << "Main CPU halted during boot at frame " << m.frame << " PC 0x" << std::hex << m.cpu.pc;
            throw std::runtime_error(error.str());
        }
        drain_audio(record_boot_wav);
    }

    if (m.audio->is_reset())
        throw std::runtime_error("Sound CPU is still held in reset; increase --boot-frames");
    // The final main instruction may have crossed the frame boundary. Main
    // execution is now frozen; use the actual device cursor for all injections.
    m.cpu.cycles = m.audio->clock_ticks();

    // Drain any pending in-flight packets so consumer catches up to producer
    {
        uint32_t wait_cycles = 0;
        const uint32_t max_wait = 16000000; // up to 1 second main cycles
        while (RingBufferState::read_consumer(m) != RingBufferState::read_producer(m) && wait_cycles < max_wait) {
            constexpr uint32_t step = 1000;
            m.audio->advance(step);
            m.cpu.cycles += step;
            wait_cycles += step;
            if (wait_cycles % 16000 == 0) drain_audio(record_boot_wav);
        }
        drain_audio(record_boot_wav);
        if (RingBufferState::read_consumer(m) != RingBufferState::read_producer(m)) {
            throw std::runtime_error("Sound CPU consumer did not synchronize with producer before freeze");
        }
    }

    // 3. Main CPU is now frozen. Establish and print event time origin
    const uint64_t event_origin_cycles = m.audio->clock_ticks();
    const double event_origin_seconds = double(event_origin_cycles) / double(f3rt::Machine::main_clock);
    const uint64_t event_origin_sample = m.audio->generated_frames();

    std::cout << "Event time origin: " << std::fixed << std::setprecision(6) << event_origin_seconds
              << " s (cycles=" << event_origin_cycles
              << ", sample=" << event_origin_sample << ")\n";

    // 4. Advance audio directly without watchdog resets; inject complete valid packets
    uint64_t current_cycles = event_origin_cycles;
    if (duration_ticks > UINT64_MAX - event_origin_cycles)
        throw std::runtime_error("Duration is outside the trace clock range");
    const uint64_t target_end_cycles = event_origin_cycles + duration_ticks;

    size_t next_packet_idx = 0;
    size_t packets_published = 0;
    uint64_t cycles_since_last_drain = 0;
    constexpr uint64_t drain_interval = 16000; // ~1ms, ~30 audio samples: guarantees no 32k overflow

    while (current_cycles < target_end_cycles) {
        // Try to inject due scheduled packets
        while (next_packet_idx < scheduled.size()) {
            auto &sp = scheduled[next_packet_idx];
            uint64_t due_cycle = event_origin_cycles + sp.tick;
            if (current_cycles < due_cycle) {
                break; // Not due yet
            }
            if (RingBufferState::available_bytes(m) < sp.packet.size())
                throw std::runtime_error("Mailbox full at scheduled tick; space commands farther apart");
            publish_packet(m, sp.packet, current_cycles);
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

        // Advance Audio directly (synchronizing cpu.cycles and Audio time)
        m.audio->advance(static_cast<uint32_t>(slice));
        current_cycles += slice;
        m.cpu.cycles = current_cycles;
        cycles_since_last_drain += slice;

        if (cycles_since_last_drain >= drain_interval) {
            drain_audio(true);
            cycles_since_last_drain = 0;
        }
    }


    // Final drain and flush End record
    drain_audio(true);
    if (m.sound_trace) {
        m.sound_trace->finish(m);
    }

    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    const double elapsed_sec = std::chrono::duration<double>(elapsed).count();

    std::cout << "SUCCESS driver=" << sound_driver
              << " set=" << set
              << " packets=" << packets_published
              << " duration=" << std::fixed << std::setprecision(3) << duration_seconds << "s"
              << " audio_frames=" << total_audio_frames
              << " audio_peak=" << audio_peak
              << " nonzero_samples=" << nonzero_samples
              << " sound_pc=0x" << std::hex << m.sound_pc() << std::dec
              << " wall_time=" << std::fixed << std::setprecision(2) << elapsed_sec << "s\n";

    return 0;
} catch (const std::exception &e) {
    std::cerr << "SOUND_EXTRACT ERROR: " << e.what() << '\n';
    return 1;
}
