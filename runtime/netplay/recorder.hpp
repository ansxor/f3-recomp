#pragma once
// Local-only desync diagnostics. Nothing in here touches the network.
//
// A MatchRecorder keeps, per match:
//   - the machine state at match frame 0 (once, in memory),
//   - every applied input word pair, one entry per match frame,
//   - a small ring of the last few CRC reports, each with the exact bytes the hash covered.
// The state at the mismatch frame is not kept: `f3rt-tool netplay-replay --dump-state` regenerates it from trigger.f3s
// and inputs.txt.
// On DESYNC, write_bundle() turns that into a folder a developer can diff offline (see docs/site/guide/netplay.md).
#include "f3rt/game_netplay.hpp"
#include "f3rt/input.hpp"
#include "f3rt/netplay.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <string>
#include <utility>
#include <vector>

namespace f3rt::netplay {

using AppliedInputs = std::array<LocalInputWord, 2>; // P1 port, P2 port exactly as applied to the machine

// Facts the frontend / tool know and the recorder cannot. All frame numbers are Machine::frame values.
struct MatchMeta {
    uint64_t trigger_machine_frame = 0;   // lobby trigger / --until-trigger frame (local snapshot point)
    uint64_t edge_machine_frame = 0;      // frame at which in_versus first held (apply_sync / capture_sync point)
    uint64_t sync_machine_frame = 0;      // frame at which capture_sync / apply_sync ran
    uint64_t start_machine_frame = 0;     // frame at match frame 0 (state in trigger.f3s)
    uint64_t lobby_hold_frames = 0;       // frames run under lobby_hold_frame
    double lobby_hold_ms = 0;             // wall time the lobby hold lasted
    uint64_t clock_resyncs = 0;           // frontend pacing counters at write time
    uint64_t audio_queue_drops = 0;
    uint64_t stall_count = 0;
    // Machine configuration the state files belong to (filled by begin_match); netplay-replay must build the same one.
    bool machine_gpu = false;
    std::string audio_backend;     // Audio::backend_name
    uint64_t machine_state_bytes = 0;
    std::vector<std::pair<std::string, std::string>> extra; // free-form key/value, written to info.json
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
std::string format_inputs_txt(const MatchMeta &meta, const SessionFacts &facts, const std::vector<AppliedInputs> &frames);
bool parse_inputs_txt(const std::string &text, InputsFile &out, std::string &error);
std::string header_value(const InputsFile &file, const std::string &key, const std::string &fallback = {});

// Minimal flat JSON helpers (info.json is flat apart from the "reports" array that is not read back).
std::string json_escape(const std::string &text);
bool json_find_u64(const std::string &json, const std::string &key, uint64_t &out);
bool json_find_string(const std::string &json, const std::string &key, std::string &out); // plain strings without escapes

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
    std::filesystem::path write_bundle(const std::filesystem::path &base_dir, const SessionFacts &facts);

    const std::vector<AppliedInputs> &inputs() const { return inputs_; }
    const std::vector<uint8_t> &trigger_state() const { return trigger_state_; }

private:
    struct Entry {
        bool valid = false;
        ReportRecord rec;
        std::vector<uint8_t> hashed;
    };
    const GameNetplay *hook_;
    std::vector<Entry> ring_;
    size_t next_ = 0;
    std::vector<uint8_t> trigger_state_;
    std::vector<AppliedInputs> inputs_;
    uint64_t seed_ = 0;

    const Entry *find(uint32_t frame) const;
};

} // namespace f3rt::netplay
