#include "recorder.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/rom.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <sstream>
#include <stdexcept>

namespace f3rt::netplay {

namespace {

constexpr size_t max_hashed_bytes = 2048;

std::string hex_u64(uint64_t v, int digits) {
    char buf[32];
    std::snprintf(buf, sizeof buf, "%0*llx", digits, static_cast<unsigned long long>(v));
    return buf;
}

std::string utc_stamp() {
    const std::time_t now = std::chrono::system_clock::to_time_t(std::chrono::system_clock::now());
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &now);
#else
    gmtime_r(&now, &tm);
#endif
    char buf[32];
    std::strftime(buf, sizeof buf, "%Y%m%dT%H%M%SZ", &tm);
    return buf;
}

void write_file(const std::filesystem::path &path, const void *data, size_t size) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) throw std::runtime_error("cannot create " + path.string());
    out.write(static_cast<const char *>(data), std::streamsize(size));
    if (!out) throw std::runtime_error("cannot write " + path.string());
}

void write_text(const std::filesystem::path &path, const std::string &text) {
    write_file(path, text.data(), text.size());
}

} // namespace

std::string json_escape(const std::string &text) {
    std::string out;
    for (const unsigned char c : text) {
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c < 0x20) {
                char buf[8];
                std::snprintf(buf, sizeof buf, "\\u%04x", c);
                out += buf;
            } else {
                out += char(c);
            }
        }
    }
    return out;
}

bool json_find_u64(const std::string &json, const std::string &key, uint64_t &out) {
    const std::string needle = "\"" + key + "\"";
    const size_t at = json.find(needle);
    if (at == std::string::npos) return false;
    size_t i = json.find(':', at + needle.size());
    if (i == std::string::npos) return false;
    ++i;
    while (i < json.size() && (json[i] == ' ' || json[i] == '\t')) ++i;
    if (i >= json.size() || json[i] < '0' || json[i] > '9') return false;
    uint64_t value = 0;
    while (i < json.size() && json[i] >= '0' && json[i] <= '9') value = value * 10 + uint64_t(json[i++] - '0');
    out = value;
    return true;
}

bool json_find_string(const std::string &json, const std::string &key, std::string &out) {
    const std::string needle = "\"" + key + "\"";
    const size_t at = json.find(needle);
    if (at == std::string::npos) return false;
    size_t i = json.find(':', at + needle.size());
    if (i == std::string::npos) return false;
    i = json.find('"', i);
    if (i == std::string::npos) return false;
    const size_t end = json.find('"', i + 1);
    if (end == std::string::npos) return false;
    out = json.substr(i + 1, end - i - 1);
    return true;
}

std::string format_inputs_txt(const MatchMeta &meta, const SessionFacts &facts, const std::vector<AppliedInputs> &frames) {
    std::ostringstream out;
    out << "# f3rt netplay inputs v1\n"
        << "# '@key value' lines are metadata (machine frames are Machine::frame values); every other line is\n"
        << "# '<match frame> <P1 word hex> <P2 word hex>' exactly as applied by the lockstep step function.\n"
        << "@player_index " << unsigned(facts.player_index) << "\n"
        << "@input_delay " << unsigned(facts.input_delay) << "\n"
        << "@seed " << facts.seed << "\n"
        << "@trigger_machine_frame " << meta.trigger_machine_frame << "\n"
        << "@edge_machine_frame " << meta.edge_machine_frame << "\n"
        << "@sync_machine_frame " << meta.sync_machine_frame << "\n"
        << "@start_machine_frame " << meta.start_machine_frame << "\n"
        << "@frames_between_sync_and_start " << (meta.start_machine_frame - meta.sync_machine_frame) << "\n"
        << "@lobby_hold_frames " << meta.lobby_hold_frames << "\n"
        << "@lobby_hold_ms " << static_cast<long long>(meta.lobby_hold_ms) << "\n";
    char line[48];
    for (size_t f = 0; f < frames.size(); ++f) {
        std::snprintf(line, sizeof line, "%zu %04x %04x\n", f, unsigned(frames[f][0]), unsigned(frames[f][1]));
        out << line;
    }
    return out.str();
}

bool parse_inputs_txt(const std::string &text, InputsFile &out, std::string &error) {
    out = {};
    std::istringstream in(text);
    std::string line;
    size_t number = 0;
    while (std::getline(in, line)) {
        ++number;
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (line.empty() || line[0] == '#') continue;
        if (line[0] == '@') {
            const size_t space = line.find(' ');
            if (space == std::string::npos) {
                out.header.emplace_back(line.substr(1), "");
            } else {
                out.header.emplace_back(line.substr(1, space - 1), line.substr(space + 1));
            }
            continue;
        }
        std::istringstream fields(line);
        size_t frame = 0;
        unsigned p1 = 0, p2 = 0;
        fields >> frame >> std::hex >> p1 >> p2;
        if (fields.fail() || p1 > 0xffff || p2 > 0xffff) {
            error = "inputs.txt line " + std::to_string(number) + ": expected '<frame> <p1 hex> <p2 hex>'";
            return false;
        }
        if (frame != out.frames.size()) {
            error = "inputs.txt line " + std::to_string(number) + ": frame " + std::to_string(frame) +
                    " is not the next sequential frame (" + std::to_string(out.frames.size()) + ")";
            return false;
        }
        out.frames.push_back({LocalInputWord(p1), LocalInputWord(p2)});
    }
    return true;
}

std::string header_value(const InputsFile &file, const std::string &key, const std::string &fallback) {
    for (const auto &[k, v] : file.header) {
        if (k == key) return v;
    }
    return fallback;
}

MatchRecorder::MatchRecorder(const GameNetplay *hook, size_t ring_size)
    : hook_(hook), ring_(std::max<size_t>(ring_size, 2)) {}

void MatchRecorder::begin_match(Machine &m, uint64_t seed) {
    seed_ = seed;
    meta.start_machine_frame = m.frame;
    meta.machine_gpu = m.game_video != nullptr;
    meta.audio_backend = m.audio ? Audio::backend_name(m.audio->backend()) : "";
    meta.machine_state_bytes = m.state_size();
    trigger_state_.resize(m.state_size());
    m.save_state(trigger_state_);
    inputs_.clear();
    // Storage is allocated here, once per match: on_report never allocates.
    for (auto &e : ring_) {
        e.valid = false;
        e.hashed.reserve(max_hashed_bytes);
    }
    next_ = 0;
}

void MatchRecorder::record_inputs(uint32_t match_frame, const AppliedInputs &words) {
    if (match_frame != inputs_.size()) {
        // Never happens with the lockstep loop (one step per frame, in order). Keep the log honest if it ever does.
        inputs_.resize(match_frame);
    }
    inputs_.push_back(words);
}

uint32_t MatchRecorder::on_report(Machine &m, uint32_t completed_frame) {
    Entry &e = ring_[next_];
    next_ = (next_ + 1) % ring_.size();

    uint32_t crc = 0;
    uint8_t phase = 0;
    if (hook_ && hook_->sync_hash_region) {
        uint8_t buf[max_hashed_bytes];
        const size_t n = hook_->sync_hash_region(m, buf, phase);
        crc = crc32(buf, n);
        e.hashed.assign(buf, buf + n);
    } else {
        e.hashed.clear();
        crc = (hook_ && hook_->sync_hash) ? hook_->sync_hash(m) : m.state_crc();
    }
    e.rec = {completed_frame, crc, phase, uint32_t(e.hashed.size())};
    e.valid = true;
    return crc;
}

const MatchRecorder::Entry *MatchRecorder::find(uint32_t frame) const {
    for (const auto &e : ring_) {
        if (e.valid && e.rec.frame == frame) return &e;
    }
    return nullptr;
}

std::filesystem::path MatchRecorder::write_bundle(const std::filesystem::path &base_dir, const SessionFacts &facts) {
    namespace fs = std::filesystem;
    const fs::path dir = base_dir / (utc_stamp() + "-" + hex_u64(facts.session_id, 16) + "-p" + std::to_string(facts.player_index));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw std::runtime_error("cannot create " + dir.string() + ": " + ec.message());

    const Entry *at = find(facts.desync_frame);
    bool exact = at != nullptr;
    if (!at) {
        // The DESYNC message can arrive after more reports than the ring holds. Fall back to the newest older report.
        for (const auto &e : ring_) {
            if (e.valid && e.rec.frame <= facts.desync_frame && (!at || e.rec.frame > at->rec.frame)) at = &e;
        }
    }
    const Entry *prev = at && at->rec.frame >= facts.crc_interval ? find(at->rec.frame - facts.crc_interval) : nullptr;

    std::vector<uint8_t> hashed;
    uint32_t cur_len = 0, prev_len = 0;
    if (at) {
        hashed.insert(hashed.end(), at->hashed.begin(), at->hashed.end());
        cur_len = uint32_t(at->hashed.size());
        if (prev) {
            hashed.insert(hashed.end(), prev->hashed.begin(), prev->hashed.end());
            prev_len = uint32_t(prev->hashed.size());
        }
    }
    write_file(dir / "hashed.bin", hashed.data(), hashed.size());
    write_file(dir / "trigger.f3s", trigger_state_.data(), trigger_state_.size());
    write_text(dir / "inputs.txt", format_inputs_txt(meta, facts, inputs_));

    const uint32_t local_crc = (facts.player_index == 0) ? facts.desync_crc_p1 : facts.desync_crc_p2;
    const uint32_t remote_crc = (facts.player_index == 0) ? facts.desync_crc_p2 : facts.desync_crc_p1;

    std::ostringstream j;
    j << "{\n";
    j << "  \"bundle_version\": 2,\n";
    j << "  \"hash_layout\": 2,\n";
    j << "  \"machine_gpu\": " << (meta.machine_gpu ? 1 : 0) << ",\n";
    j << "  \"audio_backend\": \"" << json_escape(meta.audio_backend) << "\",\n";
    j << "  \"machine_state_bytes\": " << meta.machine_state_bytes << ",\n";
    j << "  \"frame\": " << facts.desync_frame << ",\n";
    j << "  \"local_crc\": " << local_crc << ",\n";
    j << "  \"remote_crc\": " << remote_crc << ",\n";
    j << "  \"local_crc_hex\": \"0x" << hex_u64(local_crc, 8) << "\",\n";
    j << "  \"remote_crc_hex\": \"0x" << hex_u64(remote_crc, 8) << "\",\n";
    j << "  \"recorded_crc\": " << (at ? at->rec.crc : 0) << ",\n";
    j << "  \"seed\": " << facts.seed << ",\n";
    j << "  \"input_delay\": " << unsigned(facts.input_delay) << ",\n";
    j << "  \"crc_interval\": " << facts.crc_interval << ",\n";
    j << "  \"hash_phase\": " << (at ? unsigned(at->rec.phase) : 0) << ",\n";
    j << "  \"player_index\": " << unsigned(facts.player_index) << ",\n";
    j << "  \"own_name\": \"" << json_escape(facts.own_name) << "\",\n";
    j << "  \"peer_name\": \"" << json_escape(facts.peer_name) << "\",\n";
    j << "  \"room_code\": \"" << json_escape(facts.room_code) << "\",\n";
    j << "  \"session_id\": \"" << hex_u64(facts.session_id, 16) << "\",\n";
    j << "  \"build_hash\": \"";
    for (uint8_t b : facts.build_hash) j << hex_u64(b, 2);
    j << "\",\n";
    j << "  \"stall_count\": " << meta.stall_count << ",\n";
    j << "  \"clock_resyncs\": " << meta.clock_resyncs << ",\n";
    j << "  \"audio_queue_drops\": " << meta.audio_queue_drops << ",\n";
    j << "  \"trigger_machine_frame\": " << meta.trigger_machine_frame << ",\n";
    j << "  \"edge_machine_frame\": " << meta.edge_machine_frame << ",\n";
    j << "  \"sync_machine_frame\": " << meta.sync_machine_frame << ",\n";
    j << "  \"start_machine_frame\": " << meta.start_machine_frame << ",\n";
    j << "  \"frames_between_sync_and_start\": " << (meta.start_machine_frame - meta.sync_machine_frame) << ",\n";
    j << "  \"lobby_hold_frames\": " << meta.lobby_hold_frames << ",\n";
    j << "  \"lobby_hold_ms\": " << static_cast<long long>(meta.lobby_hold_ms) << ",\n";
    j << "  \"matched_frames_recorded\": " << inputs_.size() << ",\n";
    j << "  \"hashed_is_mismatch_frame\": " << (at && exact ? "true" : "false") << ",\n";
    j << "  \"state_note\": \"No state snapshot is stored during play. Regenerate the state at any match frame with "
         "`f3rt-tool netplay-replay --bundle <this folder> --dump-state FRAME out.f3s` (trigger.f3s + inputs.txt). "
         "hashed.bin holds the hashed bytes of the report at hashed_frame followed by the previous report's bytes.\",\n";
    j << "  \"hashed_frame\": " << (at ? int64_t(at->rec.frame) : int64_t(-1)) << ",\n";
    j << "  \"hashed_offset\": 0,\n";
    j << "  \"hashed_len\": " << cur_len << ",\n";
    j << "  \"hashed_prev_frame\": " << (prev ? int64_t(prev->rec.frame) : int64_t(-1)) << ",\n";
    j << "  \"hashed_prev_offset\": " << cur_len << ",\n";
    j << "  \"hashed_prev_len\": " << prev_len << ",\n";
    for (const auto &[k, v] : meta.extra) {
        j << "  \"" << json_escape(k) << "\": \"" << json_escape(v) << "\",\n";
    }
    j << "  \"reports\": [";
    std::vector<const Entry *> sorted;
    for (const auto &e : ring_) {
        if (e.valid) sorted.push_back(&e);
    }
    std::sort(sorted.begin(), sorted.end(), [](const Entry *a, const Entry *b) { return a->rec.frame < b->rec.frame; });
    for (size_t i = 0; i < sorted.size(); ++i) {
        const auto &r = sorted[i]->rec;
        j << (i ? ", " : "") << "{\"frame\": " << r.frame << ", \"crc\": " << r.crc << ", \"phase\": " << unsigned(r.phase)
          << ", \"hashed_len\": " << r.hashed_len << "}";
    }
    j << "]\n}\n";
    write_text(dir / "info.json", j.str());
    return dir;
}

} // namespace f3rt::netplay
