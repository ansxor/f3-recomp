#include "commands.hpp"
#include "../runner/runner.hpp"
#include "debug/input_script.hpp"
#include "f3rt/game_netplay.hpp"
#include "f3rt/input.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/netplay.hpp"
#include "f3rt/rom.hpp"
#include "generated_config/build_identity.hpp"
#include "netplay/recorder.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>

namespace f3rt::tool {

using namespace f3rt;
using namespace f3rt::netplay;

namespace {

void print_help(const char *) {
    std::cout << "Usage: f3rt-tool netplay [options]\n\n"
              << "Deterministic 2-player lockstep netplay client.\n\n"
              << "Options:\n"
              << "  --server HOST:PORT     Server address (required, default port 7845)\n"
              << "  --host                 Host a new room\n"
              << "  --delay N              Input delay in frames (1-8, default: 2)\n"
              << "  --join CODE            Join an existing room code\n"
              << "  --name NAME            Player name (default: player1/player2)\n"
              << "  --inputs SCRIPT        InputScript file (pre-trigger or solo word)\n"
              << "  --match-inputs SCRIPT  InputScript file for match frames (with --until-trigger)\n"
              << "  --until-trigger        Run --inputs locally until 2P versus edge fires, then netplay\n"
              << "  --frames N             Number of match frames to run (default: 3000)\n"
              << "  --crc-every N          Report state CRC every N frames (default: 60)\n"
              << "  --rom-dir DIR          ROM directory (defaults to F3RT_DEFAULT_ROM_DIR)\n"
              << "  --set SET              ROM set name (default: landmakrj)\n"
              << "  --disable-apply-sync   Joiner negative control: do not apply sync blob\n"
              << "  --gpu 0|1              Build the machine with GPU game video, like the frontend (default 0)\n"
              << "  --audio NAME           Audio backend: enhanced | reference (default reference)\n"
              << "  --desync-dir DIR       On DESYNC write a local diagnostic bundle into DIR/<time>-<session>-p<idx>/\n"
              << "                         (info.json, hashed.bin, inputs.txt, trigger.f3s). Nothing is sent over the network.\n"
              << "  --help, -h             Show this help message\n";
}

} // namespace

int cmd_netplay(int argc, char **argv) try {
    std::string server_arg;
    bool is_host = false;
    bool host_specified = false;
    std::string join_code;
    uint8_t delay = 2;
    std::string player_name;
    std::string inputs_path;
    std::string match_inputs_path;
    bool until_trigger = false;
    bool disable_apply_sync = false;
    bool use_gpu = false;
    std::string audio_backend;
    uint32_t target_frames = 3000;
    uint32_t crc_every = 60;
    std::filesystem::path desync_dir;
    std::filesystem::path rom_dir;
    std::string set = "landmakrj";

#ifdef F3RT_DEFAULT_ROM_DIR
    rom_dir = F3RT_DEFAULT_ROM_DIR;
#endif
#ifdef F3RT_DEFAULT_SET
    set = F3RT_DEFAULT_SET;
#endif

    for (int i = 0; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> const char * {
            if (i + 1 >= argc) throw std::runtime_error("Missing value for argument: " + arg);
            return argv[++i];
        };

        if (arg == "--server") {
            server_arg = value();
        } else if (arg == "--host") {
            is_host = true;
            host_specified = true;
        } else if (arg == "--delay") {
            delay = static_cast<uint8_t>(std::stoul(value()));
            if (delay < 1 || delay > 8) {
                throw std::runtime_error("--delay must be between 1 and 8");
            }
        } else if (arg == "--join") {
            join_code = value();
            is_host = false;
            host_specified = true;
        } else if (arg == "--name") {
            player_name = value();
        } else if (arg == "--inputs") {
            inputs_path = value();
        } else if (arg == "--match-inputs") {
            match_inputs_path = value();
        } else if (arg == "--until-trigger") {
            until_trigger = true;
        } else if (arg == "--disable-apply-sync") {
            disable_apply_sync = true;
        } else if (arg == "--desync-dir") {
            desync_dir = value();
        } else if (arg == "--gpu") {
            use_gpu = value() == std::string("1");
        } else if (arg == "--audio") {
            audio_backend = value();
        } else if (arg == "--frames") {
            target_frames = static_cast<uint32_t>(std::stoul(value()));
        } else if (arg == "--crc-every") {
            crc_every = static_cast<uint32_t>(std::stoul(value()));
            if (crc_every == 0) throw std::runtime_error("--crc-every must be > 0");
        } else if (arg == "--rom-dir") {
            rom_dir = value();
        } else if (arg == "--set") {
            set = value();
        } else if (arg == "--help" || arg == "-h") {
            print_help(argv[0]);
            return 0;
        } else {
            throw std::runtime_error("Unknown argument: " + arg);
        }
    }

    if (server_arg.empty()) {
        throw std::runtime_error("--server HOST:PORT is required");
    }
    if (!host_specified) {
        throw std::runtime_error("Either --host or --join CODE must be specified");
    }
    if (rom_dir.empty()) {
        throw std::runtime_error("ROM directory must be specified via --rom-dir or F3RT_DEFAULT_ROM_DIR");
    }

    if (player_name.empty()) {
        player_name = is_host ? "player1" : "player2";
    }

    // Parse host and port
    std::string host = server_arg;
    uint16_t port = netplay::default_port;
    auto colon = server_arg.rfind(':');
    if (colon != std::string::npos) {
        host = server_arg.substr(0, colon);
        port = static_cast<uint16_t>(std::stoul(server_arg.substr(colon + 1)));
    }

    // Load inputs scripts (reject scripts with RAM pokes)
    std::optional<InputScript> input_script;
    if (!inputs_path.empty()) {
        auto loaded = InputScript::load(inputs_path);
        if (loaded.has_pokes()) {
            throw std::runtime_error("Netplay input script contains memory poke rules: " + inputs_path);
        }
        input_script = std::move(loaded);
    }

    std::optional<InputScript> match_script;
    if (!match_inputs_path.empty()) {
        auto loaded = InputScript::load(match_inputs_path);
        if (loaded.has_pokes()) {
            throw std::runtime_error("Match input script contains memory poke rules: " + match_inputs_path);
        }
        match_script = std::move(loaded);
    } else if (!until_trigger) {
        match_script = input_script;
    }

    // Boot machine strictly deterministically
    auto machine = runner::create_strict_machine(rom_dir, set, use_gpu, 1);
    if (use_gpu && !machine->game_video) {
        throw std::runtime_error("--gpu 1 needs an f3rt-tool built with F3RT_GAME_VIDEO");
    }
    if (!audio_backend.empty()) {
        Audio::Backend backend;
        if (!Audio::parse_backend(audio_backend, backend)) throw std::runtime_error("--audio must be enhanced or reference");
        machine->audio->set_backend(backend);
    }
    const auto *netplay_hook = f3rt::game_netplay();

    // If --until-trigger, run locally until 2P versus edge fires
    if (until_trigger) {
        if (!netplay_hook || !netplay_hook->in_versus) {
            throw std::runtime_error("Game does not provide an in_versus netplay trigger");
        }
        bool prev_in_vs = false;
        bool triggered = false;
        while (true) {
            LocalInputWord p1 = 0, p2 = 0;
            if (input_script) {
                auto words = input_script->words(machine->frame + 1);
                p1 = words[0];
                p2 = words[1];
            }
            std::array<LocalInputWord, local_player_count> local_words{p1, p2, 0, 0};
            apply_local_inputs(*machine, local_words);
            if (!machine->run_frame(true)) {
                throw std::runtime_error("Emulation failure while waiting for versus trigger");
            }
            machine->audio->finish_frame();

            bool in_vs = netplay_hook->in_versus(*machine);
            if (!prev_in_vs && in_vs) {
                std::cout << "TRIGGER frame=" << machine->frame;
                if (netplay_hook && netplay_hook->sync_hash) {
                    std::cout << " sync_hash=0x" << std::hex << netplay_hook->sync_hash(*machine) << std::dec;
                }
                std::cout << "\n";
                std::cout.flush();
                triggered = true;
                break;
            }
            prev_in_vs = in_vs;
        }
        if (!triggered) {
            throw std::runtime_error("Versus trigger never fired");
        }
    }
    // Compute ROM CRCs
    auto rom_crcs = rom_region_crcs(machine->roms);

    // Setup netplay session configuration
    netplay::SessionConfig cfg;
    cfg.server_host = host;
    cfg.server_port = port;
    cfg.game_id = std::string(netplay::default_game_id);
    cfg.rom_crcs = rom_crcs;
    cfg.build_hash = netplay::build_hash;
    cfg.player_name = player_name;
    cfg.is_host = is_host;
    cfg.input_delay = delay;
    cfg.room_code = join_code;
    cfg.crc_interval = crc_every;

    // Machine::frame when capture_sync / apply_sync ran. The machine must not run a frame between that and match frame 0.
    uint64_t sync_frame = UINT64_MAX;
    if (is_host) {
        if (netplay_hook && netplay_hook->capture_sync) {
            cfg.capture_sync = [&]() {
                std::vector<uint8_t> buf(2048);
                size_t n = netplay_hook->capture_sync(*machine, buf);
                sync_frame = machine->frame;
                buf.resize(n);
                return buf;
            };
        }
    } else {
        if (netplay_hook && netplay_hook->apply_sync && !disable_apply_sync) {
            cfg.apply_sync = [&](std::span<const uint8_t> blob, uint64_t seed) {
                netplay_hook->apply_sync(*machine, blob, seed);
                sync_frame = machine->frame;
            };
        }
    }

    if (netplay_hook && netplay_hook->sync_hash) {
        cfg.sync_hash = [&]() -> uint32_t {
            return netplay_hook->sync_hash(*machine);
        };
    }

    netplay::Session session(cfg);
    session.start();

    // Wait until match starts
    bool room_code_printed = false;
    while (session.state() != netplay::SessionState::Running &&
           session.state() != netplay::SessionState::Ended) {
        session.poll();

        if (is_host && !room_code_printed && !session.room_code().empty()) {
            std::cout << "ROOM " << session.room_code() << "\n";
            std::cout.flush();
            room_code_printed = true;
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }

    if (session.state() == netplay::SessionState::Ended) {
        std::cerr << "Netplay session ended before match start: "
                  << session.last_error() << "\n";
        return 1;
    }

    // Explicit invariant: frames_between_sync_and_start == 0 (this tool runs no frames while waiting for GO).
    const uint64_t frames_between = (sync_frame == UINT64_MAX) ? 0 : machine->frame - sync_frame;
    std::cout << "NETPLAY_INVARIANT frames_between_sync_and_start=" << frames_between << "\n";
    if (frames_between != 0) {
        std::cerr << "NETPLAY_INVARIANT_VIOLATION: the machine ran " << frames_between << " frames between sync and start\n";
        return 1;
    }

    // Run lockstepped match frames
    std::unique_ptr<netplay::MatchRecorder> recorder;
    if (!desync_dir.empty()) {
        recorder = std::make_unique<netplay::MatchRecorder>(netplay_hook);
        recorder->meta.trigger_machine_frame = recorder->meta.edge_machine_frame =
            recorder->meta.sync_machine_frame = (sync_frame == UINT64_MAX) ? machine->frame : sync_frame;
        recorder->begin_match(*machine, session.seed());
    }
    uint32_t f = 0;
    uint32_t next_local_frame = 0;
    while (f < target_frames && session.state() == netplay::SessionState::Running) {
        session.poll();

        // Submit local input for frame f exactly once (keeping up with f)
        if (next_local_frame <= f) {
            LocalInputWord local_word = 0;
            if (match_script) {
                auto words = match_script->words(next_local_frame + 1);
                local_word = words[0];
            }
            session.submit_local(next_local_frame, local_word);
            ++next_local_frame;
        }

        if (!session.ready_to_step(f)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }

        // Shared lockstep step function (on_match_frame, apply_local_inputs, run_frame, report_crc)
        if (!step_lockstep_frame(*machine, session, f, netplay_hook, crc_every, recorder.get())) {
            std::cerr << "Emulation failure at match frame " << f << "\n";
            break;
        }

        ++f;
    }

    uint32_t final_crc = machine->state_crc();
    uint32_t final_sync = (netplay_hook && netplay_hook->sync_hash) ? netplay_hook->sync_hash(*machine) : 0;

    if (session.state() == netplay::SessionState::Running) {
        session.send_result(f, 0xFF, (netplay_hook && netplay_hook->sync_hash) ? final_sync : final_crc);

        auto end_wait = std::chrono::steady_clock::now();
        while (std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now() - end_wait).count() < 1000) {
            session.poll();
            if (session.state() == netplay::SessionState::Ended) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        session.send_bye();
        session.poll();
    }

    std::cout << "FINAL frame=" << f << " sync_hash=0x"
              << std::hex << std::setw(8) << std::setfill('0') << final_sync
              << " state_crc=0x"
              << std::setw(8) << std::setfill('0') << final_crc << std::dec << "\n";
    std::cout << "END reason=" << int(session.end_reason()) << " state=" << int(session.state()) << " error=" << session.last_error() << "\n";
    const auto &desync = session.desync_info();
    if (desync.detected) {
        std::cout << "DESYNC frame=" << desync.frame << " p1=0x"
                  << std::hex << std::setw(8) << std::setfill('0') << desync.crc_p1
                  << " p2=0x" << std::setw(8) << std::setfill('0') << desync.crc_p2
                  << std::dec << "\n";
    }
    if (desync.detected && recorder) {
        netplay::SessionFacts facts;
        facts.session_id = session.session_id();
        facts.seed = session.seed();
        facts.player_index = session.player_index();
        facts.input_delay = session.input_delay();
        facts.crc_interval = crc_every;
        facts.own_name = player_name;
        facts.peer_name = session.peer_name();
        facts.room_code = session.room_code();
        facts.build_hash = netplay::build_hash;
        facts.desync_frame = desync.frame;
        facts.desync_crc_p1 = desync.crc_p1;
        facts.desync_crc_p2 = desync.crc_p2;
        recorder->meta.stall_count = session.stats().stall_count;
        try {
            std::cout << "DESYNC_BUNDLE " << recorder->write_bundle(desync_dir, facts).string() << "\n";
        } catch (const std::exception &e) {
            std::cerr << "Could not write desync bundle: " << e.what() << "\n";
        }
    }
    std::cout.flush();

    return (f == target_frames && !desync.detected) ? 0 : 1;
} catch (const std::exception &e) {
    std::cerr << "Error: " << e.what() << "\n";
    return 1;
}

} // namespace f3rt::tool
