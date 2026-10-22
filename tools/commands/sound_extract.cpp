#include "commands.hpp"
#include "../runner/runner.hpp"
#include "../runner/sound/capture.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

namespace f3rt::tool {

namespace {

void print_help(const char *prog) {
    std::cout
        << "Usage: " << prog << " sound-extract [options]\n\n"
        << "Standalone sound extraction and stimulus injection tool (Taito F3).\n\n"
        << "Two modes:\n"
        << "  * Sound codes (--sound / --sounds): run the game's own main-CPU sound request\n"
        << "    routine for a sound code from a clean post-boot snapshot, capture the mailbox\n"
        << "    packets it writes, render them and save one trimmed WAV per code.\n"
        << "  * Raw packets (--packet / --at): inject length-prefixed mailbox packets.\n\n"
        << "Options:\n"
        << "  --rom-dir DIR          Path to ROM directory\n"
        << "  --set SET              ROM set name (default: " F3RT_DEFAULT_SET ")\n"
        << "  --sound CODE           Extract one sound code (decimal, or 0x/$ hex). Writes --wav\n"
        << "                         FILE, or CODE.wav in --out-dir\n"
        << "  --sounds FIRST-LAST    Extract an inclusive code range into --out-dir as NNN.wav\n"
        << "                         plus index.csv and index.json (failures are listed, never\n"
        << "                         skipped silently)\n"
        << "  --bank N               Sound-test BANK value 1..31 (default 1). Switches the sound\n"
        << "                         program bank via main-bus 0x300000 (saves to bank_NN/ in --out-dir)\n"
        << "  --out-dir DIR          Output directory for --sound/--sounds (created if missing)\n"
        << "  --sound-routine ADDR   Main-CPU sound request routine (word code on the stack);\n"
        << "                         overrides the per-game default and disables the sound-test\n"
        << "                         prelude\n"
        << "  --no-prelude           Call only the request routine with CODE. By default the\n"
        << "                         game's SOUND TEST sequence is replayed: music codes first\n"
        << "                         request the two setup codes the test menu derives from CODE\n"
        << "                         (typically the effect preset and parameters)\n"
        << "  --seconds DURATION     Packet mode: audio duration (default 5.0). Sound-code mode:\n"
        << "                         maximum length per code, which caps looping music\n"
        << "                         (default 20.0)\n"
        << "  --silence-threshold N  Peak |sample| at or below N counts as silence (default 8)\n"
        << "  --tail SECONDS         Stop once this much silence follows the last audible\n"
        << "                         sample (default 1.0)\n"
        << "  --packet HEX           Inject length-prefixed packet at event origin (can be\n"
        << "                         repeated)\n"
        << "  --at SECONDS:HEX       Schedule packet injection at SECONDS from event origin (can\n"
        << "                         be repeated)\n"
        << "  --boot-frames N        Interpreted main CPU boot frames before freezing (default:\n"
        << "                         900, ~15.3s)\n"
        << "  --sound-trace FILE     Record lossless bus sound trace from cold boot (F3SND2\n"
        << "                         format; reference backend, packet mode)\n"
        << "  --wav FILE             Save generated stereo 16-bit PCM audio as WAV\n"
        << "  --wav-window full|event\n"
        << "                         full: record cold boot through extraction (default)\n"
        << "                         event: record only from event time origin onward\n"
        << "                         (packet mode; sound-code mode always records the event only)\n"
        << "  --audio-backend enhanced|reference\n"
        << "                         enhanced: HLE synth, no sound CPU (default)\n"
        << "                         reference: emulated 68000 sound CPU + ES5505\n"
        << "  --sound-driver oracle|native\n"
        << "                         reference backend only. oracle: interpreted 68000 sound CPU\n"
        << "                         (default), native: statically compiled 68000 driver\n"
#if F3RT_HLE_EVENTS
        << "  --hle-events FILE      Save HLE voice start/release/stop/parameter CSV\n"
#endif
        << "  --json                 Output machine-readable JSON result\n"
        << "  --help, -h             Show this help message\n";
}

using runner::CodeCall;
using runner::hex_bytes;
using runner::parse_number;
using runner::parse_range;

struct SoundResult {
    uint32_t code = 0;
    std::string status = "ok"; // ok | silent | no-packets | error
    std::string error;
    runner::SoundTestCapture test; // category, descriptor and the requests the sound test made
    double seconds = 0;
    int peak = 0;
    std::string file;
};

std::string json_escape(const std::string &s) {
    std::string out;
    for (char c : s) {
        if (c == '"' || c == '\\') out += '\\';
        if (c == '\n') out += "\\n";
        else out += c;
    }
    return out;
}

std::string calls_text(const std::vector<CodeCall> &calls) {
    std::string out;
    for (const auto &c : calls) {
        if (!out.empty()) out += ';';
        char buf[16];
        std::snprintf(buf, sizeof buf, "0x%x:", c.code);
        out += buf + hex_bytes(c.bytes);
    }
    return out;
}

} // namespace

int cmd_sound_extract(int argc, char **argv) try {
    runner::RunnerConfig cfg;
#ifdef F3RT_DEFAULT_ROM_DIR
    cfg.rom_dir = F3RT_DEFAULT_ROM_DIR;
#endif
    cfg.audio_backend = Audio::Backend::Enhanced;

    std::vector<runner::ScheduledPacket> scheduled;
    double duration_seconds = 0.0;
    uint64_t boot_frames = 900;
    bool sound_explicit = false;
    bool json_output = false;
    std::vector<uint32_t> codes;
    std::filesystem::path out_dir;
    std::optional<uint32_t> routine_override;
    std::optional<uint32_t> bank_override;
    bool no_prelude = false;
    int silence_threshold = 8;
    double tail_seconds = 1.0;

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
        } else if (arg == "--sound") {
            codes.push_back(parse_number(value()));
        } else if (arg == "--sounds") {
            const auto [first, last] = parse_range(value());
            if (last - first > 0xffff) throw std::runtime_error("--sounds range is too large");
            for (uint32_t c = first; c <= last; ++c) codes.push_back(c);
        } else if (arg == "--bank") {
            const uint32_t b = parse_number(value());
            if (b < 1 || b > 31) throw std::runtime_error("--bank must be between 1 and 31");
            bank_override = b;
        } else if (arg == "--out-dir") {
            out_dir = value();
        } else if (arg == "--sound-routine") {
            routine_override = parse_number(value());
        } else if (arg == "--no-prelude") {
            no_prelude = true;
        } else if (arg == "--silence-threshold") {
            silence_threshold = int(parse_number(value()));
        } else if (arg == "--tail") {
            const std::string text = value();
            size_t end = 0;
            tail_seconds = std::stod(text, &end);
            if (end != text.size() || tail_seconds < 0.0 || std::isnan(tail_seconds) ||
                std::isinf(tail_seconds)) {
                throw std::runtime_error("--tail must be a non-negative number");
            }
        } else if (arg == "--packet") {
            auto pkt = runner::parse_hex(value());
            scheduled.push_back(runner::ScheduledPacket{0.0, std::move(pkt), 0});
        } else if (arg == "--at") {
            scheduled.push_back(runner::parse_at_schedule(value()));
        } else if (arg == "--seconds") {
            const std::string text = value();
            size_t end = 0;
            duration_seconds = std::stod(text, &end);
            if (end != text.size() || duration_seconds <= 0.0 || std::isnan(duration_seconds) ||
                std::isinf(duration_seconds)) {
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
                throw std::runtime_error("Invalid --sound-driver '" + cfg.sound_driver +
                                         "': must be 'oracle' or 'native'");
            }
        } else if (arg == "--audio-backend") {
            const std::string name = value();
            if (!Audio::parse_backend(name, cfg.audio_backend)) {
                throw std::runtime_error("--audio-backend must be enhanced or reference");
            }
        } else if (arg == "--hle-events") {
#if F3RT_HLE_EVENTS
            cfg.hle_events_path = value();
#else
            throw std::runtime_error("--hle-events requires a build with F3RT_HLE_EVENTS=ON");
#endif
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
    if (enhanced && (sound_explicit || !cfg.sound_trace_path.empty())) {
        throw std::runtime_error(
            "Enhanced audio does not execute a sound driver or emit CPU bus traces");
    }
    if (!enhanced && !cfg.hle_events_path.empty()) {
        throw std::runtime_error("--hle-events requires --audio-backend enhanced");
    }

    const bool code_mode = !codes.empty();
    if (code_mode && !scheduled.empty())
        throw std::runtime_error("--sound/--sounds cannot be combined with --packet/--at");
    if (code_mode && codes.size() > 1 && out_dir.empty())
        throw std::runtime_error("--sounds requires --out-dir");
    if (code_mode && codes.size() > 1 && !cfg.hle_events_path.empty())
        throw std::runtime_error("--hle-events with --sounds would be overwritten per code; "
                                 "extract codes one at a time");
    if (code_mode && out_dir.empty() && cfg.wav_path.empty())
        throw std::runtime_error("--sound requires --wav FILE or --out-dir DIR");
    if (!code_mode && (!out_dir.empty() || routine_override || no_prelude))
        throw std::runtime_error("--out-dir/--sound-routine/--no-prelude need --sound or --sounds");
    if (duration_seconds == 0.0) duration_seconds = code_mode ? 20.0 : 5.0;

    std::stable_sort(scheduled.begin(), scheduled.end(),
                     [](const auto &a, const auto &b) { return a.time_seconds < b.time_seconds; });

    if (duration_seconds > double(UINT64_MAX / Machine::MAIN_CLOCK)) {
        throw std::runtime_error("Duration exceeds the trace clock range");
    }
    const uint64_t duration_ticks =
        static_cast<uint64_t>(std::round(duration_seconds * double(Machine::MAIN_CLOCK)));
    if (!duration_ticks) throw std::runtime_error("Duration is shorter than one main clock tick");
    if (!scheduled.empty() && scheduled.back().tick >= duration_ticks) {
        throw std::runtime_error(
            "Every scheduled packet must be earlier than --seconds after clock rounding");
    }

    const runner::SoundProfile *profile = nullptr;
    uint32_t request_routine = 0;
    if (code_mode) {
        profile = runner::find_sound_profile(cfg.set);
        if (routine_override) request_routine = *routine_override;
        else if (profile) request_routine = profile->request;
        else
            throw std::runtime_error("No sound request routine known for set '" + cfg.set +
                                     "'; pass --sound-routine ADDR");
    }

    const auto start_time = std::chrono::steady_clock::now();

    cfg.translated = false;
    cfg.allow_fallback = true;
    // Sound-code mode writes one WAV per code itself; the runner records no continuous WAV.
    const std::filesystem::path single_wav = cfg.wav_path;
    if (code_mode) cfg.wav_path.clear();

    const bool record_boot_wav = !code_mode && (cfg.wav_window == "full");

    // Interpreted boot to a frozen machine whose sound side has consumed every pending packet.
    auto runner = runner::boot_to_freeze(cfg, boot_frames, record_boot_wav);
    auto *m = &runner->machine();

    const uint64_t event_origin_cycles = m->audio->clock_ticks();
    const double event_origin_seconds = double(event_origin_cycles) / double(Machine::MAIN_CLOCK);
    const uint64_t event_origin_sample = m->audio->generated_frames();

    if (!json_output) {
        std::cout << "Event time origin: " << std::fixed << std::setprecision(6)
                  << event_origin_seconds << " s (cycles=" << event_origin_cycles
                  << ", sample=" << event_origin_sample << ")\n";
    }

    if (code_mode) {
        const uint32_t rate = m->audio->sample_rate();
        const runner::PostBootSnapshot snapshot = runner::take_post_boot_snapshot(*m);
        if (!json_output && enhanced)
            std::cout << "HLE commands before freeze: " << snapshot.boot_commands << '\n';
        // A reset pulse rewinds the HLE read pointer to ring byte 0, so the packets the game sent
        // during boot (effect preset/parameters, stop-all) are re-parsed together with the new
        // request and the HLE ends in exactly the post-boot state. That needs the ring to still
        // hold the whole boot stream; the check throws when it does not.
        if (enhanced) runner::boot_packet_stream(*m, snapshot.boot_commands);
        std::filesystem::path effective_out_dir = out_dir;
        if (bank_override && !out_dir.empty()) {
            char bdir[32];
            std::snprintf(bdir, sizeof(bdir), "bank_%02u", *bank_override);
            effective_out_dir = out_dir / bdir;
        }
        const std::filesystem::path wav_dir =
            effective_out_dir.empty() ? single_wav.parent_path() : effective_out_dir;
        if (!wav_dir.empty()) std::filesystem::create_directories(wav_dir);

        std::vector<SoundResult> results;
        results.reserve(codes.size());

        for (const uint32_t code : codes) {
            SoundResult res;
            res.code = code;
            std::vector<int16_t> pcm; // interleaved stereo
            try {
                // The HLE sequencer, synth and effects run on a worker thread outside the machine
                // snapshot, and a reused worker renders the same request differently (its absolute
                // clock and DSP history carry over). Every code therefore gets a new machine, hence
                // a new worker, restored from the post-boot snapshot; a worker that failed on an
                // earlier code is discarded the same way.
                runner::restore_post_boot(runner, cfg, snapshot);
                m = &runner->machine();

                if (bank_override) runner::apply_sound_bank(*m, profile, *bank_override);

                runner::capture_sound_test(*m, routine_override ? nullptr : profile,
                                           request_routine, code, no_prelude, res.test);

                bool any = false;
                for (const auto &c : res.test.calls) any = any || !c.bytes.empty();
                if (!any) res.status = "no-packets";

                const uint64_t origin = m->audio->clock_ticks();
                const uint64_t end_cycles = origin + duration_ticks;
                const size_t tail_frames = size_t(std::llround(tail_seconds * rate));
                uint64_t now = origin;
                size_t first_audible = SIZE_MAX, last_audible = 0;
                std::array<int16_t, 4096> block{};
                constexpr uint64_t SLICE = Machine::MAIN_CLOCK / 100; // 10 ms
                while (now < end_cycles) {
                    const uint64_t slice = std::min<uint64_t>(SLICE, end_cycles - now);
                    m->audio->advance(uint32_t(slice));
                    now += slice;
                    m->cpu.cycles = now;
                    while (const size_t n = m->audio->render(block.data(), block.size() / 2)) {
                        for (size_t i = 0; i < n; ++i) {
                            const int16_t l = block[i * 2], r = block[i * 2 + 1];
                            const int amp = std::max(std::abs(int(l)), std::abs(int(r)));
                            res.peak = std::max(res.peak, amp);
                            const size_t frame = pcm.size() / 2;
                            if (amp > silence_threshold) {
                                if (first_audible == SIZE_MAX) first_audible = frame;
                                last_audible = frame;
                            }
                            pcm.push_back(l);
                            pcm.push_back(r);
                        }
                    }
                    const size_t frames = pcm.size() / 2;
                    // Silence before the first audible sample is waited out up to the cap.
                    if (first_audible != SIZE_MAX && frames - last_audible >= tail_frames) break;
                }
                if (first_audible == SIZE_MAX) {
                    pcm.clear();
                    if (res.status == "ok") res.status = "silent";
                } else {
                    const size_t lead = size_t(0.02 * rate), tail = size_t(0.05 * rate);
                    const size_t begin = first_audible > lead ? first_audible - lead : 0;
                    const size_t end = std::min(last_audible + 1 + tail, pcm.size() / 2);
                    pcm = std::vector<int16_t>(pcm.begin() + ptrdiff_t(begin * 2),
                                               pcm.begin() + ptrdiff_t(end * 2));
                }
            } catch (const std::exception &e) {
                res.status = "error";
                res.error = e.what();
                pcm.clear();
            }
            res.seconds = double(pcm.size() / 2) / double(rate);
            if (!pcm.empty()) {
                std::filesystem::path path = single_wav;
                if (effective_out_dir.empty() == false && (codes.size() > 1 || single_wav.empty())) {
                    char name[32];
                    std::snprintf(name, sizeof name, "%03u.wav", code);
                    path = effective_out_dir / name;
                }
                WavWriter wav(path, rate);
                wav.append(pcm);
                res.file = path.filename().string();
            }
            if (!json_output)
                std::cout << "code " << code << " " << res.status << " category=" << res.test.category
                          << " type=" << res.test.descriptor_type << " seconds=" << std::fixed
                          << std::setprecision(3) << res.seconds << " peak=" << res.peak
                          << (res.error.empty() ? "" : "  ERROR: " + res.error) << '\n';
            results.push_back(std::move(res));
        }

        if (!effective_out_dir.empty()) {
            std::ofstream csv(effective_out_dir / "index.csv");
            csv << "code,status,category,descriptor_type,descriptor,seconds,peak,file,calls,error\n";
            for (const auto &r : results)
                csv << r.code << ',' << r.status << ',' << r.test.category << ',' << r.test.descriptor_type
                    << ',' << hex_bytes(r.test.descriptor) << ',' << std::fixed << std::setprecision(3)
                    << r.seconds << ',' << r.peak << ',' << r.file << ',' << calls_text(r.test.calls)
                    << ",\"" << json_escape(r.error) << "\"\n";
            std::ofstream js(effective_out_dir / "index.json");
            js << "[\n";
            for (size_t i = 0; i < results.size(); ++i) {
                const auto &r = results[i];
                js << " {\"code\":" << r.code << ",\"status\":\"" << r.status
                   << "\",\"category\":" << r.test.category << ",\"descriptor_type\":"
                   << r.test.descriptor_type << ",\"descriptor\":\"" << hex_bytes(r.test.descriptor)
                   << "\",\"seconds\":" << std::fixed << std::setprecision(3) << r.seconds
                   << ",\"peak\":" << r.peak << ",\"file\":\"" << r.file << "\",\"calls\":[";
                for (size_t c = 0; c < r.test.calls.size(); ++c)
                    js << (c ? "," : "") << "{\"code\":" << r.test.calls[c].code << ",\"bytes\":\""
                       << hex_bytes(r.test.calls[c].bytes) << "\"}";
                js << "],\"error\":\"" << json_escape(r.error) << "\"}" << (i + 1 < results.size() ? "," : "")
                   << '\n';
            }
            js << "]\n";
        }
        runner->finish();

        size_t ok = 0, silent = 0, none = 0, failed = 0;
        for (const auto &r : results) {
            ok += r.status == "ok";
            silent += r.status == "silent";
            none += r.status == "no-packets";
            failed += r.status == "error";
        }
        const double elapsed =
            std::chrono::duration<double>(std::chrono::steady_clock::now() - start_time).count();
        std::cout << (json_output ? "" : "SUMMARY ") << "backend="
                  << Audio::backend_name(cfg.audio_backend) << " codes=" << results.size()
                  << " ok=" << ok << " silent=" << silent << " no_packets=" << none
                  << " error=" << failed << " wall_time=" << std::fixed << std::setprecision(2)
                  << elapsed << "s\n";
        for (const auto &r : results)
            if (r.status != "ok")
                std::cout << "  code " << r.code << ": " << r.status
                          << (r.error.empty() ? "" : " (" + r.error + ")")
                          << " packets=" << calls_text(r.test.calls) << '\n';
        return failed ? 1 : 0;
    }

    uint64_t current_cycles = event_origin_cycles;
    if (duration_ticks > UINT64_MAX - event_origin_cycles) {
        throw std::runtime_error("Duration is outside the trace clock range");
    }
    const uint64_t target_end_cycles = event_origin_cycles + duration_ticks;

    size_t next_packet_idx = 0;
    size_t packets_published = 0;
    uint64_t cycles_since_last_drain = 0;
    constexpr uint64_t DRAIN_INTERVAL = 16000;

    while (current_cycles < target_end_cycles) {
        while (next_packet_idx < scheduled.size()) {
            auto &sp = scheduled[next_packet_idx];
            uint64_t due_cycle = event_origin_cycles + sp.tick;
            if (current_cycles < due_cycle) break;
            if (runner::RingBufferState::available_bytes(*m) < sp.packet.size()) {
                throw std::runtime_error(
                    "Mailbox full at scheduled tick; space commands farther apart");
            }
            runner::publish_packet(*m, sp.packet, current_cycles);
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

        m->audio->advance(static_cast<uint32_t>(slice));
        current_cycles += slice;
        m->cpu.cycles = current_cycles;
        cycles_since_last_drain += slice;

        if (cycles_since_last_drain >= DRAIN_INTERVAL) {
            runner->drain_audio(true);
            cycles_since_last_drain = 0;
        }
    }

    runner->drain_audio(true);
    runner->finish();

    const auto elapsed = std::chrono::steady_clock::now() - start_time;
    const double elapsed_seconds = std::chrono::duration<double>(elapsed).count();

    if (json_output) {
        std::cout << "{\"status\":\"SUCCESS\",\"backend\":\""
                  << Audio::backend_name(cfg.audio_backend) << "\",\"driver\":\""
                  << (enhanced ? "none" : cfg.sound_driver) << "\",\"set\":\"" << cfg.set
                  << "\",\"packets\":" << packets_published << ",\"duration\":" << duration_seconds
                  << ",\"audio_frames\":" << runner->audio_frames()
                  << ",\"audio_peak\":" << runner->audio_peak()
                  << ",\"nonzero_samples\":" << runner->nonzero_samples() << ",\"sound_pc\":\"0x"
                  << std::hex << m->sound_pc() << std::dec << "\""
                  << ",\"wall_time\":" << elapsed_seconds
                  << ",\"event_origin_seconds\":" << event_origin_seconds
                  << ",\"event_origin_cycles\":" << event_origin_cycles << "}\n";
    } else {
        std::cout << "SUCCESS backend=" << Audio::backend_name(cfg.audio_backend)
                  << " driver=" << (enhanced ? "none" : cfg.sound_driver) << " set=" << cfg.set
                  << " packets=" << packets_published << " duration=" << std::fixed
                  << std::setprecision(3) << duration_seconds << "s"
                  << " audio_frames=" << runner->audio_frames()
                  << " audio_peak=" << runner->audio_peak()
                  << " nonzero_samples=" << runner->nonzero_samples() << " sound_pc=0x" << std::hex
                  << m->sound_pc() << std::dec << " wall_time=" << std::fixed << std::setprecision(2)
                  << elapsed_seconds << "s\n";
    }

    return 0;
} catch (const std::exception &e) {
    std::cerr << "SOUND_EXTRACT ERROR: " << e.what() << '\n';
    return 1;
}

} // namespace f3rt::tool
