#pragma once
// Local-only desync diagnostics. Nothing in here touches the network.
//
// A MatchRecorder keeps, per match:
//   - the machine state at match frame 0 (once, in memory),
//   - every applied input word pair, one entry per match frame,
//   - a small ring of the last few CRC reports, each with the exact bytes the hash covered.
// The state at the mismatch frame is not kept: `f3rt-tool netplay-replay --dump-state` regenerates it from trigger.f3s
// and inputs.txt.
// On DESYNC, write_bundle() writes `<settings>/netplay-desync/<UTC>-<session_id>-p<player>/` containing:
//   info.json    mismatch frame, local/remote CRCs, seed, input delay, hash phase, player and names,
//                build hash, room code, stall/clock-resync counters, regeneration note, report list
//   hashed.bin   hashed bytes of the report at hashed_frame, then those of the previous report
//   inputs.txt   every applied input word pair (both ports) plus the trigger/sync/start frame numbers
//   trigger.f3s  this peer's machine state at match frame 0
// The ring holds the last 8 reports by default (ring_size); nothing is written until DESYNC.
#include "f3rt/game_netplay.hpp"
#include "f3rt/input.hpp"
#include "f3rt/netplay.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace f3rt::netplay {

using AppliedInputs =
    std::array<LocalInputWord, 2>; // P1 port, P2 port exactly as applied to the machine

// Facts the frontend / tool know and the recorder cannot. All frame numbers are Machine::frame values.
struct MatchMeta {
    uint64_t trigger_machine_frame =
        0; // lobby trigger / --until-trigger frame (local snapshot point)
    uint64_t edge_machine_frame =
        0; // frame at which in_versus first held (apply_sync / capture_sync point)
    uint64_t sync_machine_frame = 0;  // frame at which capture_sync / apply_sync ran
    uint64_t start_machine_frame = 0; // frame at match frame 0 (state in trigger.f3s)
    uint64_t lobby_hold_frames = 0;   // frames run under lobby_hold_frame
    double lobby_hold_ms = 0;         // wall time the lobby hold lasted
    uint64_t clock_resyncs = 0;       // frontend pacing counters at write time
    uint64_t audio_queue_drops = 0;
    uint64_t stall_count = 0;
    // Machine configuration the state files belong to (filled by begin_match); netplay-replay must build the same one.
    bool machine_gpu = false;
    std::string audio_backend; // Audio::backend_name
    uint64_t machine_state_bytes = 0;
    std::vector<std::pair<std::string, std::string>>
        extra; // free-form key/value, written to info.json
};

struct SessionFacts {
    uint64_t session_id = 0;
    uint64_t seed = 0;
    uint8_t player_index = 0;
    uint8_t input_delay = 0;
    uint32_t crc_interval = 60;
    std::string own_name, peer_name, room_code;
    std::array<uint8_t, 32> build_hash{};
    uint32_t desync_frame = 0;
    uint32_t desync_crc_p1 = 0, desync_crc_p2 = 0;
};

struct ReportRecord {
    uint32_t frame = 0; // completed match frames when the report was made
    uint32_t crc = 0;
    uint8_t phase = 0;
    uint32_t hashed_len = 0;
};

// Text format of inputs.txt (also parsed by `f3rt-tool netplay-replay`).
struct InputsFile {
    std::vector<std::pair<std::string, std::string>> header; // "@key value" lines, in order
    std::vector<AppliedInputs> frames;                       // index = match frame
};
std::string format_inputs_txt(const MatchMeta &meta, const SessionFacts &facts,
                              const std::vector<AppliedInputs> &frames);
bool parse_inputs_txt(const std::string &text, InputsFile &out, std::string &error);
std::string header_value(const InputsFile &file, const std::string &key,
                         const std::string &fallback = {});

// info.json, written by MatchRecorder::write_bundle and read back by `f3rt-tool netplay-replay` (cpp-yyjson).
// A key that is missing or has the wrong JSON type stays nullopt.
struct BundleInfo {
    std::optional<uint64_t> seed, frame, crc_interval, hashed_len, recorded_crc, player_index,
        machine_gpu, hash_layout;
    std::optional<std::string> audio_backend;
};
// Throws std::runtime_error if `json` is not valid JSON or its top level is not an object.
BundleInfo parse_info_json(const std::string &json);

class MatchRecorder {
public:
    explicit MatchRecorder(const GameNetplay *hook, size_t ring_size = 8);

    // Match frame 0: stores trigger.f3s in memory (once per match) and resets the per-match logs.
    void begin_match(Machine &m, uint64_t seed);

    // Called by run_lockstep_frame once per match frame, before the frame runs.
    void record_inputs(uint32_t match_frame, const AppliedInputs &words);

    // Called at a CRC report frame, after the frame ran. Returns the CRC to report.
    // One hashed-region copy (<= 496 bytes) into preallocated ring storage; no machine snapshot.
    uint32_t on_report(Machine &m, uint32_t completed_frame);

    MatchMeta meta;

    // Writes <base_dir>/<UTC timestamp>-<session>-p<idx>/ and returns its path. Throws std::runtime_error on I/O failure.
    std::filesystem::path write_bundle(const std::filesystem::path &base_dir,
                                       const SessionFacts &facts);

    const std::vector<AppliedInputs> &inputs() const { return m_inputs; }
    const std::vector<uint8_t> &trigger_state() const { return m_trigger_state; }

private:
    struct Entry {
        bool valid = false;
        ReportRecord rec;
        std::vector<uint8_t> hashed;
    };
    const GameNetplay *m_hook;
    std::vector<Entry> m_ring;
    size_t m_next = 0;
    std::vector<uint8_t> m_trigger_state;
    std::vector<AppliedInputs> m_inputs;
    uint64_t m_seed = 0;

    const Entry *find(uint32_t frame) const;
};

} // namespace f3rt::netplay
