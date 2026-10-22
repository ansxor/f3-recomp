#include "audio/hle/sequencer.hpp"
#include "audio/hle/rom_view.hpp"
#include "audio/hle/sound_rom_5a7e9117.hpp"
#include "f3rt/memory_map.hpp"
#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace f3rt::hle {
namespace {

constexpr uint32_t ROM_BASE = SOUND_ROM_BASE;
constexpr uint32_t STREAM_BASE = sound_5a7e9117::SEQ_STREAM_BASE;

// Song track stream layout
constexpr size_t CONDUCTOR_STREAM_INDEX = 0;
constexpr size_t FIRST_MUSIC_STREAM_INDEX = 1;
constexpr size_t ARRANGEMENT_STREAM_INDEX = 10;
constexpr size_t TOTAL_SONG_STREAMS = 11;

// Packet / Mailbox opcodes
constexpr uint8_t CMD_EFFECT_PRESET = 0x20;
constexpr uint8_t CMD_EFFECT_PARAM = 0x21;
constexpr uint8_t CMD_OP_MIN = 0x80;
constexpr uint8_t CMD_OP_MAX = 0x90;
constexpr uint8_t CMD_START = 0x80;
constexpr uint8_t CMD_START_REPEAT = 0x81;
constexpr uint8_t CMD_STOP = 0x82;
constexpr uint8_t CMD_PAUSE = 0x83;
constexpr uint8_t CMD_RESUME = 0x84;
constexpr uint8_t CMD_SEEK = 0x85;
constexpr uint8_t CMD_SEQUENCE_VOLUME = 0x86;
constexpr uint8_t CMD_SEQUENCE_PAN = 0x87;
constexpr uint8_t CMD_SEQUENCE_RATE = 0x88;
constexpr uint8_t CMD_SEQUENCE_FADE = 0x89;
constexpr uint8_t CMD_TRACK_VOLUME = 0x8a;
constexpr uint8_t CMD_TRACK_PAN = 0x8b;
constexpr uint8_t CMD_TRACK_PARAM = 0x8c;
constexpr uint8_t CMD_TRACK_PROGRAM = 0x8d;
constexpr uint8_t CMD_DIRECT_NOTE_ON = 0x8e;
constexpr uint8_t CMD_DIRECT_NOTE_OFF = 0x8f;
constexpr uint8_t CMD_NOP = 0x90;

// Sequence track event opcodes
constexpr uint8_t EV_NOTE_SHORT_MAX = 0x58;
constexpr uint8_t EV_NOTE_LONG_MAX = 0xb0;
constexpr uint8_t EV_NOTE_KEY_OFFSET = 0x58;
constexpr uint8_t EV_CONTROLLER_MIN = 0xb0;
constexpr uint8_t EV_CONTROLLER_MAX = 0xd9;
constexpr uint8_t EV_PROGRAM_SELECTOR = 0xd9;
constexpr uint8_t EV_SPECIAL_TRACK_DA = 0xda;
constexpr uint8_t EV_SPECIAL_TRACK_DB = 0xdb;
constexpr uint8_t EV_RELATIVE_NOTE = 0xdc;
constexpr uint8_t EV_DELAY = 0xe6;
constexpr uint8_t EV_ARRANGEMENT = 0xe7;
constexpr uint8_t EV_NOTIFICATION = 0xe8;
constexpr uint8_t EV_END = 0xe9;

// Arrangement marker
constexpr uint16_t ARRANGEMENT_LOOP_MARKER = 100;

[[noreturn]] inline void invalid(const char *message) { throw std::runtime_error(message); }

constexpr uint8_t expected_sequence_command_length(uint8_t op) noexcept {
    switch (op) {
    case CMD_START:
    case CMD_START_REPEAT:
    case CMD_STOP:
    case CMD_PAUSE:
    case CMD_RESUME:
        return 3;
    case CMD_SEQUENCE_VOLUME:
    case CMD_SEQUENCE_PAN:
    case CMD_SEQUENCE_RATE:
        return 4;
    case CMD_SEEK:
    case CMD_TRACK_VOLUME:
    case CMD_TRACK_PAN:
    case CMD_DIRECT_NOTE_OFF:
        return 5;
    case CMD_TRACK_PARAM:
    case CMD_TRACK_PROGRAM:
    case CMD_DIRECT_NOTE_ON:
    case CMD_NOP:
        return 6;
    case CMD_SEQUENCE_FADE:
        return 7;
    default:
        return 0;
    }
}

// =============================================================================
// Pure Bit-Decoding Helpers
// =============================================================================

constexpr uint8_t decode_short_note_velocity(uint16_t operand) noexcept {
    const auto velocity = uint8_t((operand >> 8) & 0x7c);
    return velocity ? uint8_t(velocity | (velocity >> 5)) : 1;
}

constexpr uint8_t decode_long_note_velocity(uint16_t operand) noexcept {
    const auto velocity = uint8_t(operand & 0x7f);
    return velocity ? velocity : 1;
}

constexpr int16_t decode_arrangement_transpose(uint16_t word3) noexcept {
    return static_cast<int16_t>(int16_t(uint16_t(word3 * 2)) >> 11);
}

// =============================================================================
// Static ROM Data Structures & Parser
// =============================================================================

struct StreamEvent {
    uint32_t address = 0;
    uint32_t delay = 0;
    uint16_t operand = 0;
    uint16_t duration = 0;
    uint8_t opcode = 0;
    uint8_t key = 0;
    uint8_t velocity = 0;
    std::array<uint16_t, 4> arrangement{};
};

struct TrackData {
    std::vector<StreamEvent> events;
};

struct SongData {
    bool present = false;
    uint8_t flags = 0;
    uint8_t rate = 0;
    uint8_t numerator = 0;
    uint8_t exponent = 0;
    uint16_t bars = 0;
    uint16_t ticks_per_bar = 0;
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
    std::array<TrackConfig, TRACK_COUNT> initial{};
    std::array<TrackData, TOTAL_SONG_STREAMS> tracks{};
    uint8_t linked = 0;
};

TrackData parse_track_stream(const RomView &rom, uint32_t p, uint32_t end) {
    TrackData out;
    bool finished = false;

    while (p + 2 <= end) {
        StreamEvent e;
        e.address = p;
        const uint16_t token = rom.read_u16(p);
        p += 2;

        if (!(token & 0x8000)) {
            continue;
        }

        e.opcode = uint8_t(token);
        e.delay = (token >> 8) & 0x7f;

        auto read_operand = [&]() {
            if (p + 2 > end) {
                invalid("truncated HLE event operand");
            }
            const uint16_t word = rom.read_u16(p);
            p += 2;
            return word;
        };

        if (e.opcode < EV_NOTE_SHORT_MAX) {
            e.key = e.opcode;
            e.operand = read_operand();
            e.duration = e.operand & 0x3ff;
            if (!e.duration) {
                e.duration = read_operand();
            }
            e.velocity = decode_short_note_velocity(e.operand);
        } else if (e.opcode < EV_NOTE_LONG_MAX) {
            e.key = uint8_t(e.opcode - EV_NOTE_KEY_OFFSET);
            e.operand = read_operand();
            e.duration = e.operand & 0x3ff;
            if (!e.duration) {
                e.duration = 1;
            }
            e.velocity = decode_long_note_velocity(e.operand);
        } else if (e.opcode < EV_RELATIVE_NOTE) {
            e.operand = read_operand();
        } else if (e.opcode == EV_RELATIVE_NOTE) {
            invalid("relative-note references are absent from supported ROM bank");
        } else if (e.opcode == EV_DELAY) {
            e.operand = read_operand();
            e.delay = e.operand | (uint16_t(token << 1) & 0x8000);
        } else if (e.opcode == EV_ARRANGEMENT) {
            for (auto &word : e.arrangement) {
                word = read_operand();
            }
            e.delay = 0;
        } else if (e.opcode == EV_NOTIFICATION || e.opcode == EV_END) {
            e.delay = 0;
        } else {
            invalid("unsupported HLE sequence opcode");
        }

        out.events.push_back(e);
        if (e.opcode == EV_END) {
            finished = true;
            break;
        }
    }

    if (!finished) {
        invalid("HLE track missing end event");
    }
    return out;
}

TrackConfig parse_initial_track(const RomView &rom, uint32_t header_addr, size_t track_index) {
    TrackConfig config{};

    // c13d1e/c13ca4 restore even-offset controller defaults from c13556.
    for (uint32_t index = 0; index <= 0x14; index += 2) {
        config.parameters.set_byte(
            TrackParameters::At::controllers + index,
            rom.read_byte(sound_5a7e9117::CONTROLLER_DEFAULTS_TABLE + index));
    }

    const uint32_t record = header_addr + sound_5a7e9117::SEQ_HEADER_TRACKS_OFFSET +
                            uint32_t(track_index) * sound_5a7e9117::SEQ_TRACK_RECORD_SIZE;

    for (uint32_t n = 0; n < sound_5a7e9117::SEQ_TRACK_RECORD_SIZE; ++n) {
        config.parameters.set_byte(TrackParameters::At::program_record + n,
                                   rom.read_byte(record + n));
    }

    config.program = rom.read_u16(record);
    config.parameters.set_program_image(config.program);
    config.parameters.copy_record_to_live();

    return config;
}

std::array<SongData, SONG_SLOTS> parse_sound_bank(const RomView &rom) {
    std::array<SongData, SONG_SLOTS> bank_data{};

    const uint32_t stream_limit = ROM_BASE + SOUND_ROM_IMAGE_SIZE;
    const uint32_t header_rel = rom.read_u32(STREAM_BASE);
    if (header_rel < 8 + SONG_SLOTS * 4 || (header_rel & 1) ||
        STREAM_BASE + header_rel >= stream_limit) {
        invalid("unsupported HLE sequence bank layout");
    }

    const uint32_t header = STREAM_BASE + header_rel;
    const uint32_t header_size = rom.read_u32(header);
    if (header_size < 8 + SONG_SLOTS * 2 + sound_5a7e9117::SEQ_HEADER_SIZE ||
        header + header_size > stream_limit) {
        invalid("unsupported HLE sequence bank layout");
    }

    for (size_t id = 0; id < SONG_SLOTS; ++id) {
        const uint32_t relative = rom.read_u32(STREAM_BASE + 8 + uint32_t(id) * 4);
        if (!relative) {
            continue;
        }

        const uint32_t start = STREAM_BASE + relative;
        const uint32_t end = start + rom.read_u32(start);
        if (relative < 408 || (relative & 1) || end > header || end < start + 48) {
            invalid("invalid HLE sequence extent");
        }

        const uint16_t header_offset = rom.read_u16(header + 8 + uint32_t(id) * 2);
        if (header_offset < 208 ||
            uint32_t(header_offset) + sound_5a7e9117::SEQ_HEADER_SIZE > rom.read_u32(header)) {
            invalid("invalid HLE sequence header");
        }

        const uint32_t h = header + header_offset;
        auto &song = bank_data[id];
        song.present = true;
        song.flags = rom.read_byte(h + 1);
        song.rate = rom.read_byte(h + sound_5a7e9117::SEQ_HEADER_RATE_OFFSET);
        song.bars = rom.read_u16(h + sound_5a7e9117::SEQ_HEADER_BARS_OFFSET);

        const uint8_t exponent = rom.read_byte(h + sound_5a7e9117::SEQ_HEADER_TIME_SIG_EXP_OFFSET);
        if (exponent > 8) {
            invalid("unsupported HLE time signature");
        }
        song.exponent = exponent;
        song.numerator = rom.read_byte(h + sound_5a7e9117::SEQ_HEADER_TIME_SIG_FACTOR_OFFSET);
        song.ticks_per_bar = uint16_t((384u >> exponent) * song.numerator);
        song.loop_start = rom.read_u32(h + sound_5a7e9117::SEQ_HEADER_LOOP_START_OFFSET);
        song.loop_end = rom.read_u32(h + sound_5a7e9117::SEQ_HEADER_LOOP_END_OFFSET);
        song.linked = rom.read_byte(h + sound_5a7e9117::SEQ_HEADER_LINKED_OFFSET);

        for (size_t t = 0; t < TRACK_COUNT; ++t) {
            song.initial[t] = parse_initial_track(rom, h, t);
        }

        std::array<uint32_t, TOTAL_SONG_STREAMS> offsets{};
        for (uint32_t t = 0; t < TOTAL_SONG_STREAMS; ++t) {
            offsets[t] = rom.read_u32(start + 4 + t * 4);
        }

        for (size_t t = 0; t < TOTAL_SONG_STREAMS; ++t) {
            if (!offsets[t]) {
                continue;
            }
            if (offsets[t] < 48 || (offsets[t] & 1) || start + offsets[t] + 4 > end) {
                invalid("invalid HLE track offset");
            }
            uint32_t bound = end;
            for (const auto offset : offsets) {
                if (offset > offsets[t]) {
                    bound = std::min(bound, start + offset);
                }
            }
            song.tracks[t] = parse_track_stream(rom, start + offsets[t] + 4, bound);
        }
    }

    return bank_data;
}

// =============================================================================
// Sequence State Machine
// =============================================================================

enum class SequenceStatus : uint8_t {
    Stopped, // completely inactive; no physical tracks held
    Playing, // active and advancing the tempo pulse (when rate > 0)
    Paused,  // halted via CMD_PAUSE; retains tracks and resumes via CMD_RESUME
    Parked,  // halted at natural end or after CMD_SEEK; retains tracks for direct commands
};

// Sequencing position and tempo. The track records, the note bookkeeping and the volume mix live in TrackService.
struct SongState {
    SequenceStatus status = SequenceStatus::Stopped;
    bool rate_override = false;
    uint16_t rate = 0;
    uint16_t saved_rate = 0;
    uint32_t position = 0;

    // Conductor track (Track 0) state
    size_t conductor = 0;
    uint32_t conductor_countdown = 1;
    bool conductor_ended = false;

    // Arrangement & transposition state
    uint8_t selected = 0;
    uint8_t phase = 0;
    uint8_t arrangement_owner = 0;
    uint16_t arrangement_repeat = 1;
    int16_t transpose = 0;
    uint16_t transpose_mask = 0;

    bool is_active() const noexcept { return status != SequenceStatus::Stopped; }

    bool is_running() const noexcept { return is_active() && rate > 0; }

    void pause_playback() noexcept {
        status = SequenceStatus::Paused;
        rate = 0;
        rate_override = true;
    }

    void resume_playback() noexcept {
        status = SequenceStatus::Playing;
        rate = saved_rate;
    }

    void park_playback() noexcept {
        status = SequenceStatus::Parked;
        rate = 0;
    }

    void override_rate(uint8_t new_rate) noexcept {
        status = SequenceStatus::Playing;
        rate = saved_rate = new_rate;
        rate_override = true;
    }
};

} // namespace

// =============================================================================
// Sequencer Orchestrator Implementation
// =============================================================================

struct Sequencer::Impl {
    std::span<const uint8_t> rom;
    Mailbox &mailbox;
    TrackService &tracks;
    std::array<SongData, SONG_SLOTS> data{};
    std::array<SongState, SONG_SLOTS> state{};
    std::array<PulseTimer, SONG_SLOTS> beats{};
    uint64_t serial = 0;
    uint8_t current_bank = 1;

    // -------------------------------------------------------------------------
    // 1. Lifecycle & Public Entry Points
    // -------------------------------------------------------------------------

    Impl(std::span<const uint8_t> image, Mailbox &mailbox, TrackService &tracks)
        : rom(image), mailbox(mailbox), tracks(tracks) {
        if (rom.size() < SOUND_ROM_SIZE) {
            invalid("HLE requires at least a 512 KiB padded sound ROM");
        }
        load_bank(1);
    }

    void load_bank(uint8_t bank) {
        current_bank = bank;
        data = parse_sound_bank(RomView{rom, current_bank});
    }

    void dispatch_command_packet(std::span<const uint8_t> p, uint64_t instance, uint64_t tick) {
        if (p.size() < 2 || p[0] != p.size()) {
            invalid("invalid HLE length-prefixed packet");
        }

        const uint8_t op = p[1];
        if (op == CMD_EFFECT_PRESET) {
            if (p.size() == 3 && p[2] < 13) {
                mailbox.push(EffectEvent{EffectEvent::Kind::Preset, tick, p[2]});
            }
            return;
        }
        if (op == CMD_EFFECT_PARAM) {
            if (p.size() == 4) {
                mailbox.push(EffectEvent{EffectEvent::Kind::Parameter, tick, 0, p[2], p[3]});
            }
            return;
        }

        // Firmware dispatcher consumes rejected opcodes.
        if (op < CMD_OP_MIN || op > CMD_OP_MAX) {
            return;
        }
        if (p.size() != expected_sequence_command_length(op)) {
            return;
        }

        const bool selection = (p[2] & 0x80) != 0;
        uint8_t id = p[2] & 0x7f;
        if (id >= SONG_SLOTS) {
            return;
        }

        // c12de6 applies a command to this sequence and follows linked aliases.
        std::array<bool, SONG_SLOTS> visited{};
        while (true) {
            if (visited[id]) {
                invalid("cyclic HLE linked sequence alias");
            }
            visited[id] = true;
            execute_sequence_command(op, id, selection, p, instance, tick);
            if (!data[id].present || !data[id].linked) {
                break;
            }
            id = data[id].linked;
            if (id >= SONG_SLOTS) {
                invalid("invalid HLE linked sequence alias");
            }
        }
    }

    // -------------------------------------------------------------------------
    // 2. Mailbox Command Execution
    // -------------------------------------------------------------------------

    void execute_sequence_command(uint8_t op, uint8_t id, bool selection,
                                  std::span<const uint8_t> p, uint64_t instance, uint64_t tick) {
        if (selection && is_arranged(id)) {
            if (op == CMD_START || op == CMD_START_REPEAT) {
                stop_selected_child(id, tick);
                advance_arrangement(id, tick);
                return;
            }
            if (op == CMD_STOP) {
                stop_selected_child(id, tick);
                return;
            }
            if (const uint8_t child = selected_child(id)) {
                execute_sequence_command(op, child, false, p, instance, tick);
            }
            return;
        }

        auto &s = state[id];
        if (op == CMD_START || op == CMD_START_REPEAT) {
            start_sequence(id, tick);
            return;
        }
        if (op == CMD_STOP) {
            stop_sequence(id, tick);
            return;
        }
        if (!s.is_active() && op != CMD_SEEK) {
            return;
        }

        switch (op) {
        case CMD_PAUSE:
            s.pause_playback();
            beats[id].set_step(0);
            tracks.release_notes(id, tick);
            break;

        case CMD_RESUME:
            s.resume_playback();
            beats[id].set_step(s.rate);
            break;

        case CMD_SEEK: {
            if (!s.is_active()) {
                start_sequence(id, tick);
            }
            initialize_tracks(id, id, tick);
            const uint32_t target_pulses = (uint32_t(p[3]) << 8) | p[4];
            for (uint64_t n = 0; n < target_pulses && s.is_active(); ++n) {
                tick_sequence(id, tick, /*silent=*/true);
            }
            finish_sequence(id, tick);
            break;
        }

        case CMD_SEQUENCE_VOLUME:
            mailbox.push(TrackEvent{TrackEventKind::SequenceVolume, tick, id, 0, 0, p[3]});
            break;

        case CMD_SEQUENCE_PAN:
            mailbox.push(TrackEvent{TrackEventKind::SequencePan, tick, id, 0, 0, p[3]});
            break;

        case CMD_SEQUENCE_RATE:
            s.override_rate(p[3]);
            beats[id].set_step(s.rate);
            break;

        case CMD_SEQUENCE_FADE: {
            const RomView rom_view{rom, current_bank};
            const auto multiplier =
                int16_t(rom_view.read_u16(sound_5a7e9117::RATE_TABLE + uint32_t(p[5]) * 2));
            mailbox.push(TrackEvent{TrackEventKind::SequenceFade, tick, id, 0, 0, 0, 0, p[3], p[4],
                                    multiplier});
            break;
        }

        default:
            execute_track_command(op, id, p, instance, tick);
            break;
        }
    }

    void execute_track_command(uint8_t op, uint8_t id, std::span<const uint8_t> p,
                               uint64_t instance, uint64_t tick) {
        const uint8_t logical = p[3];
        if (!logical || logical > TRACK_COUNT) {
            return;
        }
        const auto t = uint8_t(logical - 1);
        const Track &track = tracks.track(id, t);
        if (!track.is_live()) {
            return;
        }

        switch (op) {
        case CMD_TRACK_VOLUME:
            mailbox.push(TrackEvent{TrackEventKind::Volume, tick, id, t, 0, p[4]});
            break;

        case CMD_TRACK_PAN:
            mailbox.push(TrackEvent{TrackEventKind::Pan, tick, id, t, 0, p[4]});
            break;

        case CMD_TRACK_PARAM:
            mailbox.push(TrackEvent{TrackEventKind::Controller, tick, id, t, p[4], p[5]});
            break;

        case CMD_TRACK_PROGRAM:
            mailbox.push(TrackEvent{TrackEventKind::Program, tick, id, t, 0, 0,
                                    uint16_t((uint16_t(p[4]) << 8) | p[5])});
            break;

        case CMD_DIRECT_NOTE_ON:
            if (instance & (uint64_t(1) << 63)) {
                invalid("direct-note instance uses reserved music ID bit");
            }
            trigger_note(id, t, p[4], p[5], 0x7fff, instance, tick, /*transpose=*/false);
            break;

        case CMD_DIRECT_NOTE_OFF:
            mailbox.push(ReleaseKeyEvent{tick, id, t, uint8_t(p[4] + 21)});
            break;

        case CMD_NOP:
            // c1310e retains return-PC c13148 in A3; c13148 writes 6(A3)
            // = ROM c1314e, not note RAM. The board ignores this ROM write.
            break;

        default:
            break;
        }
    }

    // -------------------------------------------------------------------------
    // 3. Sequence Transport (Start / Stop / Finish / Init)
    // -------------------------------------------------------------------------

    void start_sequence(uint8_t id, uint64_t tick) {
        if (!data[id].present) {
            return;
        }
        const uint8_t phase = state[id].phase;
        stop_sequence(id, tick);

        auto &s = state[id];
        s = {};
        s.status = SequenceStatus::Playing;
        s.rate = s.saved_rate = data[id].rate;
        s.phase = phase;
        // An ordinary start never interprets the selection list in track 10.
        tracks.start_sequence(id, data[id].initial, tick);
        beats[id].reset();
        beats[id].set_step(s.rate);
        initialize_tracks(id, id, tick);
    }

    void initialize_tracks(uint8_t owner, uint8_t selected, uint64_t tick) {
        auto &s = state[owner];
        const auto &song = data[selected];
        if (!song.present) {
            invalid("arrangement selects absent HLE sequence");
        }

        s.selected = selected;
        s.position = 0;
        s.conductor = 0;
        s.conductor_countdown = 1;
        s.conductor_ended = song.tracks[CONDUCTOR_STREAM_INDEX].events.empty();

        // Note-offs only: the physical tracks survive a repeat.
        tracks.release_notes(owner, tick);
        for (size_t t = 0; t < TRACK_COUNT; ++t) {
            Track &track = tracks.track(owner, uint8_t(t));
            track.config = song.initial[t];
            track.config.parameters.set_previous_key(tracks.previous_key(track.physical));
            track.cursor = 0;
            track.countdown = 1;

            const bool can_stream = track.physical &&
                                    !song.tracks[FIRST_MUSIC_STREAM_INDEX + t].events.empty() &&
                                    track.config.program != NO_PROGRAM;
            track.status = can_stream ? TrackStatus::Streaming : TrackStatus::Inactive;
            // c13ef6 re-sends the track record: voices of this track still sounding pick it up before the
            // pulse's own events are applied.
            if (track.is_live()) mailbox.push(TrackEvent{TrackEventKind::Refresh, tick, owner, uint8_t(t)});
        }

        if (!s.rate_override) {
            s.saved_rate = s.rate = song.rate;
            beats[owner].set_step(s.rate);
        }

        process_due_events(owner, tick, /*silent=*/false, /*initial=*/true);
    }

    void stop_sequence(uint8_t id, uint64_t tick) {
        tracks.stop_sequence(id, tick);
        beats[id].set_step(0);
        state[id].status = SequenceStatus::Stopped;
    }

    void finish_sequence(uint8_t id, uint64_t tick) {
        // Natural end parks the sequencer but retains allocated track records. c13072 still accepts direct 8d/8e
        // commands on those tracks. Explicit 82 instead removes the active sequence through c12c30.
        tracks.park_sequence(id, tick);
        state[id].park_playback();
        beats[id].set_step(0);
    }

    // -------------------------------------------------------------------------
    // 4. Arrangement Playlist Control (Track 10)
    // -------------------------------------------------------------------------

    bool is_arranged(uint8_t id) const {
        const auto &arr_events = data[id].tracks[ARRANGEMENT_STREAM_INDEX].events;
        return !arr_events.empty() && arr_events.front().opcode == EV_ARRANGEMENT;
    }

    uint8_t selected_child(uint8_t id) const {
        const auto &s = state[id];
        const auto &arr_events = data[id].tracks[ARRANGEMENT_STREAM_INDEX].events;
        if (!s.phase || s.phase > arr_events.size()) {
            return 0;
        }
        const auto &e = arr_events[s.phase - 1];
        return (e.opcode == EV_ARRANGEMENT && e.arrangement[3] != ARRANGEMENT_LOOP_MARKER)
                   ? uint8_t(e.arrangement[0])
                   : 0;
    }

    void stop_selected_child(uint8_t id, uint64_t tick) {
        if (const uint8_t child = selected_child(id)) {
            stop_sequence(child, tick);
        }
        state[id].phase = 0;
    }

    uint8_t advance_arrangement(uint8_t id, uint64_t tick, const SongState *previous = nullptr) {
        auto &s = state[id];
        const auto &list = data[id].tracks[ARRANGEMENT_STREAM_INDEX].events;

        // c12bea/c12d0e: 100 is a loop-selection marker, not a playable phase.
        // At the terminator, return to the phase following the last marker.
        for (size_t attempt = 0; attempt <= list.size(); ++attempt) {
            ++s.phase;
            if (s.phase > list.size() || list[s.phase - 1].opcode == EV_END) {
                s.phase = 0;
                for (size_t n = 0; n < list.size(); ++n) {
                    if (list[n].opcode == EV_ARRANGEMENT &&
                        list[n].arrangement[3] == ARRANGEMENT_LOOP_MARKER) {
                        s.phase = uint8_t(n + 1);
                    }
                }
                if (!s.phase) {
                    return 0;
                }
                continue;
            }

            const auto &e = list[s.phase - 1];
            if (e.arrangement[3] == ARRANGEMENT_LOOP_MARKER) {
                continue;
            }

            const auto child = uint8_t(e.arrangement[0]);
            if (!child || e.arrangement[0] >= SONG_SLOTS || !data[child].present) {
                invalid("invalid HLE arrangement selection");
            }

            s.arrangement_repeat = e.arrangement[3] & 0x7f;
            if (state[child].is_active()) {
                return child;
            }

            start_sequence(child, tick);
            auto &next = state[child];
            next.arrangement_owner = id;
            next.transpose_mask = e.arrangement[2];
            next.transpose = decode_arrangement_transpose(e.arrangement[3]);

            if (previous) {
                tracks.copy_mix(previous->selected, child);
                if (previous->rate_override) {
                    next.rate_override = true;
                    next.rate = next.saved_rate = previous->saved_rate;
                    beats[child].set_step(next.rate);
                }
            }
            return child;
        }
        return 0;
    }

    // -------------------------------------------------------------------------
    // 5. Musical Tick & Track Event Processing
    // -------------------------------------------------------------------------

    void tick_sequence(uint8_t id, uint64_t tick, bool silent = false) {
        auto &s = state[id];
        if (!s.is_active()) {
            return;
        }

        ++s.position;
        const auto &song = data[s.selected];

        process_due_events(id, tick, silent);

        // c14cc0..c14d12 performs the header's bar-boundary transition even
        // when an individual track has one final tick beyond that boundary.
        const uint32_t boundary = uint32_t(song.bars) * song.ticks_per_bar;
        bool finished = boundary ? (s.position >= boundary) : s.conductor_ended;
        if (!boundary) {
            for (const auto &track : tracks.tracks(id)) {
                if (track.is_streaming()) {
                    finished = false;
                }
            }
        }
        if (!finished) {
            return;
        }

        if (s.arrangement_owner) {
            auto &owner = state[s.arrangement_owner];
            bool repeat_done = false;
            if (owner.arrangement_repeat) {
                --owner.arrangement_repeat;
                repeat_done = (owner.arrangement_repeat == 0);
            }
            if (repeat_done) {
                const uint8_t child = advance_arrangement(s.arrangement_owner, tick, &s);
                if (child != id) {
                    stop_sequence(id, tick);
                    return;
                }
            }
            initialize_tracks(id, id, tick);
        } else {
            finish_sequence(id, tick);
        }
    }

    void process_due_events(uint8_t id, uint64_t tick, bool silent, bool initial = false) {
        auto &s = state[id];
        const auto &song = data[s.selected];

        bool conductor_due = initial;
        if (!s.conductor_ended && !conductor_due && s.conductor_countdown) {
            conductor_due = (--s.conductor_countdown == 0);
        }
        if (!s.conductor_ended && conductor_due) {
            const auto &stream = song.tracks[CONDUCTOR_STREAM_INDEX].events;
            while (s.conductor < stream.size()) {
                const auto &event = stream[s.conductor++];
                if (event.opcode == EV_END) {
                    s.conductor_ended = true;
                    break;
                }
                if (event.opcode != EV_DELAY) {
                    invalid("unsupported HLE conductor control");
                }
                s.conductor_countdown = event.delay;
                if (event.delay) {
                    break;
                }
            }
        }

        for (size_t t = 0; t < TRACK_COUNT; ++t) {
            Track &track = tracks.track(id, uint8_t(t));
            if (!track.is_streaming()) {
                continue;
            }
            if (!initial) {
                if (!track.countdown) {
                    continue;
                }
                --track.countdown;
                if (track.countdown != 0) {
                    continue;
                }
            }

            const auto &stream = song.tracks[FIRST_MUSIC_STREAM_INDEX + t].events;
            while (track.cursor < stream.size()) {
                const auto &event = stream[track.cursor++];
                if (event.opcode == EV_END) {
                    track.status = TrackStatus::Ended;
                    break;
                }
                if (event.opcode < EV_NOTE_LONG_MAX) {
                    if (!silent) {
                        trigger_note(id, t, event.key, event.velocity, event.duration,
                                     (uint64_t(1) << 63) | ++serial, tick);
                    }
                } else {
                    apply_track_control(id, t, event, tick, silent);
                }
                track.countdown = event.delay;
                if (event.delay) {
                    break;
                }
            }
        }
    }

    void apply_track_control(uint8_t id, size_t track_id, const StreamEvent &event, uint64_t tick,
                             bool silent) {
        const uint8_t op = event.opcode;
        const uint8_t value = event.operand & 0x7f;
        const auto t = uint8_t(track_id);

        if (op >= EV_CONTROLLER_MIN && op < EV_CONTROLLER_MAX) {
            if (!silent)
                mailbox.push(TrackEvent{TrackEventKind::Controller, tick, id, t,
                                        uint8_t(op - EV_CONTROLLER_MIN), value});
        } else if (op == EV_PROGRAM_SELECTOR) {
            if (!silent)
                mailbox.push(TrackEvent{TrackEventKind::ProgramSelector, tick, id, t, 0, value});
        } else if (op == EV_SPECIAL_TRACK_DA || op == EV_SPECIAL_TRACK_DB) {
            invalid("HLE special-track control absent from supplied data");
        } else if (op == EV_NOTIFICATION) {
            invalid("HLE notification absent from supplied data");
        } else if (op != EV_DELAY && op != EV_END) {
            invalid("unsupported HLE track control");
        }
    }

    // -------------------------------------------------------------------------
    // 6. Note Triggering
    // -------------------------------------------------------------------------

    void trigger_note(uint8_t id, size_t track_id, uint8_t key, uint8_t velocity, uint16_t duration,
                      uint64_t instance, uint64_t tick, bool transpose = true) {
        auto &s = state[id];
        const auto t = uint8_t(track_id);
        const Track &track = tracks.track(id, t);
        if (!track.is_live() || track.config.program == NO_PROGRAM) {
            return;
        }

        int effective = key;
        if (transpose && (s.transpose_mask & (1u << track_id))) {
            effective += s.transpose;
            if (effective < 0 || effective >= EV_NOTE_SHORT_MAX) {
                return; // c141b6/c141bc.
            }
        }

        const auto kernel_key = uint8_t(effective + 21);
        NoteOnEvent note;
        note.instance = instance;
        note.tick = tick;
        note.sequence = id;
        note.track = t;
        note.duration = duration;
        note.program = track.config.program;
        note.key = key;
        note.kernel_key = kernel_key;
        note.velocity = velocity;
        note.previous_key = track.config.parameters.previous_key();
        mailbox.push(note);

        // c172c2 consumes the previous key; c1723e stores the new factory key
        // only after all instrument layers have received their onset context.
        mailbox.push(TrackEvent{TrackEventKind::PreviousKey, tick, id, t, 0, kernel_key});
    }
};

// =============================================================================
// Public Sequencer Facade
// =============================================================================

Sequencer::Sequencer(std::span<const uint8_t> rom, Mailbox &mailbox, TrackService &tracks)
    : m_impl(std::make_unique<Impl>(rom, mailbox, tracks)) {}

Sequencer::~Sequencer() = default;

void Sequencer::command(std::span<const uint8_t> packet, uint64_t instance, uint64_t tick) {
    m_impl->dispatch_command_packet(packet, instance, tick);
}

void Sequencer::reset(uint64_t tick) {
    for (uint8_t id = 0; id < SONG_SLOTS; ++id) {
        m_impl->stop_sequence(id, tick);
    }
    m_impl->state = {};
    for (PulseTimer &beat : m_impl->beats) {
        beat.reset();
        beat.set_step(0);
    }
    if (m_impl->current_bank != 1) {
        m_impl->load_bank(1);
    }
}

void Sequencer::set_bank(uint8_t bank, [[maybe_unused]] uint64_t tick) {
    if (m_impl->current_bank == bank) {
        return;
    }
    m_impl->load_bank(bank);
}

std::array<PulseTimer, SONG_SLOTS> &Sequencer::beats() noexcept { return m_impl->beats; }

void Sequencer::pulse(uint64_t tick, uint8_t tag) { m_impl->tick_sequence(tag, tick); }

bool Sequencer::validate(std::span<const uint8_t> sound_rom) noexcept {
    if (sound_rom.size() < SOUND_ROM_SIZE) {
        return false;
    }
    try {
        Mailbox mailbox;
        TrackService tracks(mailbox);
        Sequencer seq(sound_rom, mailbox, tracks);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace f3rt::hle
