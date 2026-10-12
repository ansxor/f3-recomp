#include "recorder.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/rom.hpp"
#include "cpp_yyjson.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <ctime>
#include <fstream>
#include <span>
#include <sstream>
#include <stdexcept>

namespace f3rt::netplay {

namespace {

constexpr size_t MAX_HASHED_BYTES = 2048;

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

namespace {

// One field of a parsed info.json object. nullopt when the key is absent or has another JSON type.
template <typename Object>
std::optional<uint64_t> info_u64(const Object &obj, std::string_view key) {
    const auto it = obj.find(key);
    if (it == obj.end()) return std::nullopt;
    return it->second.as_uint();
}

template <typename Object>
std::optional<std::string> info_string(const Object &obj, std::string_view key) {
    const auto it = obj.find(key);
    if (it == obj.end()) return std::nullopt;
    const auto text = it->second.as_string();
    if (!text) return std::nullopt;
    return std::string(*text);
}

} // namespace

BundleInfo parse_info_json(const std::string &json) {
    const auto doc = yyjson::read(json, yyjson::ReadFlag::AllowInvalidUnicode);
    const auto obj = doc.as_object();
    if (!obj) throw std::runtime_error("info.json is not a JSON object");
    BundleInfo info;
    info.seed = info_u64(*obj, "seed");
    info.frame = info_u64(*obj, "frame");
    info.crc_interval = info_u64(*obj, "crc_interval");
    info.hashed_len = info_u64(*obj, "hashed_len");
    info.recorded_crc = info_u64(*obj, "recorded_crc");
    info.player_index = info_u64(*obj, "player_index");
    info.machine_gpu = info_u64(*obj, "machine_gpu");
    info.hash_layout = info_u64(*obj, "hash_layout");
    info.audio_backend = info_string(*obj, "audio_backend");
    return info;
}

std::string format_inputs_txt(const MatchMeta &meta, const SessionFacts &facts,
                              const std::vector<AppliedInputs> &frames) {
    std::ostringstream out;
    out << "# f3rt netplay inputs v1\n"
        << "# '@key value' lines are metadata (machine frames are Machine::frame values); every "
           "other line is\n"
        << "# '<match frame> <P1 word hex> <P2 word hex>' exactly as applied by the lockstep step "
           "function.\n"
        << "@player_index " << unsigned(facts.player_index) << "\n"
        << "@input_delay " << unsigned(facts.input_delay) << "\n"
        << "@seed " << facts.seed << "\n"
        << "@trigger_machine_frame " << meta.trigger_machine_frame << "\n"
        << "@edge_machine_frame " << meta.edge_machine_frame << "\n"
        << "@sync_machine_frame " << meta.sync_machine_frame << "\n"
        << "@start_machine_frame " << meta.start_machine_frame << "\n"
        << "@frames_between_sync_and_start " << (meta.start_machine_frame - meta.sync_machine_frame)
        << "\n"
        << "@lobby_hold_frames " << meta.lobby_hold_frames << "\n"
        << "@lobby_hold_ms " << static_cast<long long>(meta.lobby_hold_ms) << "\n";
    char line[48];
    for (size_t f = 0; f < frames.size(); ++f) {
        std::snprintf(line, sizeof line, "%zu %04x %04x\n", f, unsigned(frames[f][0]),
                      unsigned(frames[f][1]));
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
            error = "inputs.txt line " + std::to_string(number) +
                    ": expected '<frame> <p1 hex> <p2 hex>'";
            return false;
        }
        if (frame != out.frames.size()) {
            error = "inputs.txt line " + std::to_string(number) + ": frame " +
                    std::to_string(frame) + " is not the next sequential frame (" +
                    std::to_string(out.frames.size()) + ")";
            return false;
        }
        out.frames.push_back({LocalInputWord(p1), LocalInputWord(p2)});
    }
    return true;
}

std::string header_value(const InputsFile &file, const std::string &key,
                         const std::string &fallback) {
    for (const auto &[k, v] : file.header) {
        if (k == key) return v;
    }
    return fallback;
}

MatchRecorder::MatchRecorder(const GameNetplay *hook, size_t ring_size)
    : m_hook(hook), m_ring(std::max<size_t>(ring_size, 2)) {}

void MatchRecorder::begin_match(Machine &m, uint64_t seed) {
    m_seed = seed;
    meta.start_machine_frame = m.frame;
    meta.machine_gpu = m.game_video != nullptr;
    meta.audio_backend = m.audio ? Audio::backend_name(m.audio->backend()) : "";
    meta.machine_state_bytes = m.state_size();
    m_trigger_state.resize(m.state_size());
    m.save_state(m_trigger_state);
    m_inputs.clear();
    // Storage is allocated here, once per match: on_report never allocates.
    for (auto &e : m_ring) {
        e.valid = false;
        e.hashed.reserve(MAX_HASHED_BYTES);
    }
    m_next = 0;
}

void MatchRecorder::record_inputs(uint32_t match_frame, const AppliedInputs &words) {
    if (match_frame != m_inputs.size()) {
        // Never happens with the lockstep loop (one step per frame, in order). Keep the log honest if it ever does.
        m_inputs.resize(match_frame);
    }
    m_inputs.push_back(words);
}

uint32_t MatchRecorder::on_report(Machine &m, uint32_t completed_frame) {
    Entry &e = m_ring[m_next];
    m_next = (m_next + 1) % m_ring.size();

    uint32_t crc = 0;
    uint8_t phase = 0;
    if (m_hook && m_hook->sync_hash_region) {
        uint8_t buf[MAX_HASHED_BYTES];
        const size_t n = m_hook->sync_hash_region(m, std::span<uint8_t>(buf, MAX_HASHED_BYTES), phase);
        crc = crc32(buf, n);
        e.hashed.assign(buf, buf + n);
    } else {
        e.hashed.clear();
        crc = (m_hook && m_hook->sync_hash) ? m_hook->sync_hash(m) : m.state_crc();
    }
    e.rec = {completed_frame, crc, phase, uint32_t(e.hashed.size())};
    e.valid = true;
    return crc;
}

const MatchRecorder::Entry *MatchRecorder::find(uint32_t frame) const {
    for (const auto &e : m_ring) {
        if (e.valid && e.rec.frame == frame) return &e;
    }
    return nullptr;
}

std::filesystem::path MatchRecorder::write_bundle(const std::filesystem::path &base_dir,
                                                  const SessionFacts &facts) {
    namespace fs = std::filesystem;
    const fs::path dir = base_dir / (utc_stamp() + "-" + hex_u64(facts.session_id, 16) + "-p" +
                                     std::to_string(facts.player_index));
    std::error_code ec;
    fs::create_directories(dir, ec);
    if (ec) throw std::runtime_error("cannot create " + dir.string() + ": " + ec.message());

    const Entry *at = find(facts.desync_frame);
    bool exact = at != nullptr;
    if (!at) {
        // The DESYNC message can arrive after more reports than the ring holds. Fall back to the newest older report.
        for (const auto &e : m_ring) {
            if (e.valid && e.rec.frame <= facts.desync_frame &&
                (!at || e.rec.frame > at->rec.frame))
                at = &e;
        }
    }
    const Entry *prev = at && at->rec.frame >= facts.crc_interval
                            ? find(at->rec.frame - facts.crc_interval)
                            : nullptr;

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
    write_file(dir / "trigger.f3s", m_trigger_state.data(), m_trigger_state.size());
    write_text(dir / "inputs.txt", format_inputs_txt(meta, facts, m_inputs));

    const uint32_t local_crc =
        (facts.player_index == 0) ? facts.desync_crc_p1 : facts.desync_crc_p2;
    const uint32_t remote_crc =
        (facts.player_index == 0) ? facts.desync_crc_p2 : facts.desync_crc_p1;

    yyjson::object j;
    j.emplace("bundle_version", 2);
    j.emplace("hash_layout", 2);
    j.emplace("machine_gpu", meta.machine_gpu ? 1 : 0);
    j.emplace("audio_backend", meta.audio_backend, yyjson::copy_string);
    j.emplace("machine_state_bytes", meta.machine_state_bytes);
    j.emplace("frame", facts.desync_frame);
    j.emplace("local_crc", local_crc);
    j.emplace("remote_crc", remote_crc);
    j.emplace("local_crc_hex", "0x" + hex_u64(local_crc, 8), yyjson::copy_string);
    j.emplace("remote_crc_hex", "0x" + hex_u64(remote_crc, 8), yyjson::copy_string);
    j.emplace("recorded_crc", at ? at->rec.crc : 0u);
    j.emplace("seed", facts.seed);
    j.emplace("input_delay", unsigned(facts.input_delay));
    j.emplace("crc_interval", facts.crc_interval);
    j.emplace("hash_phase", at ? unsigned(at->rec.phase) : 0u);
    j.emplace("player_index", unsigned(facts.player_index));
    j.emplace("own_name", facts.own_name, yyjson::copy_string);
    j.emplace("peer_name", facts.peer_name, yyjson::copy_string);
    j.emplace("room_code", facts.room_code, yyjson::copy_string);
    j.emplace("session_id", hex_u64(facts.session_id, 16), yyjson::copy_string);
    std::string build_hash;
    for (uint8_t b : facts.build_hash) build_hash += hex_u64(b, 2);
    j.emplace("build_hash", build_hash, yyjson::copy_string);
    j.emplace("stall_count", meta.stall_count);
    j.emplace("clock_resyncs", meta.clock_resyncs);
    j.emplace("audio_queue_drops", meta.audio_queue_drops);
    j.emplace("trigger_machine_frame", meta.trigger_machine_frame);
    j.emplace("edge_machine_frame", meta.edge_machine_frame);
    j.emplace("sync_machine_frame", meta.sync_machine_frame);
    j.emplace("start_machine_frame", meta.start_machine_frame);
    j.emplace("frames_between_sync_and_start", meta.start_machine_frame - meta.sync_machine_frame);
    j.emplace("lobby_hold_frames", meta.lobby_hold_frames);
    j.emplace("lobby_hold_ms", static_cast<int64_t>(meta.lobby_hold_ms));
    j.emplace("matched_frames_recorded", m_inputs.size());
    j.emplace("hashed_is_mismatch_frame", at && exact);
    j.emplace(
        "state_note",
        "No state snapshot is stored during play. Regenerate the state at any match frame with "
        "`f3rt-tool netplay-replay --bundle <this folder> --dump-state FRAME out.f3s` (trigger.f3s "
        "+ inputs.txt). "
        "hashed.bin holds the hashed bytes of the report at hashed_frame followed by the previous "
        "report's bytes.");
    j.emplace("hashed_frame", at ? int64_t(at->rec.frame) : int64_t(-1));
    j.emplace("hashed_offset", 0);
    j.emplace("hashed_len", cur_len);
    j.emplace("hashed_prev_frame", prev ? int64_t(prev->rec.frame) : int64_t(-1));
    j.emplace("hashed_prev_offset", cur_len);
    j.emplace("hashed_prev_len", prev_len);
    for (const auto &[k, v] : meta.extra) j.emplace(k, v, yyjson::copy_string);

    // Ring slots in frame order, oldest report first, so the output does not depend on
    // where the next write landed in the ring.
    std::vector<size_t> sorted;
    for (size_t i = 0; i < m_ring.size(); ++i) {
        if (m_ring[i].valid) sorted.push_back(i);
    }
    std::sort(sorted.begin(), sorted.end(),
              [this](size_t a, size_t b) { return m_ring[a].rec.frame < m_ring[b].rec.frame; });
    yyjson::array reports;
    for (size_t i : sorted) {
        const Entry &e = m_ring[i];
        yyjson::object r;
        r.emplace("frame", e.rec.frame);
        r.emplace("crc", e.rec.crc);
        r.emplace("phase", unsigned(e.rec.phase));
        r.emplace("hashed_len", e.rec.hashed_len);
        reports.emplace_back(std::move(r));
    }
    j.emplace("reports", std::move(reports));

    const auto text =
        j.write(yyjson::WriteFlag::Pretty | yyjson::WriteFlag::PrettyTwoSpaces |
                yyjson::WriteFlag::NewlineAtEnd | yyjson::WriteFlag::AllowInvalidUnicode);
    write_text(dir / "info.json", std::string(text));
    return dir;
}

} // namespace f3rt::netplay
