#include "trace_recorder.hpp"

#include "audio/hle/replay.hpp"
#include "json.hpp"
#include "simplify.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <stdexcept>
#include <unordered_map>

namespace f3rt::webplayer {

namespace {

constexpr uint32_t NONE = 0xffffffffu;
constexpr uint16_t NO_SECTION = 0xffffu;
constexpr size_t WINDOW_FRAMES = 512;
constexpr size_t REGION_COLUMNS = 256;
constexpr uint8_t SECTION_OPEN = 255; // end_how of a section cut by the end of the span

// Per-voice curve lanes (README 4.8). tolerance in lane units; values outside [lo, hi] cannot occur.
struct LaneDef {
    const char *name;
    bool is_signed;
    int32_t tolerance;
};
constexpr unsigned LANE_COUNT = 10;
constexpr LaneDef LANES[LANE_COUNT] = {
    {"pitch", true, 2},       {"glide", true, 2},       {"lfo", true, 256},   {"env_pitch", true, 256},
    {"env_filter", true, 256}, {"env_amp", true, 256}, {"left", false, 170}, {"right", false, 170},
    {"k1", false, 320},       {"k2", false, 320},
};
constexpr unsigned LANE_LEFT = 6, LANE_RIGHT = 7, LANE_K1 = 8, LANE_K2 = 9;

using LaneValues = std::array<int32_t, LANE_COUNT>;

// Voice flags (README 4.12).
constexpr uint8_t VOICE_LOOP = 1, VOICE_REVERSE = 2, VOICE_BIDI = 4, VOICE_DIRECT = 8, VOICE_LEGATO = 16,
                  VOICE_RETRIGGER = 32, VOICE_HANDOFF = 64;
constexpr uint8_t REGION_LOOP = 1, REGION_REVERSE = 2, REGION_BIDI = 4;

// Channel record offsets of the sound driver (the Channel::parameters bytes the HLE reads; README 4.9).
constexpr unsigned CH_FIRST = 0x0a;          // first controller byte
constexpr unsigned CH_BEND = 0x0a;           // word, 0x4000 = centre
constexpr unsigned CH_MOD_0C = 0x0c;         // word
constexpr unsigned CH_MOD_10 = 0x10;         // word
constexpr unsigned CH_MOD_12 = 0x12;         // word
constexpr unsigned CH_EXPRESSION = 0x14;
constexpr unsigned CH_MOD_1A = 0x1a;         // word
constexpr unsigned CH_LAYER_MASK = 0x1c;
constexpr unsigned CH_VOLUME = 0x20;
constexpr unsigned CH_PREVIOUS_KEY = 0x22;   // internal state, not a lane
constexpr unsigned CH_PAN = 0x26;
constexpr unsigned CH_PAIR_SELECT = 0x31;
constexpr unsigned CH_TRANSPOSE = 0x35;
constexpr unsigned CH_END = 0x38;            // Channel::parameters size
constexpr int32_t BEND_CENTRE = 0x4000;

enum ChanLane : uint8_t {
    CL_PROGRAM, CL_BEND, CL_EXPRESSION, CL_VOLUME, CL_PAN, CL_TRANSPOSE, CL_SEQ_VOLUME, CL_PAIR_SELECT, CL_LAYER_MASK,
    CL_MOD_0C, CL_MOD_10, CL_MOD_12, CL_MOD_1A, CL_NAMED
};
constexpr const char *CHAN_LANE_NAMES[CL_NAMED] = {"program", "bend",       "expression", "volume",    "pan",
                                                   "transpose", "seq_volume", "pair_select", "layer_mask", "mod_0c",
                                                   "mod_10",   "mod_12",     "mod_1a"};
// seq_volume fades change every 12 ms; their runs are stored as linear segments.
constexpr uint32_t FADE_GAP_FRAMES = 512;
constexpr int32_t FADE_TOLERANCE = 1;

uint16_t word_at(const hle::Channel &c, unsigned offset) {
    return uint16_t((uint16_t(c.parameters[offset]) << 8) | c.parameters[offset + 1]);
}

std::array<int32_t, CL_NAMED> named_lanes(const hle::Channel &c) {
    std::array<int32_t, CL_NAMED> v{};
    v[CL_PROGRAM] = c.program;
    v[CL_BEND] = int16_t(uint16_t(word_at(c, CH_BEND) - BEND_CENTRE));
    v[CL_EXPRESSION] = c.parameters[CH_EXPRESSION];
    v[CL_VOLUME] = c.parameters[CH_VOLUME];
    v[CL_PAN] = int8_t(c.parameters[CH_PAN]);
    v[CL_TRANSPOSE] = int8_t(c.parameters[CH_TRANSPOSE]);
    v[CL_SEQ_VOLUME] = c.sequence_volume;
    v[CL_PAIR_SELECT] = c.parameters[CH_PAIR_SELECT];
    v[CL_LAYER_MASK] = c.parameters[CH_LAYER_MASK];
    v[CL_MOD_0C] = word_at(c, CH_MOD_0C);
    v[CL_MOD_10] = word_at(c, CH_MOD_10);
    v[CL_MOD_12] = word_at(c, CH_MOD_12);
    v[CL_MOD_1A] = word_at(c, CH_MOD_1A);
    return v;
}

// Parameter bytes that have no named lane and are not internal state.
std::array<bool, CH_END> raw_lane_offsets() {
    std::array<bool, CH_END> raw{};
    for (unsigned i = CH_FIRST; i < CH_END; ++i) raw[i] = true;
    for (const unsigned named : {CH_BEND, CH_MOD_0C, CH_MOD_10, CH_MOD_12, CH_MOD_1A}) raw[named] = raw[named + 1] = false;
    for (const unsigned single : {CH_EXPRESSION, CH_LAYER_MASK, CH_VOLUME, CH_PREVIOUS_KEY, CH_PAN, CH_PAIR_SELECT, CH_TRANSPOSE})
        raw[single] = false;
    return raw;
}

uint32_t to_frame(uint64_t frame) {
    if (frame >= NONE) throw std::runtime_error("trace frame exceeds 32 bits");
    return uint32_t(frame);
}

// ---- binary writer -----------------------------------------------------------------------------------------------

template <class T> constexpr const char *dtype_name() {
    if constexpr (std::is_same_v<T, uint8_t>) return "u8";
    else if constexpr (std::is_same_v<T, int8_t>) return "i8";
    else if constexpr (std::is_same_v<T, uint16_t>) return "u16";
    else if constexpr (std::is_same_v<T, int16_t>) return "i16";
    else if constexpr (std::is_same_v<T, uint32_t>) return "u32";
    else if constexpr (std::is_same_v<T, int32_t>) return "i32";
    else return "f32";
}

// songs/<id>.bin: "F3TR", u32 version = 1, u32 total_bytes, u32 0, then 8-byte aligned little-endian columns.
class BinWriter {
public:
    BinWriter() : bytes_(16, 0) {
        std::memcpy(bytes_.data(), "F3TR", 4);
        bytes_[4] = 1;
    }
    // Appends the column and writes its JSON descriptor as the value of the current key.
    template <class T> void column(json::Writer &w, std::span<const T> values) {
        while (bytes_.size() % 8) bytes_.push_back(0);
        const size_t offset = bytes_.size();
        for (const T v : values) {
            using U = std::make_unsigned_t<T>;
            const U u = std::bit_cast<U>(v);
            for (size_t b = 0; b < sizeof(T); ++b) bytes_.push_back(uint8_t(u >> (8 * b)));
        }
        w.begin_object();
        w.member("offset", offset);
        w.member("dtype", dtype_name<T>());
        w.member("length", values.size());
        w.end_object();
    }
    template <class T> void column(json::Writer &w, const std::vector<T> &values) {
        column<T>(w, std::span<const T>(values));
    }
    std::vector<uint8_t> finish() {
        while (bytes_.size() % 8) bytes_.push_back(0);
        const uint32_t total = uint32_t(bytes_.size());
        for (size_t b = 0; b < 4; ++b) bytes_[8 + b] = uint8_t(total >> (8 * b));
        return std::move(bytes_);
    }

private:
    std::vector<uint8_t> bytes_;
};

// float peak -> u8 level: clamp(round(255 + 2.5 * dB), 0, 255)
uint8_t level_byte(float peak) {
    if (!(peak > 0.f)) return 0;
    const double db = 20.0 * std::log10(double(peak));
    return uint8_t(std::clamp(std::lround(255.0 + 2.5 * db), 0L, 255L));
}

const char *program_name(hle::EffectsProgram p) {
    switch (p) {
    case hle::EffectsProgram::None: return "None";
    case hle::EffectsProgram::Reverb: return "Reverb";
    case hle::EffectsProgram::Hall: return "Hall";
    case hle::EffectsProgram::Chorus: return "Chorus";
    case hle::EffectsProgram::Echo: return "Echo";
    case hle::EffectsProgram::Mute: return "Mute";
    }
    return "None";
}

} // namespace

const char *end_reason_name(EndReason reason) {
    switch (reason) {
    case EndReason::LoopWrap: return "loop_wrap";
    case EndReason::SongEnd: return "song_end";
    case EndReason::Cap: return "cap";
    }
    return "cap";
}

// ---- recorder state ----------------------------------------------------------------------------------------------

namespace {

struct SectionRow {
    uint8_t owner = 0, sequence = 0, phase = 0, end_how = SECTION_OPEN;
    uint16_t repeat = 0, bars = 0, rate = 0;
    int16_t transpose = 0;
    uint32_t start_frame = 0, end_frame = NONE;
    uint64_t start_tick = 0;
};
struct BarRow {
    uint32_t frame;
    uint16_t section;
    uint32_t bar;
};
struct RateRow {
    uint32_t frame;
    uint8_t sequence;
    uint16_t rate;
};
struct NoteRow {
    uint64_t instance = 0, tick = 0;
    uint32_t on = 0, off = NONE;
    uint8_t sequence = 0, track = 0, key = 0, kernel_key = 0, velocity = 0;
    uint16_t program = 0, section = NO_SECTION;
    uint32_t channel = 0; // channel key (sequence << 8 | track) until finish() numbers the channels
    uint32_t voice_first = 0;
    uint8_t voice_count = 0;
    bool keep = true;
};
struct ParamSample {
    uint32_t frame;
    LaneValues value;
};
struct VoiceRow {
    uint32_t note = 0; // index into notes (record order) until finish() sorts them
    uint8_t layer = 0, slot = 0, pair = 0, flags = 0, skey = 0, filter_mode = 0;
    uint32_t start = 0, release = NONE, stop = NONE;
    uint64_t start_tick = 0;
    uint32_t sample_start = 0, sample_end = 0;
    bool loop = false, reverse = false;
    uint32_t order = 0;
    LaneValues onset{};
    std::vector<ParamSample> samples;
};
struct ChannelState {
    hle::Channel last;
    bool seen = false;
};
struct ChanEvent {
    uint32_t frame;
    uint32_t channel; // channel key
    uint16_t lane;    // CL_* or CL_NAMED + raw offset
    int32_t value;
};
struct EffectRow {
    uint32_t frame;
    bool param;
    uint8_t preset, index, value;
    hle::EffectsInfo info;
};
struct GainRow {
    uint32_t frame;
    uint8_t channel, raw;
    float linear;
};
struct Region {
    uint32_t start, end;
    uint8_t flags;
    bool operator<(const Region &o) const {
        if (start != o.start) return start < o.start;
        if (end != o.end) return end < o.end;
        return (flags & (REGION_LOOP | REGION_REVERSE)) < (o.flags & (REGION_LOOP | REGION_REVERSE));
    }
};

LaneValues lane_values(const hle::VoiceEvent &e) {
    LaneValues v{};
    v[0] = e.pitch;
    v[1] = e.glide;
    v[2] = e.lfo;
    v[3] = e.env[0];
    v[4] = e.env[1];
    v[5] = e.env[2];
    v[6] = e.left_volume;
    v[7] = e.right_volume;
    v[8] = e.k1;
    v[9] = e.k2;
    return v;
}

void write_envelope(json::Writer &w, const hle::EnvelopeInfo &e) {
    w.begin_object();
    w.key("levels").begin_array();
    for (const auto v : e.levels) w.value(v);
    w.end_array();
    w.key("rates").begin_array();
    for (const auto v : e.rates) w.value(v);
    w.end_array();
    w.member("key_rate", e.key_rate);
    w.member("velocity_curve", e.velocity_curve);
    w.member("one_shot", e.one_shot);
    w.member("loop", e.loop);
    w.member("vel_level", e.vel_level);
    w.member("vel_attack", e.vel_attack);
    w.end_object();
}

template <class F> void write_splits(json::Writer &w, const std::vector<hle::SplitInfo> &splits, F &&region_of) {
    w.key("splits").begin_array(true);
    for (const auto &s : splits) {
        w.begin_object();
        w.member("key_low", s.key_low);
        w.member("key_high", s.key_high);
        w.member("tuning", s.tuning);
        w.member("loop_mode", s.loop_mode);
        w.member("accumulator", s.accumulator);
        w.member("start", s.start);
        w.member("end", s.end);
        w.member("start_word", s.start_word);
        w.member("end_word", s.end_word);
        w.member("phase_word", s.phase_word);
        w.member("loop", s.loop);
        w.member("bidirectional", s.bidirectional);
        w.member("reverse", s.reverse);
        w.member("region", region_of(s));
        w.end_object();
    }
    w.end_array();
}

std::string hex_of(const uint8_t *p, size_t n) {
    static constexpr char HEX[] = "0123456789abcdef";
    std::string s;
    for (size_t i = 0; i < n; ++i) {
        s += HEX[p[i] >> 4];
        s += HEX[p[i] & 15];
    }
    return s;
}

} // namespace

struct TraceRecorder::Impl {
    TraceOptions options;
    EngineQueries queries;

    // span limit
    uint64_t limit_tick = std::numeric_limits<uint64_t>::max();
    bool wrapped = false, ended = false;
    unsigned wraps = 0;
    std::vector<uint64_t> wrap_ticks;
    uint8_t wrap_owner = 0, wrap_resume = 0;

    std::vector<SectionRow> sections;
    std::array<int32_t, 256> open_section;
    std::vector<BarRow> bars;
    std::vector<RateRow> rates;
    std::vector<NoteRow> notes;
    std::unordered_map<uint64_t, uint32_t> note_of; // instance -> notes index
    std::vector<VoiceRow> voices;
    std::unordered_map<uint64_t, uint32_t> voice_of; // (instance << 8 | layer) -> latest voice row
    std::array<std::vector<uint32_t>, 32> slot_voices; // slot -> voice rows
    uint32_t open_voice_rows = 0;
    std::map<uint32_t, ChannelState> channels; // key = sequence << 8 | track
    std::vector<ChanEvent> chan_events;
    std::vector<EffectRow> effects;
    std::vector<GainRow> gains;
    // levels: per 512-frame window: 8 buses + 2 DAC channels
    std::vector<std::array<float, 10>> windows;

    Impl(const TraceOptions &o, EngineQueries q) : options(o), queries(std::move(q)) { open_section.fill(-1); }

    bool recording(uint64_t tick) const { return tick < limit_tick; }

    void diff_channel(uint8_t sequence, uint8_t track, const hle::Channel &c, uint64_t tick) {
        const uint32_t key = uint32_t(sequence) << 8 | track;
        ChannelState &state = channels[key];
        const uint32_t frame = to_frame(hle::Core::frames_at(tick));
        const auto now = named_lanes(c);
        const auto before = named_lanes(state.last);
        const auto raw = raw_lane_offsets();
        for (unsigned lane = 0; lane < CL_NAMED; ++lane)
            if (!state.seen || now[lane] != before[lane]) chan_events.push_back({frame, key, uint16_t(lane), now[lane]});
        for (unsigned off = CH_FIRST; off < CH_END; ++off)
            if (raw[off] && (state.seen ? c.parameters[off] != state.last.parameters[off] : c.parameters[off] != 0))
                chan_events.push_back({frame, key, uint16_t(CL_NAMED + off), c.parameters[off]});
        state.last = c;
        state.seen = true;
    }
};

TraceRecorder::TraceRecorder(const TraceOptions &options, EngineQueries queries)
    : impl_(std::make_unique<Impl>(options, std::move(queries))) {}
TraceRecorder::~TraceRecorder() = default;

bool TraceRecorder::wrapped() const { return impl_->wrapped; }
uint64_t TraceRecorder::wrap_tick() const { return impl_->limit_tick; }
bool TraceRecorder::song_ended() const { return impl_->ended; }
uint32_t TraceRecorder::open_voices() const { return impl_->open_voice_rows; }

void TraceRecorder::note_on(const hle::Note &note) {
    Impl &s = *impl_;
    if (!s.recording(note.tick)) return;
    NoteRow row;
    row.instance = note.instance;
    row.tick = note.tick;
    row.on = to_frame(hle::Core::frames_at(note.tick));
    row.sequence = note.sequence;
    row.track = note.track;
    row.key = note.key;
    row.kernel_key = note.kernel_key;
    row.velocity = note.velocity;
    row.program = note.channel.program;
    const int32_t section = s.open_section[note.sequence];
    row.section = section < 0 ? NO_SECTION : uint16_t(section);
    row.channel = uint32_t(note.sequence) << 8 | note.track;
    s.note_of[note.instance] = uint32_t(s.notes.size());
    s.notes.push_back(row);
    s.diff_channel(note.sequence, note.track, note.channel, note.tick);
}

void TraceRecorder::note_off(uint64_t instance, uint64_t tick) {
    Impl &s = *impl_;
    const auto it = s.note_of.find(instance);
    if (it == s.note_of.end()) return;
    NoteRow &n = s.notes[it->second];
    if (n.off == NONE) n.off = to_frame(hle::Core::frames_at(tick));
}

void TraceRecorder::channel_update(uint8_t sequence, uint8_t track, const hle::Channel &channel, uint64_t tick) {
    if (!impl_->recording(tick)) return;
    impl_->diff_channel(sequence, track, channel, tick);
}

void TraceRecorder::effect(uint8_t preset, uint64_t tick) {
    Impl &s = *impl_;
    if (!s.recording(tick)) return;
    const hle::EffectsInfo info = s.queries.effects ? s.queries.effects() : hle::EffectsInfo{};
    s.effects.push_back({to_frame(hle::Core::frames_at(tick)), false, preset, 0, 0, info});
}

void TraceRecorder::effect_parameter(uint8_t index, uint8_t value, uint64_t tick) {
    Impl &s = *impl_;
    if (!s.recording(tick)) return;
    const hle::EffectsInfo info = s.queries.effects ? s.queries.effects() : hle::EffectsInfo{};
    s.effects.push_back({to_frame(hle::Core::frames_at(tick)), true, 0, index, value, info});
}

void TraceRecorder::gain(unsigned channel, uint8_t raw, float linear, uint64_t tick) {
    Impl &s = *impl_;
    if (!s.recording(tick)) return;
    s.gains.push_back({to_frame(hle::Core::frames_at(tick)), uint8_t(channel), raw, linear});
}

void TraceRecorder::voice(const hle::VoiceEvent &e) {
    Impl &s = *impl_;
    auto note = s.note_of.find(e.instance);
    const uint32_t frame = to_frame(hle::Core::frames_at(e.tick));
    if (note == s.note_of.end()) {
        NoteRow n;
        n.instance = e.instance;
        n.tick = e.tick;
        n.on = frame;
        n.sequence = e.sequence;
        n.track = e.track;
        n.key = e.key;
        n.kernel_key = uint8_t(e.key + 21);
        n.velocity = 100;
        n.channel = uint32_t(e.sequence) << 8 | e.track;
        s.note_of[e.instance] = uint32_t(s.notes.size());
        s.notes.push_back(n);
        note = s.note_of.find(e.instance);
    }
    const uint64_t key = e.instance << 8 | e.layer;
    const auto row_it = s.voice_of.find(key);
    switch (e.kind) {
    case hle::VoiceEvent::Kind::Start: {
        if (!s.recording(e.tick)) return;
        VoiceRow row;
        row.note = note->second;
        row.layer = e.layer;
        row.slot = e.slot;
        row.pair = e.output_pair;
        row.skey = e.sounding_key;
        row.filter_mode = e.filter_mode;
        row.start = frame;
        row.start_tick = e.tick;
        row.sample_start = e.sample_start;
        row.sample_end = e.sample_end;
        row.loop = e.loop;
        row.reverse = e.reverse;
        row.onset = lane_values(e);
        row.order = uint32_t(s.voices.size());
        if (e.loop) row.flags |= VOICE_LOOP;
        if (e.reverse) row.flags |= VOICE_REVERSE;
        if (e.direct) row.flags |= VOICE_DIRECT;
        if (row_it != s.voice_of.end()) row.flags |= VOICE_RETRIGGER;
        s.voice_of[key] = uint32_t(s.voices.size());
        s.slot_voices[e.slot & 31].push_back(uint32_t(s.voices.size()));
        s.voices.push_back(std::move(row));
        ++s.open_voice_rows;
        break;
    }
    case hle::VoiceEvent::Kind::Release:
        if (note != s.note_of.end() && s.notes[note->second].off == NONE) s.notes[note->second].off = frame;
        if (row_it != s.voice_of.end() && s.voices[row_it->second].release == NONE)
            s.voices[row_it->second].release = frame;
        break;
    case hle::VoiceEvent::Kind::Stop:
        if (note != s.note_of.end() && s.notes[note->second].off == NONE) s.notes[note->second].off = frame;
        if (row_it != s.voice_of.end() && s.voices[row_it->second].stop == NONE) {
            s.voices[row_it->second].stop = frame;
            --s.open_voice_rows;
        }
        break;
    case hle::VoiceEvent::Kind::Parameters: {
        // Legato handoff (README 4.6): if no voice row is active for this note/layer, but an active voice
        // exists on e.slot from a previous note, hand it off.
        if (row_it == s.voice_of.end() || s.voices[row_it->second].stop != NONE) {
            uint32_t active = NONE;
            for (const uint32_t r : s.slot_voices[e.slot & 31]) {
                if (s.voices[r].stop == NONE) {
                    active = r;
                    break;
                }
            }
            if (active != NONE && s.voices[active].note != note->second) {
                s.voices[active].stop = frame;
                s.voices[active].flags |= VOICE_HANDOFF;
                --s.open_voice_rows;

                VoiceRow row;
                row.note = note->second;
                row.layer = e.layer;
                row.slot = e.slot;
                row.pair = s.voices[active].pair;
                row.skey = e.sounding_key;
                row.filter_mode = e.filter_mode;
                row.start = frame;
                row.start_tick = e.tick;
                row.sample_start = s.voices[active].sample_start;
                row.sample_end = s.voices[active].sample_end;
                row.loop = s.voices[active].loop;
                row.reverse = s.voices[active].reverse;
                row.onset = lane_values(e);
                row.order = uint32_t(s.voices.size());
                row.flags |= VOICE_LEGATO;
                s.voice_of[key] = uint32_t(s.voices.size());
                s.slot_voices[e.slot & 31].push_back(uint32_t(s.voices.size()));
                s.voices.push_back(std::move(row));
                ++s.open_voice_rows;
                return;
            }
            return;
        }
        VoiceRow &row = s.voices[row_it->second];
        if (row.stop != NONE) return;
        const LaneValues v = lane_values(e);
        if (frame <= row.start) {
            row.onset = v; // the last event at the start frame defines the onset values
        } else if (!row.samples.empty() && row.samples.back().frame == frame) {
            row.samples.back().value = v;
        } else {
            row.samples.push_back({frame, v});
        }
        break;
    }
    }
}

void TraceRecorder::mix_block(uint64_t first_frame, const float *buses8, const float *stereo, size_t frames,
                              const std::array<float, 2> &, const std::array<float, 2> &) {
    Impl &s = *impl_;
    for (size_t i = 0; i < frames; ++i) {
        const size_t window = size_t((first_frame + i) / WINDOW_FRAMES);
        if (window >= s.windows.size()) s.windows.resize(window + 1, std::array<float, 10>{});
        auto &peak = s.windows[window];
        for (size_t b = 0; b < 8; ++b) peak[b] = std::max(peak[b], std::fabs(buses8[i * 8 + b]));
        // DAC order: the single left/right exchange of the output stage
        peak[8] = std::max(peak[8], std::fabs(stereo[i * 2 + 1]));
        peak[9] = std::max(peak[9], std::fabs(stereo[i * 2]));
    }
}

void TraceRecorder::section_start(const hle::SectionInfo &info, uint64_t tick) {
    Impl &s = *impl_;
    if (!s.recording(tick)) return;
    SectionRow row;
    row.owner = info.owner;
    row.sequence = info.sequence;
    row.phase = info.phase;
    row.repeat = info.repeat_index;
    row.bars = info.bars;
    row.rate = info.rate;
    row.transpose = info.transpose;
    row.start_frame = to_frame(hle::Core::frames_at(tick));
    row.start_tick = tick;
    s.open_section[info.sequence] = int32_t(s.sections.size());
    s.sections.push_back(row);
}

void TraceRecorder::section_end(uint8_t sequence, hle::SectionEnd how, uint64_t tick) {
    Impl &s = *impl_;
    const int32_t index = s.open_section[sequence];
    if (index < 0) return;
    SectionRow &row = s.sections[size_t(index)];
    row.end_frame = to_frame(hle::Core::frames_at(tick));
    row.end_how = uint8_t(how);
    s.open_section[sequence] = -1;
}

void TraceRecorder::bar(uint8_t sequence, uint32_t bar_index, uint64_t tick) {
    Impl &s = *impl_;
    if (!s.recording(tick)) return;
    const int32_t section = s.open_section[sequence];
    s.bars.push_back({to_frame(hle::Core::frames_at(tick)), section < 0 ? NO_SECTION : uint16_t(section), bar_index});
}

void TraceRecorder::rate_change(uint8_t sequence, uint16_t rate, uint64_t tick) {
    Impl &s = *impl_;
    if (!s.recording(tick)) return;
    s.rates.push_back({to_frame(hle::Core::frames_at(tick)), sequence, rate});
}

void TraceRecorder::arrangement_wrap(uint8_t owner, uint8_t resume_phase, uint64_t tick) {
    Impl &s = *impl_;
    if (!s.recording(tick)) return;
    if (resume_phase == 0) {
        s.ended = true;
        return;
    }
    ++s.wraps;
    s.wrap_ticks.push_back(tick);
    s.wrap_owner = owner;
    s.wrap_resume = resume_phase;
    if (s.wraps >= s.options.loops) {
        s.wrapped = true;
        s.limit_tick = tick;
    }
}

// ---- finish ------------------------------------------------------------------------------------------------------

TraceResult TraceRecorder::finish(const SongMeta &meta, const SpanEnd &end, std::span<const uint16_t> sample_words) {
    Impl &s = *impl_;
    TraceResult result;
    result.frames = end.frames;
    result.audio_frames = end.audio_frames;
    result.ticks = end.ticks;
    result.end_reason = end.reason;
    const bool loop = end.reason == EndReason::LoopWrap && s.wrapped;
    result.loop = loop;

    // Notes and voices that fired at the wrap tick itself (other sequences) belong to the next iteration.
    if (s.wrapped) {
        for (NoteRow &n : s.notes) n.keep = n.tick < s.limit_tick;
    }
    std::vector<uint32_t> voice_order;
    for (uint32_t i = 0; i < s.voices.size(); ++i) {
        const VoiceRow &v = s.voices[i];
        if (!s.notes[v.note].keep || (s.wrapped && v.start_tick >= s.limit_tick)) continue;
        voice_order.push_back(i);
    }
    // notes: stable by on_frame (record order is chronological already); kept notes only
    std::vector<uint32_t> note_new(s.notes.size(), NONE);
    std::vector<uint32_t> note_list;
    for (uint32_t i = 0; i < s.notes.size(); ++i)
        if (s.notes[i].keep) {
            note_new[i] = uint32_t(note_list.size());
            note_list.push_back(i);
        }
    std::stable_sort(voice_order.begin(), voice_order.end(), [&](uint32_t a, uint32_t b) {
        const VoiceRow &x = s.voices[a], &y = s.voices[b];
        const uint32_t nx = note_new[x.note], ny = note_new[y.note];
        if (nx != ny) return nx < ny;
        if (x.layer != y.layer) return x.layer < y.layer;
        if (x.start != y.start) return x.start < y.start;
        return x.order < y.order;
    });

    // channels, numbered by (sequence, track)
    std::set<uint32_t> channel_keys;
    for (const uint32_t i : note_list) channel_keys.insert(s.notes[i].channel);
    for (const auto &[key, state] : s.channels) channel_keys.insert(key);
    std::map<uint32_t, uint16_t> channel_id;
    for (const uint32_t key : channel_keys) channel_id[key] = uint16_t(channel_id.size());

    // instruments: every program a note selects
    std::map<uint16_t, uint32_t> program_uses;
    for (const uint32_t i : note_list) ++program_uses[s.notes[i].program];
    std::vector<std::optional<hle::ProgramInfo>> programs;
    for (const auto &[id, uses] : program_uses)
        programs.push_back(s.queries.program ? s.queries.program(id) : std::nullopt);

    // regions: sample ranges of the instruments' splits and of the recorded voices
    std::map<Region, uint8_t> region_flags; // key ignores bidi; flags accumulate it
    auto add_region = [&](uint32_t start, uint32_t end_word, bool loop_flag, bool reverse, bool bidi) {
        Region key{start, end_word, uint8_t((loop_flag ? REGION_LOOP : 0) | (reverse ? REGION_REVERSE : 0))};
        region_flags[key] |= bidi ? REGION_BIDI : 0;
    };
    for (const auto &p : programs) {
        if (!p) continue;
        for (const auto &layer : p->layers)
            for (const auto &sp : layer.splits) add_region(sp.start_word, sp.end_word, sp.loop, sp.reverse, sp.bidirectional);
        for (const auto &reg : p->regions)
            for (const auto &sp : reg.splits) add_region(sp.start_word, sp.end_word, sp.loop, sp.reverse, sp.bidirectional);
    }
    for (const uint32_t i : voice_order) {
        const VoiceRow &v = s.voices[i];
        add_region(v.sample_start, v.sample_end, v.loop, v.reverse, false);
    }
    std::vector<Region> regions;
    std::map<Region, uint16_t> region_index;
    for (const auto &[key, bidi] : region_flags) {
        if (regions.size() >= 0xffff) throw std::runtime_error("too many sample regions");
        region_index[key] = uint16_t(regions.size());
        regions.push_back({key.start, key.end, uint8_t(key.flags | bidi)});
    }
    auto region_of = [&](uint32_t start, uint32_t end_word, bool loop_flag, bool reverse) {
        return region_index.at(Region{start, end_word, uint8_t((loop_flag ? REGION_LOOP : 0) | (reverse ? REGION_REVERSE : 0))});
    };

    // ---- JSON + binary
    json::Writer w;
    BinWriter bin;
    auto table = [&](size_t rows, auto &&columns) {
        w.begin_object();
        w.member("rows", rows);
        w.key("columns").begin_object();
        columns();
        w.end_object();
        w.end_object();
    };
    auto col = [&](const char *name, auto &&values) {
        w.key(name);
        bin.column(w, values);
    };

    w.begin_object();
    w.member("format", "f3-webplayer-trace");
    w.member("version", 1);
    w.member("set", meta.set);
    w.key("song").begin_object();
    w.member("id", meta.id);
    w.member("code", meta.code);
    if (meta.bank) w.member("bank", *meta.bank);
    else w.member_null("bank");
    if (meta.page) w.member("page", *meta.page);
    else w.member_null("page");
    w.member("label", meta.label);
    w.end_object();
    w.key("time").begin_object();
    w.member("sample_rate", hle::SAMPLE_RATE);
    w.member("main_clock", hle::MAIN_CLOCK);
    w.member("timer_ticks", hle::TIMER_TICKS);
    w.member("pulses_per_quarter", 96);
    w.end_object();
    w.key("span").begin_object();
    w.member("frames", end.frames);
    w.member("audio_frames", end.audio_frames);
    w.member("ticks", end.ticks);
    w.member("end_reason", end_reason_name(end.reason));
    w.member("loops", s.options.loops);
    w.end_object();

    if (loop) {
        uint64_t start_tick = 0;
        uint32_t start_frame = 0;
        bool found = false;
        for (const SectionRow &r : s.sections)
            if (r.owner == s.wrap_owner && r.phase == s.wrap_resume) {
                start_tick = r.start_tick;
                start_frame = r.start_frame;
                found = true;
                break;
            }
        if (!found) {
            if (s.wrap_ticks.size() >= 2) {
                const uint64_t period = s.wrap_ticks[1] - s.wrap_ticks[0];
                start_tick = s.wrap_ticks[0] >= period ? s.wrap_ticks[0] - period : 0;
                start_frame = to_frame(hle::Core::frames_at(start_tick));
                found = true;
            } else {
                // If resume_phase > 1, the intro has (resume_phase - 1) arrangement sections before wrap 1.
                std::vector<const SectionRow *> arr_sections;
                for (const auto &r : s.sections)
                    if (r.start_tick < s.wrap_ticks.front() && r.sequence > 2)
                        arr_sections.push_back(&r);
                if (s.wrap_resume > 1 && s.wrap_resume - 1 < arr_sections.size()) {
                    start_tick = arr_sections[s.wrap_resume - 1]->start_tick;
                    start_frame = arr_sections[s.wrap_resume - 1]->start_frame;
                    found = true;
                }
            }
        }
        (void)found;
        const uint64_t first_wrap = s.wrap_ticks.front();
        w.key("loop").begin_object();
        w.member("start_frame", start_frame);
        w.member("end_frame", to_frame(hle::Core::frames_at(first_wrap)));
        w.member("start_tick", start_tick);
        w.member("end_tick", first_wrap);
        w.member("length_ticks", first_wrap - start_tick);
        w.member("resume_phase", s.wrap_resume);
        w.member("iterations", s.options.loops);
        w.end_object();
    } else {
        w.member_null("loop");
    }
    w.key("synth").begin_object();
    w.member("bend_range", s.queries.bend_range);
    w.end_object();

    // sequences
    std::set<uint8_t> sequence_ids;
    for (const SectionRow &r : s.sections) sequence_ids.insert(r.sequence), sequence_ids.insert(r.owner);
    for (const uint32_t i : note_list) sequence_ids.insert(s.notes[i].sequence);
    w.key("sequences").begin_array(true);
    for (const uint8_t id : sequence_ids) {
        const auto info = s.queries.sequence ? s.queries.sequence(id) : std::nullopt;
        if (!info) continue;
        w.begin_object();
        w.member("id", info->id);
        w.member("arranged", info->arranged);
        w.member("arrangement_entries", info->arrangement_entries);
        w.member("bars", info->bars);
        w.member("numerator", info->numerator);
        w.member("exponent", info->exponent);
        w.member("ticks_per_bar", info->ticks_per_bar);
        w.member("rate", info->rate);
        w.member("flags", info->flags);
        w.member("linked", info->linked);
        w.key("header_loop").begin_object();
        w.member("start", info->header_loop_start);
        w.member("end", info->header_loop_end);
        w.member("used_by_hle", false);
        w.end_object();
        w.key("tracks").begin_array();
        for (const auto &t : info->tracks) {
            w.begin_object();
            w.member("track", t.track);
            w.member("events", t.events);
            w.member("initial_program", t.initial_program);
            w.end_object();
        }
        w.end_array();
        w.end_object();
    }
    w.end_array();

    // sections
    w.key("sections");
    table(s.sections.size(), [&] {
        std::vector<uint8_t> owner, sequence, phase, end_how;
        std::vector<uint16_t> repeat, bars, rate;
        std::vector<int16_t> transpose;
        std::vector<uint32_t> start_frame, end_frame;
        for (const SectionRow &r : s.sections) {
            owner.push_back(r.owner);
            sequence.push_back(r.sequence);
            phase.push_back(r.phase);
            repeat.push_back(r.repeat);
            start_frame.push_back(r.start_frame);
            // sections still playing when the span ends are cut there
            end_frame.push_back(r.end_frame == NONE ? to_frame(end.frames) : r.end_frame);
            end_how.push_back(r.end_how);
            rate.push_back(r.rate);
            bars.push_back(r.bars);
            transpose.push_back(r.transpose);
        }
        col("owner", owner);
        col("sequence", sequence);
        col("phase", phase);
        col("repeat", repeat);
        col("start_frame", start_frame);
        col("end_frame", end_frame);
        col("end_how", end_how);
        col("rate", rate);
        col("bars", bars);
        col("transpose", transpose);
    });
    w.key("bars");
    table(s.bars.size(), [&] {
        std::vector<uint32_t> frame, bar;
        std::vector<uint16_t> section;
        for (const BarRow &r : s.bars) {
            frame.push_back(r.frame);
            section.push_back(r.section);
            bar.push_back(r.bar);
        }
        col("frame", frame);
        col("section", section);
        col("bar", bar);
    });
    w.key("rate_changes");
    table(s.rates.size(), [&] {
        std::vector<uint32_t> frame;
        std::vector<uint8_t> sequence;
        std::vector<uint16_t> rate;
        for (const RateRow &r : s.rates) {
            frame.push_back(r.frame);
            sequence.push_back(r.sequence);
            rate.push_back(r.rate);
        }
        col("frame", frame);
        col("sequence", sequence);
        col("rate", rate);
    });
    w.key("channels").begin_array();
    for (const auto &[key, id] : channel_id) {
        w.begin_object();
        w.member("id", id);
        w.member("sequence", key >> 8);
        w.member("track", key & 0xff);
        w.end_object();
    }
    w.end_array();

    // voices per note
    std::vector<uint32_t> first_voice(note_list.size(), 0);
    std::vector<uint32_t> voice_count(note_list.size(), 0);
    for (uint32_t row = 0; row < voice_order.size(); ++row) {
        const uint32_t n = note_new[s.voices[voice_order[row]].note];
        if (voice_count[n] == 0) first_voice[n] = row;
        ++voice_count[n];
    }

    w.key("notes");
    table(note_list.size(), [&] {
        std::vector<uint32_t> on, off, voice_first;
        std::vector<uint8_t> sequence, track, key, kkey, vel, count;
        std::vector<uint16_t> program, section, channel;
        for (uint32_t n = 0; n < note_list.size(); ++n) {
            const NoteRow &r = s.notes[note_list[n]];
            on.push_back(r.on);
            off.push_back(r.off);
            sequence.push_back(r.sequence);
            track.push_back(r.track);
            key.push_back(r.key);
            kkey.push_back(r.kernel_key);
            vel.push_back(r.velocity);
            program.push_back(r.program);
            section.push_back(r.section);
            voice_first.push_back(first_voice[n]);
            if (voice_count[n] > 255) throw std::runtime_error("a note owns more than 255 voice rows");
            count.push_back(uint8_t(voice_count[n]));
            channel.push_back(channel_id.at(r.channel));
        }
        col("on_frame", on);
        col("off_frame", off);
        col("sequence", sequence);
        col("track", track);
        col("key", key);
        col("kkey", kkey);
        col("vel", vel);
        col("program", program);
        col("section", section);
        col("voice_first", voice_first);
        col("voice_count", count);
        col("channel", channel);
    });

    // lane simplification
    struct LaneOut {
        std::vector<uint16_t> dt;
        std::vector<int32_t> value;
    };
    std::array<LaneOut, LANE_COUNT> lane_out;
    std::vector<std::array<uint32_t, LANE_COUNT>> lane_first(voice_order.size());
    std::vector<std::array<uint16_t, LANE_COUNT>> lane_count(voice_order.size());
    for (uint32_t row = 0; row < voice_order.size(); ++row) {
        const VoiceRow &v = s.voices[voice_order[row]];
        for (unsigned lane = 0; lane < LANE_COUNT; ++lane) {
            std::vector<LanePoint> samples;
            samples.reserve(v.samples.size());
            for (const ParamSample &p : v.samples) samples.push_back({p.frame, p.value[lane]});
            const int32_t lo = LANES[lane].is_signed ? INT16_MIN : 0;
            const int32_t hi = LANES[lane].is_signed ? INT16_MAX : UINT16_MAX;
            if (s.options.keep_raw_lanes) {
                RawVoiceLane raw;
                raw.voice = row;
                raw.lane = LANES[lane].name;
                raw.start_frame = v.start;
                raw.onset = v.onset[lane];
                for (const LanePoint &p : samples) raw.samples.emplace_back(p.frame, p.value);
                result.raw_lanes.push_back(std::move(raw));
            }
            const auto points = simplify_lane(v.start, v.onset[lane], samples, LANES[lane].tolerance, lo, hi);
            lane_first[row][lane] = uint32_t(lane_out[lane].dt.size());
            uint32_t prev_frame = v.start;
            int32_t prev_value = v.onset[lane];
            uint32_t count = 0;
            for (const LanePoint &p : points) {
                // dt is 16 bits: a longer segment gets interpolated intermediate points
                while (p.frame - prev_frame > 0xffffu) {
                    const double t = 65535.0 / double(p.frame - prev_frame);
                    const int32_t mid = int32_t(std::lround(prev_value + (p.value - prev_value) * t));
                    prev_frame += 0xffffu;
                    prev_value = mid;
                    lane_out[lane].dt.push_back(0xffffu);
                    lane_out[lane].value.push_back(mid);
                    ++count;
                }
                lane_out[lane].dt.push_back(uint16_t(p.frame - prev_frame));
                lane_out[lane].value.push_back(p.value);
                prev_frame = p.frame;
                prev_value = p.value;
                ++count;
            }
            if (count > 0xffffu) throw std::runtime_error("a voice lane holds more than 65535 points");
            lane_count[row][lane] = uint16_t(count);
            result.counts.lane_points += count;
        }
    }

    w.key("voices");
    table(voice_order.size(), [&] {
        std::vector<uint32_t> note, start, release, stop;
        std::vector<uint8_t> layer, slot, pair, flags, skey, filter_mode;
        std::vector<uint16_t> region;
        std::array<std::vector<int32_t>, LANE_COUNT> onset;
        for (uint32_t row = 0; row < voice_order.size(); ++row) {
            const VoiceRow &v = s.voices[voice_order[row]];
            note.push_back(note_new[v.note]);
            layer.push_back(v.layer);
            slot.push_back(v.slot);
            pair.push_back(v.pair);
            const uint16_t reg = region_of(v.sample_start, v.sample_end, v.loop, v.reverse);
            uint8_t f = v.flags;
            if (regions[reg].flags & REGION_BIDI) f |= VOICE_BIDI;
            flags.push_back(f);
            start.push_back(v.start);
            release.push_back(v.release);
            stop.push_back(v.stop);
            skey.push_back(v.skey);
            filter_mode.push_back(v.filter_mode);
            region.push_back(reg);
            for (unsigned lane = 0; lane < LANE_COUNT; ++lane) onset[lane].push_back(v.onset[lane]);
        }
        col("note", note);
        col("layer", layer);
        col("slot", slot);
        col("pair", pair);
        col("flags", flags);
        col("start_frame", start);
        col("release_frame", release);
        col("stop_frame", stop);
        col("skey", skey);
        col("filter_mode", filter_mode);
        col("region", region);
        for (unsigned lane = 0; lane < LANE_COUNT; ++lane) {
            const std::string name = std::string(LANES[lane].name) + "0";
            w.key(name);
            if (LANES[lane].is_signed) {
                std::vector<int16_t> v16;
                for (const int32_t x : onset[lane]) v16.push_back(int16_t(x));
                bin.column(w, v16);
            } else {
                std::vector<uint16_t> v16;
                for (const int32_t x : onset[lane]) v16.push_back(uint16_t(x));
                bin.column(w, v16);
            }
        }
        for (unsigned lane = 0; lane < LANE_COUNT; ++lane) {
            std::vector<uint32_t> first;
            std::vector<uint16_t> count;
            for (uint32_t row = 0; row < voice_order.size(); ++row) {
                first.push_back(lane_first[row][lane]);
                count.push_back(lane_count[row][lane]);
            }
            col((std::string(LANES[lane].name) + "_first").c_str(), first);
            col((std::string(LANES[lane].name) + "_count").c_str(), count);
        }
    });

    w.key("lanes").begin_object();
    for (unsigned lane = 0; lane < LANE_COUNT; ++lane) {
        w.key(LANES[lane].name);
        table(lane_out[lane].dt.size(), [&] {
            col("dt", lane_out[lane].dt);
            w.key("value");
            if (LANES[lane].is_signed) {
                std::vector<int16_t> v16(lane_out[lane].value.begin(), lane_out[lane].value.end());
                bin.column(w, v16);
            } else {
                std::vector<uint16_t> v16(lane_out[lane].value.begin(), lane_out[lane].value.end());
                bin.column(w, v16);
            }
        });
    }
    w.end_object();

    // channel automation: stable by frame; fades (seq_volume) are stored as linear segments
    std::vector<ChanEvent> events = s.chan_events;
    std::stable_sort(events.begin(), events.end(), [](const ChanEvent &a, const ChanEvent &b) { return a.frame < b.frame; });
    {
        // fade runs of seq_volume per channel
        std::map<uint32_t, std::vector<size_t>> fade; // channel -> indices into events
        for (size_t i = 0; i < events.size(); ++i)
            if (events[i].lane == CL_SEQ_VOLUME) fade[events[i].channel].push_back(i);
        std::vector<bool> drop(events.size(), false);
        for (const auto &[channel, indices] : fade) {
            size_t i = 0;
            while (i < indices.size()) {
                size_t j = i + 1;
                while (j < indices.size() && events[indices[j]].frame - events[indices[j - 1]].frame <= FADE_GAP_FRAMES &&
                       events[indices[j]].frame > events[indices[j - 1]].frame)
                    ++j;
                if (j - i >= 3) {
                    std::vector<LanePoint> samples;
                    for (size_t k = i + 1; k < j; ++k) samples.push_back({events[indices[k]].frame, events[indices[k]].value});
                    const auto kept = simplify_lane(events[indices[i]].frame, events[indices[i]].value, samples, FADE_TOLERANCE, 0, 127);
                    for (size_t k = i + 1; k < j; ++k) drop[indices[k]] = true;
                    for (const LanePoint &p : kept) {
                        // reuse the dropped slot of the event on that frame
                        for (size_t k = i + 1; k < j; ++k)
                            if (events[indices[k]].frame == p.frame) {
                                events[indices[k]].value = p.value;
                                drop[indices[k]] = false;
                            }
                    }
                }
                i = j;
            }
        }
        std::vector<ChanEvent> kept;
        for (size_t i = 0; i < events.size(); ++i)
            if (!drop[i]) kept.push_back(events[i]);
        events = std::move(kept);
    }
    std::set<unsigned> raw_offsets;
    for (const ChanEvent &e : events)
        if (e.lane >= CL_NAMED) raw_offsets.insert(unsigned(e.lane - CL_NAMED));
    std::map<unsigned, unsigned> raw_lane_id;
    std::vector<std::string> lane_names(CHAN_LANE_NAMES, CHAN_LANE_NAMES + CL_NAMED);
    for (const unsigned off : raw_offsets) {
        raw_lane_id[off] = unsigned(lane_names.size());
        char buf[16];
        std::snprintf(buf, sizeof buf, "raw_%02x", off);
        lane_names.push_back(buf);
    }
    w.key("chan_events");
    table(events.size(), [&] {
        std::vector<uint32_t> frame;
        std::vector<uint16_t> channel;
        std::vector<uint8_t> lane;
        std::vector<int32_t> value;
        for (const ChanEvent &e : events) {
            frame.push_back(e.frame);
            channel.push_back(channel_id.at(e.channel));
            lane.push_back(uint8_t(e.lane >= CL_NAMED ? raw_lane_id.at(unsigned(e.lane - CL_NAMED)) : e.lane));
            value.push_back(e.value);
        }
        col("frame", frame);
        col("channel", channel);
        col("lane", lane);
        col("value", value);
    });
    result.counts.chan_events = uint32_t(events.size());
    w.key("chan_lanes").begin_array();
    for (const auto &name : lane_names) w.value(name);
    w.end_array();
    w.key("chan_lanes_linear").begin_array();
    w.value("seq_volume");
    w.end_array();

    // levels
    const size_t level_rows = (size_t(end.frames) + WINDOW_FRAMES - 1) / WINDOW_FRAMES;
    s.windows.resize(std::max(s.windows.size(), level_rows));
    w.key("levels").begin_object();
    w.member("window_frames", WINDOW_FRAMES);
    w.member("rows", level_rows);
    {
        std::vector<uint8_t> buses, out;
        for (size_t r = 0; r < level_rows; ++r) {
            for (size_t b = 0; b < 8; ++b) buses.push_back(level_byte(s.windows[r][b]));
            out.push_back(level_byte(s.windows[r][8]));
            out.push_back(level_byte(s.windows[r][9]));
        }
        w.key("buses");
        bin.column(w, buses);
        w.key("out");
        bin.column(w, out);
    }
    w.member("encoding", "u8 = clamp(round(255 + 2.5 * dB), 0, 255), peak of the window");
    w.member("out_order", "DAC (left/right exchanged relative to OTIS/ESP)");
    w.end_object();

    // effects, gain
    w.key("effects").begin_array(true);
    for (const EffectRow &e : s.effects) {
        w.begin_object();
        w.member("frame", e.frame);
        w.member("kind", e.param ? "param" : "preset");
        if (e.param) {
            w.member("index", e.index);
            w.member("value", e.value);
        } else {
            w.member("preset", e.preset);
        }
        w.member("program", program_name(e.info.program));
        w.key("p").begin_array();
        for (const uint8_t p : e.info.p) w.value(p);
        w.end_array();
        w.key("info").begin_object();
        w.member("decay", e.info.decay);
        w.member("damp", e.info.damp);
        w.member("feedback", e.info.feedback);
        w.member("send", e.info.send);
        w.member("dry_send", e.info.dry_send);
        w.member("chorus_to_reverb", e.info.chorus_to_reverb);
        w.member("chorus_out", e.info.chorus_out);
        w.member("lfo_a", e.info.lfo_a);
        w.member("lfo_b", e.info.lfo_b);
        w.member("lfo_c", e.info.lfo_c);
        w.member("lfo_depth_frames", e.info.lfo_depth_frames);
        w.member("echo_frames", e.info.echo_frames);
        w.member("mod_center", e.info.mod_center);
        w.member("enhance", e.info.enhance);
        w.member("hall_a", e.info.hall_a);
        w.member("hall_g", e.info.hall_g);
        w.member("hall_g2", e.info.hall_g2);
        w.member("hall_g108", e.info.hall_g108);
        w.member("bypassed", e.info.bypassed);
        w.end_object();
        w.end_object();
    }
    w.end_array();
    w.key("gain").begin_array(true);
    for (const GainRow &g : s.gains) {
        w.begin_object();
        w.member("frame", g.frame);
        w.member("channel", g.channel);
        w.member("raw", g.raw);
        w.member("linear", g.linear);
        w.end_object();
    }
    w.end_array();

    // mixer (README 4.11)
    w.key("mixer").begin_object();
    w.member("output_boost", hle::OUTPUT_BOOST);
    w.member("input_gain", 0.18);
    w.member("gain_model", "SingleStage");
    w.member("dc_blocker_hz", 20);
    w.key("pairs").begin_array(true);
    struct PairDesc {
        int pair;
        int bus_l, bus_r;
        const char *role;
    };
    for (const PairDesc &p : {PairDesc{0, 0, 1, "ES5505 pair 0: not connected, silent"},
                              PairDesc{1, 2, 3, "ES5510 serio0: dry in every program"},
                              PairDesc{2, 4, 5, "ES5510 serio1: reverb input (Reverb/Hall), stereo chorus (Chorus), ping-pong echo (Echo)"},
                              PairDesc{3, 6, 7, "ES5510 serio2: reverb send"}}) {
        w.begin_object();
        w.member("pair", p.pair);
        w.key("buses").begin_array();
        w.value(p.bus_l).value(p.bus_r);
        w.end_array();
        w.member("role", p.role);
        w.end_object();
    }
    w.end_array();
    w.key("programs").begin_object();
    struct ProgramDesc {
        const char *name, *description, *parameters;
    };
    for (const ProgramDesc &p :
         {ProgramDesc{"None", "no preset selected", ""},
          ProgramDesc{"Reverb", "dry + reverb taps (decoded ES5510 program)",
                      "p0 decay, p1 damping, p2 LFO rate, p3 LFO depth, p6 high nibble input, p7 all-pass gain, p8/p9 return gains"},
          ProgramDesc{"Hall", "Reverb with cross-coupled chains", "as Reverb"},
          ProgramDesc{"Chorus", "four-tap stereo chorus", "p0 decay, p1 feedback, p2 LFO rates, p6 mix"},
          ProgramDesc{"Echo", "ping-pong echo", "p1 feedback, p2 delay (x112 frames), p6 mix/enhancer"},
          ProgramDesc{"Mute", "outputs nothing", ""}}) {
        w.key(p.name).begin_object();
        w.member("description", p.description);
        w.member("parameters", p.parameters);
        w.end_object();
    }
    w.end_object();
    w.end_object();

    // instruments
    w.key("instruments").begin_array(true);
    {
        size_t index = 0;
        for (const auto &[id, uses] : program_uses) {
            const auto &p = programs[index++];
            w.begin_object();
            w.member("program", id);
            w.member("used_by_notes", uses);
            if (!p || p->kind == hle::ProgramInfo::Kind::Absent) {
                w.member("kind", "absent");
                w.member("glide_rate", 0);
                w.key("layers").begin_array();
                w.end_array();
                w.end_object();
                continue;
            }
            w.member("kind", p->kind == hle::ProgramInfo::Kind::Layered ? "layered" : "direct");
            w.member("enabled_mask", p->enabled_mask);
            w.member("glide_rate", p->glide_rate);
            auto split_region = [&](const hle::SplitInfo &sp) {
                return region_of(sp.start_word, sp.end_word, sp.loop, sp.reverse);
            };
            w.key("layers").begin_array(true);
            if (p->kind == hle::ProgramInfo::Kind::Layered) {
                for (const auto &l : p->layers) {
                    w.begin_object();
                    w.member("index", l.index);
                    w.member("enabled", l.enabled);
                    w.member("raw", hex_of(l.raw.data(), l.raw.size()));
                    w.key("envelope").begin_array(true);
                    for (const auto &e : l.envelope) write_envelope(w, e);
                    w.end_array();
                    w.member("base_pitch", l.base_pitch);
                    w.member("base_fine", l.base_fine);
                    w.member("use_played_key", l.use_played_key);
                    w.member("pitch_envelope_depth", l.pitch_envelope_depth);
                    w.member("pitch_envelope_offset", l.pitch_envelope_offset);
                    w.member("lfo_pitch_depth", l.lfo_pitch_depth);
                    w.member("pitch_source", l.pitch_source);
                    w.member("allocation_mode", l.allocation_mode);
                    w.member("filter_mode", l.filter_mode);
                    w.member("k2_bank", l.k2_bank);
                    w.member("pitch_source_depth", l.pitch_source_depth);
                    w.member("k1_base", l.k1_base);
                    w.member("k1_key", l.k1_key);
                    w.member("k1_envelope", l.k1_envelope);
                    w.member("filter_source", l.filter_source);
                    w.member("default_pan", l.default_pan);
                    w.member("filter_source_depth", l.filter_source_depth);
                    w.member("k2_base", l.k2_base);
                    w.member("k2_apply_source", l.k2_apply_source);
                    w.member("k2_key", l.k2_key);
                    w.member("k2_envelope", l.k2_envelope);
                    w.member("weight_amount", l.weight_amount);
                    w.member("weight_low", l.weight_low);
                    w.member("weight_high", l.weight_high);
                    w.member("level", l.level);
                    w.member("level_plus_43", l.level_plus_43);
                    w.member("amplitude_source", l.amplitude_source);
                    w.member("pair_choice", l.pair_choice);
                    w.member("priority", l.priority);
                    w.member("amplitude_source_depth", l.amplitude_source_depth);
                    w.member("lfo_source", l.lfo_source);
                    w.member("random_period", l.random_period);
                    w.member("lfo_depth", l.lfo_depth);
                    w.member("lfo_phase_reset", l.lfo_phase_reset);
                    w.member("lfo_rate_index", l.lfo_rate_index);
                    w.member("lfo_wave", l.lfo_wave);
                    w.member("lfo_fade_rate", l.lfo_fade_rate);
                    w.member("sample_id", l.sample_id);
                    w.member("reverse", l.reverse);
                    w.member("velocity_gate", l.velocity_gate);
                    w.member("start_source", l.start_source);
                    w.member("start_amount", l.start_amount);
                    w.member("start_offset", l.start_offset);
                    w.member("delay_ticks", l.delay_ticks);
                    write_splits(w, l.splits, split_region);
                    w.end_object();
                }
            } else {
                for (const auto &r : p->regions) {
                    w.begin_object();
                    w.member("index", r.index);
                    w.member("raw", hex_of(r.raw.data(), r.raw.size()));
                    w.member("key_low", r.key_low);
                    w.member("key_high", r.key_high);
                    w.member("sample_id", r.sample_id);
                    w.member("reverse", r.reverse);
                    w.member("transposition", r.transposition);
                    w.member("fine_pitch", r.fine_pitch);
                    w.member("sensitivity", r.sensitivity);
                    w.member("decay_rate", r.decay_rate);
                    w.member("ignore_note_off", r.ignore_note_off);
                    w.member("release_rate", r.release_rate);
                    w.member("root_key_relative", r.root_key_relative);
                    w.member("filter_index", r.filter_index);
                    w.member("level", r.level);
                    w.member("level_plus_168", r.level_plus_168);
                    w.member("pan", r.pan);
                    w.member("pair_choice", r.pair_choice);
                    w.member("velocity_curve", r.velocity_curve);
                    write_splits(w, r.splits, split_region);
                    w.end_object();
                }
            }
            w.end_array();
            w.end_object();
        }
    }
    w.end_array();

    // regions + overviews
    w.key("regions");
    table(regions.size(), [&] {
        std::vector<uint32_t> start, end_word, peak_offset;
        std::vector<uint8_t> flags;
        for (size_t i = 0; i < regions.size(); ++i) {
            start.push_back(regions[i].start);
            end_word.push_back(regions[i].end);
            flags.push_back(regions[i].flags);
            peak_offset.push_back(uint32_t(i * REGION_COLUMNS * 2));
        }
        col("start", start);
        col("end", end_word);
        col("flags", flags);
        col("peak_offset", peak_offset);
    });
    {
        std::vector<int8_t> peaks(regions.size() * REGION_COLUMNS * 2, 0);
        for (size_t r = 0; r < regions.size(); ++r) {
            const uint64_t first = std::min<uint64_t>(regions[r].start, sample_words.size());
            const uint64_t last = std::min<uint64_t>(std::max(regions[r].end, regions[r].start), sample_words.size());
            const uint64_t length = last - first;
            if (length == 0) continue;
            for (size_t c = 0; c < REGION_COLUMNS; ++c) {
                uint64_t a = first + length * c / REGION_COLUMNS;
                uint64_t b = first + length * (c + 1) / REGION_COLUMNS;
                if (b <= a) b = a + 1;
                if (b > last) {
                    b = last;
                    a = std::min(a, b - 1);
                }
                int8_t lo = INT8_MAX, hi = INT8_MIN;
                for (uint64_t i = a; i < b; ++i) {
                    const int8_t v = int8_t(sample_words[i] >> 8); // the sample ROM's low byte is zero: 8-bit PCM
                    lo = std::min(lo, v);
                    hi = std::max(hi, v);
                }
                peaks[(r * REGION_COLUMNS + c) * 2] = lo;
                peaks[(r * REGION_COLUMNS + c) * 2 + 1] = hi;
            }
        }
        w.key("peaks");
        bin.column(w, peaks);
    }
    w.end_object();

    result.counts.sections = uint32_t(s.sections.size());
    result.counts.notes = uint32_t(note_list.size());
    result.counts.voices = uint32_t(voice_order.size());
    result.counts.instruments = uint32_t(program_uses.size());
    result.counts.regions = uint32_t(regions.size());
    result.counts.channels = uint32_t(channel_id.size());
    result.json = w.take();
    result.bin = bin.finish();
    return result;
}
} // namespace f3rt::webplayer
