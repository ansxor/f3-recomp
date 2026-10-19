#include "audio/hle/sequencer.hpp"
#include "f3rt/memory_map.hpp"
#include "audio/hle/sound_rom_5a7e9117.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <utility>
#include <vector>

namespace f3rt::hle {
namespace {

constexpr uint32_t ROM_BASE = SOUND_ROM_BASE;
constexpr uint32_t STREAM_BASE = sound_5a7e9117::SEQ_STREAM_BASE;
constexpr size_t SLOTS = 100;
constexpr size_t TRACK_COUNT = 8;
constexpr size_t NOTE_CAPACITY = 4096;
constexpr uint16_t NO_NOTE = 0xffff;
constexpr uint16_t NO_PROGRAM = 0xffff;

// Song track stream layout
constexpr size_t CONDUCTOR_STREAM_INDEX = 0;
constexpr size_t FIRST_MUSIC_STREAM_INDEX = 1;
constexpr size_t ARRANGEMENT_STREAM_INDEX = 10;
constexpr size_t TOTAL_SONG_STREAMS = 11;
constexpr uint8_t MAX_PHYSICAL_CHANNEL = 56;
constexpr uint16_t TEMPO_ACCUMULATOR_THRESHOLD = 625;

// ROM bus addresses & offsets
constexpr uint32_t BANKED_WINDOW_BASE = 0xc20000;
constexpr uint32_t BANKED_WINDOW_END = 0xc40000;
constexpr uint32_t BANKED_OVERFLOW_BASE = 0xc40000;
constexpr uint32_t BANK_PAGE_SIZE = 0x20000;
constexpr uint32_t BANK_OVERFLOW_OFFSET = 0x40000;
constexpr uint32_t SOUND_ROM_IMAGE_SIZE = 0x80000;

// Channel parameter record offsets
constexpr size_t PARAM_PROGRAM_H = 0x04;
constexpr size_t PARAM_PROGRAM_L = 0x05;
constexpr size_t PARAM_OWNER = sound_5a7e9117::VOICE_OWNER_OFFSET;
constexpr size_t PARAM_MOD_1A = 0x1a;
constexpr size_t PARAM_MOD_1C = 0x1c;
constexpr size_t PARAM_PREVIOUS_KEY = 0x22;
constexpr size_t PARAM_PROGRAM_RECORD_BASE = 0x2a;
constexpr size_t PARAM_MOD_2E = 0x2e;
constexpr size_t PARAM_MOD_2F = 0x2f;
constexpr size_t PARAM_MOD_30 = 0x30;
constexpr size_t PARAM_FLAGS = 0x32;
constexpr size_t PARAM_SELECTOR = 0x33;

// Selector threshold / bit constants
constexpr uint8_t SELECTOR_FLAG_EXTENDED = 0x40;
constexpr uint8_t SELECTOR_BANK_BASE = 0x7c;
constexpr uint8_t SELECTOR_INTERNAL_MIN = 0x64;
constexpr uint8_t SELECTOR_INTERNAL_MAX = 0x7b;
constexpr uint8_t SELECTOR_SPECIAL_7B = 0x7b;

constexpr uint8_t PAN_CENTER = 0x40;

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
constexpr uint8_t CMD_CHANNEL_VOLUME = 0x8a;
constexpr uint8_t CMD_CHANNEL_PAN = 0x8b;
constexpr uint8_t CMD_CHANNEL_PARAM = 0x8c;
constexpr uint8_t CMD_CHANNEL_PROGRAM = 0x8d;
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
    case CMD_CHANNEL_VOLUME:
    case CMD_CHANNEL_PAN:
    case CMD_DIRECT_NOTE_OFF:
        return 5;
    case CMD_CHANNEL_PARAM:
    case CMD_CHANNEL_PROGRAM:
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

struct Event {
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
    std::vector<Event> events;
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
    std::array<Channel, TRACK_COUNT> initial{};
    std::array<TrackData, TOTAL_SONG_STREAMS> tracks{};
    uint8_t linked = 0;
};

class RomView {
public:
    RomView(std::span<const uint8_t> image, uint8_t bank) noexcept : m_rom(image), m_bank(bank) {}

    uint8_t read_byte(uint32_t address) const {
        if (address < ROM_BASE || address >= ROM_BASE + SOUND_ROM_IMAGE_SIZE) {
            invalid("HLE ROM read outside image");
        }

        uint32_t off = address - ROM_BASE;
        if (m_rom.size() > SOUND_ROM_SIZE) {
            if (address >= BANKED_WINDOW_BASE && address < BANKED_WINDOW_END) {
                off = uint32_t(m_bank) * BANK_PAGE_SIZE + (address - BANKED_WINDOW_BASE);
            } else if (address >= BANKED_OVERFLOW_BASE) {
                off = BANK_OVERFLOW_OFFSET + (address - BANKED_OVERFLOW_BASE);
            }
        }

        if (off >= m_rom.size()) {
            invalid("HLE ROM read outside image");
        }
        return m_rom[off];
    }

    uint16_t read_u16(uint32_t address) const {
        return uint16_t((uint16_t(read_byte(address)) << 8) | read_byte(address + 1));
    }

    uint32_t read_u32(uint32_t address) const {
        return (uint32_t(read_u16(address)) << 16) | read_u16(address + 2);
    }

private:
    std::span<const uint8_t> m_rom;
    uint8_t m_bank;
};

TrackData parse_track_stream(const RomView &rom, uint32_t p, uint32_t end) {
    TrackData out;
    bool finished = false;

    while (p + 2 <= end) {
        Event e;
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

Channel parse_initial_channel(const RomView &rom, uint32_t header_addr, size_t track_index) {
    Channel channel{};

    // c13d1e/c13ca4 restore even-offset controller defaults from c13556.
    for (uint32_t index = 0; index <= 0x14; index += 2) {
        channel.parameters[PARAM_OWNER + index] =
            rom.read_byte(sound_5a7e9117::CONTROLLER_DEFAULTS_TABLE + index);
    }

    const uint32_t record = header_addr + sound_5a7e9117::SEQ_HEADER_TRACKS_OFFSET +
                            uint32_t(track_index) * sound_5a7e9117::SEQ_TRACK_RECORD_SIZE;

    for (uint32_t n = 0; n < 14; ++n) {
        channel.parameters[PARAM_PROGRAM_RECORD_BASE + n] = rom.read_byte(record + n);
    }

    channel.program = rom.read_u16(record);
    channel.parameters[PARAM_PROGRAM_H] = uint8_t(channel.program >> 8);
    channel.parameters[PARAM_PROGRAM_L] = uint8_t(channel.program);

    constexpr std::array<std::pair<size_t, size_t>, 5> PARAM_COPIES{{
        {sound_5a7e9117::CHANNEL_PARAM_VOLUME, sound_5a7e9117::CHANNEL_PARAM_VOLUME_RECORD},
        {sound_5a7e9117::CHANNEL_PARAM_PAN, sound_5a7e9117::CHANNEL_PARAM_PAN_RECORD},
        {PARAM_MOD_1A, PARAM_MOD_2E},
        {PARAM_MOD_1C, PARAM_MOD_2F},
        {PARAM_SELECTOR, PARAM_MOD_30},
    }};

    for (const auto &[dst, src] : PARAM_COPIES) {
        channel.parameters[dst] = channel.parameters[src];
    }

    return channel;
}

std::array<SongData, SLOTS> parse_sound_bank(const RomView &rom) {
    std::array<SongData, SLOTS> bank_data{};

    const uint32_t stream_limit = ROM_BASE + SOUND_ROM_IMAGE_SIZE;
    const uint32_t header_rel = rom.read_u32(STREAM_BASE);
    if (header_rel < 8 + SLOTS * 4 || (header_rel & 1) ||
        STREAM_BASE + header_rel >= stream_limit) {
        invalid("unsupported HLE sequence bank layout");
    }

    const uint32_t header = STREAM_BASE + header_rel;
    const uint32_t header_size = rom.read_u32(header);
    if (header_size < 8 + SLOTS * 2 + sound_5a7e9117::SEQ_HEADER_SIZE ||
        header + header_size > stream_limit) {
        invalid("unsupported HLE sequence bank layout");
    }

    for (size_t id = 0; id < SLOTS; ++id) {
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
            song.initial[t] = parse_initial_channel(rom, h, t);
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
// Hardware Resource Pools (Voices & Active Notes)
// =============================================================================

struct ActiveNote {
    uint64_t instance = 0;
    uint16_t next = NO_NOTE;
    uint16_t remaining = 0;
    uint8_t key = 0;
    uint8_t kernel_key = 0;
};

class PhysicalVoicePool {
public:
    // c128a8 allocates channels 56..1. Their previous-key byte survives
    // sequence destruction; portamento must not use logical-track history.
    void reset() noexcept {
        m_used.fill(false);
        m_previous_key.fill(0);
    }

    uint8_t allocate() noexcept {
        for (uint8_t ch = MAX_PHYSICAL_CHANNEL; ch > 0; --ch) {
            if (!m_used[ch]) {
                m_used[ch] = true;
                return ch;
            }
        }
        return 0;
    }

    void release(uint8_t ch) noexcept {
        if (ch > 0 && ch < m_used.size()) {
            m_used[ch] = false;
        }
    }

    uint8_t previous_key(uint8_t ch) const noexcept { return m_previous_key[ch]; }

    void set_previous_key(uint8_t ch, uint8_t key) noexcept { m_previous_key[ch] = key; }

private:
    std::array<bool, MAX_PHYSICAL_CHANNEL + 1> m_used{};
    std::array<uint8_t, MAX_PHYSICAL_CHANNEL + 1> m_previous_key{};
};

class NotePool {
public:
    NotePool() noexcept { reset(); }

    void reset() noexcept {
        m_free_head = 0;
        for (uint16_t n = 0; n < NOTE_CAPACITY; ++n) {
            m_notes[n] = {};
            m_notes[n].next = (n + 1 == NOTE_CAPACITY) ? NO_NOTE : uint16_t(n + 1);
        }
    }

    void allocate_note(uint16_t &track_head, uint64_t instance, uint8_t key, uint8_t kernel_key,
                       uint16_t duration) {
        if (m_free_head == NO_NOTE) {
            invalid("HLE note bookkeeping pool exhausted");
        }
        const uint16_t index = m_free_head;
        m_free_head = m_notes[index].next;

        auto &n = m_notes[index];
        n.instance = instance;
        n.key = key;
        n.kernel_key = kernel_key;
        n.remaining = duration;
        n.next = track_head;
        track_head = index;
    }

    void release_all(uint16_t &track_head, VoiceSink &sink, uint64_t tick) {
        while (track_head != NO_NOTE) {
            const uint16_t n = track_head;
            track_head = m_notes[n].next;
            release_single(n, sink, tick);
        }
    }

    void release_matching_key(uint16_t &track_head, uint8_t kernel_key, VoiceSink &sink,
                              uint64_t tick) {
        uint16_t *link = &track_head;
        while (*link != NO_NOTE) {
            const uint16_t n = *link;
            if (m_notes[n].kernel_key == kernel_key) {
                *link = m_notes[n].next;
                release_single(n, sink, tick);
                break;
            }
            link = &m_notes[n].next;
        }
    }

    void step_durations(uint16_t &track_head, VoiceSink &sink, uint64_t tick) {
        uint16_t *link = &track_head;
        while (*link != NO_NOTE) {
            const uint16_t n = *link;
            if (m_notes[n].remaining) {
                --m_notes[n].remaining;
            }
            if (!m_notes[n].remaining) {
                *link = m_notes[n].next;
                release_single(n, sink, tick);
            } else {
                link = &m_notes[n].next;
            }
        }
    }

private:
    void release_single(uint16_t index, VoiceSink &sink, uint64_t tick) {
        sink.note_off(m_notes[index].instance, tick);
        m_notes[index].next = m_free_head;
        m_free_head = index;
    }

    std::array<ActiveNote, NOTE_CAPACITY> m_notes{};
    uint16_t m_free_head = 0;
};

// =============================================================================
// Sequencer State Machine Models (TrackState & SongState)
// =============================================================================

enum class TrackStatus : uint8_t {
    Inactive,  // Unallocated or empty track
    Streaming, // Actively reading events from ROM track stream
    Ended,     // Reached EV_END, but voice stays live for direct mailbox commands
};

struct TrackState {
    Channel channel{};
    size_t cursor = 0;
    uint32_t countdown = 1;
    uint16_t head = NO_NOTE;
    uint8_t physical = 0;
    TrackStatus status = TrackStatus::Inactive;

    bool is_live() const noexcept { return status != TrackStatus::Inactive; }

    bool is_streaming() const noexcept { return status == TrackStatus::Streaming; }

    void select_program(uint16_t program) noexcept {
        channel.program = program;
        channel.parameters[PARAM_PROGRAM_H] = uint8_t(program >> 8);
        channel.parameters[PARAM_PROGRAM_L] = uint8_t(program);
    }

    void set_volume(uint8_t volume) noexcept {
        channel.parameters[sound_5a7e9117::CHANNEL_PARAM_VOLUME] = volume;
        channel.parameters[sound_5a7e9117::CHANNEL_PARAM_VOLUME_RECORD] = volume;
    }

    void set_pan(uint8_t pan) noexcept {
        channel.parameters[sound_5a7e9117::CHANNEL_PARAM_PAN] = pan;
        channel.parameters[sound_5a7e9117::CHANNEL_PARAM_PAN_RECORD] = pan;
    }

    void set_controller(size_t controller_index, uint8_t value, const char *err_msg) {
        const size_t offset = PARAM_OWNER + controller_index * 2;
        if (offset >= channel.parameters.size()) {
            invalid(err_msg);
        }
        channel.parameters[offset] =
            (offset == sound_5a7e9117::PAN_CONTROLLER_OFFSET) ? uint8_t(value - PAN_CENTER) : value;
    }

    void apply_program_selector(uint8_t value) noexcept {
        channel.parameters[PARAM_SELECTOR] = value;
        if (value >= SELECTOR_INTERNAL_MIN && value < SELECTOR_INTERNAL_MAX) {
            return;
        }
        if (value == SELECTOR_SPECIAL_7B) {
            channel.parameters[PARAM_FLAGS] |= SELECTOR_FLAG_EXTENDED;
            return;
        }
        if (value >= SELECTOR_BANK_BASE) {
            channel.bank = value - SELECTOR_BANK_BASE;
            return;
        }
        channel.parameters[PARAM_FLAGS] &= uint8_t(~SELECTOR_FLAG_EXTENDED);
        select_program(uint16_t((uint16_t(channel.bank) << 14) | value));
        // c144e2 passes the channel with bit7 set: c0edda clears bank after selection.
        channel.bank = 0;
    }
};

enum class SequenceStatus : uint8_t {
    Stopped, // Completely inactive; no physical channels held
    Playing, // Active and advancing tempo clock (when rate > 0)
    Paused,  // Halted via CMD_PAUSE; retains channels and resumes via CMD_RESUME
    Parked,  // Halted at natural end or after CMD_SEEK; retains channels for 8d/8e commands
};

struct SongState {
    std::array<TrackState, TRACK_COUNT> tracks{};

    // Playback & tempo clock state
    SequenceStatus status = SequenceStatus::Stopped;
    bool rate_override = false;
    uint16_t rate = 0;
    uint16_t saved_rate = 0;
    uint16_t accumulator = 0;
    uint32_t position = 0;

    // Conductor track (Track 0) state
    size_t conductor = 0;
    uint32_t conductor_countdown = 1;
    bool conductor_ended = false;

    // Master volume & fade state
    uint16_t volume = 0x7fff;
    int16_t fade_step = 0;
    uint8_t fade_target = 127;

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

    void set_immediate_volume(uint8_t high_byte) noexcept {
        volume = uint16_t((uint16_t(high_byte) << 8) | uint8_t(volume));
        fade_step = 0;
    }

    void start_volume_fade(uint8_t start_vol, uint8_t target_vol,
                           int16_t rate_multiplier) noexcept {
        volume = uint16_t((uint16_t(start_vol) << 8) | uint8_t(volume));
        fade_target = target_vol;
        const auto difference = int16_t(uint16_t(uint8_t(target_vol - start_vol)) << 8);
        const int32_t product = int32_t(difference) * rate_multiplier;
        fade_step = int16_t((uint32_t(product) * 2) >> 16);
    }

    bool step_tempo_accumulator() noexcept {
        if (!is_running()) {
            return false;
        }
        accumulator = uint16_t(accumulator + rate);
        if (accumulator >= TEMPO_ACCUMULATOR_THRESHOLD) {
            accumulator -= TEMPO_ACCUMULATOR_THRESHOLD;
            return true;
        }
        return false;
    }

    bool step_volume_fade() noexcept {
        if (!is_active() || !fade_step) {
            return false;
        }
        volume = uint16_t(volume + fade_step);
        const auto current = int8_t(volume >> 8);
        const auto target = int8_t(fade_target);
        if ((fade_step > 0 && current >= target) || (fade_step < 0 && current < target)) {
            volume = uint16_t((uint16_t(fade_target) << 8) | uint8_t(volume));
            fade_step = 0;
        }
        return true;
    }
};

} // namespace

// =============================================================================
// Sequencer Orchestrator Implementation
// =============================================================================

struct Sequencer::Impl {
    std::span<const uint8_t> rom;
    VoiceSink &sink;
    std::array<SongData, SLOTS> data{};
    std::array<SongState, SLOTS> state{};
    NotePool note_pool{};
    PhysicalVoicePool voice_pool{};

    uint64_t now = 0;
    uint64_t timer = TIMER_TICKS;
    uint64_t fade_timer = uint64_t(12) * TIMER_TICKS;
    uint64_t serial = 0;
    uint8_t current_bank = 1;
    SequenceObserver *observer = nullptr;

    // -------------------------------------------------------------------------
    // 1. Lifecycle & Public Entry Points
    // -------------------------------------------------------------------------

    explicit Impl(std::span<const uint8_t> image, VoiceSink &output) : rom(image), sink(output) {
        if (rom.size() < SOUND_ROM_SIZE) {
            invalid("HLE requires at least a 512 KiB padded sound ROM");
        }
        load_bank(1);
    }

    void load_bank(uint8_t bank) {
        current_bank = bank;
        data = parse_sound_bank(RomView{rom, current_bank});
    }

    void advance_clock(uint64_t tick) {
        if (tick < now) {
            invalid("HLE sequencer clock moved backwards");
        }

        while (std::min(timer, fade_timer) <= tick) {
            const uint64_t at = std::min(timer, fade_timer);

            if (at == timer) {
                for (uint8_t id = 0; id < SLOTS; ++id) {
                    if (state[id].step_tempo_accumulator()) {
                        tick_sequence(id, at);
                    }
                }
                timer += TIMER_TICKS;
            }

            if (at == fade_timer) {
                for (uint8_t id = 0; id < SLOTS; ++id) {
                    if (state[id].step_volume_fade()) {
                        sync_all_live_tracks(id, at);
                    }
                }
                fade_timer += uint64_t(12) * TIMER_TICKS;
            }
        }

        now = tick;
    }

    void dispatch_command_packet(std::span<const uint8_t> p, uint64_t instance, uint64_t tick) {
        advance_clock(tick);
        if (p.size() < 2 || p[0] != p.size()) {
            invalid("invalid HLE length-prefixed packet");
        }

        const uint8_t op = p[1];
        if (op == CMD_EFFECT_PRESET) {
            if (p.size() == 3 && p[2] < 13) {
                sink.effect(p[2], tick);
            }
            return;
        }
        if (op == CMD_EFFECT_PARAM) {
            if (p.size() == 4) {
                sink.effect_parameter(p[2], p[3], tick);
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
        if (id >= SLOTS) {
            return;
        }

        // c12de6 applies a command to this sequence and follows linked aliases.
        std::array<bool, SLOTS> visited{};
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
            if (id >= SLOTS) {
                invalid("invalid HLE linked sequence alias");
            }
        }
    }

    bool sequences_running() const {
        for (const auto &s : state) {
            if (s.is_running()) {
                return true;
            }
        }
        return false;
    }

    std::optional<SequenceInfo> describe(uint8_t id) const {
        if (id >= SLOTS || !data[id].present) {
            return std::nullopt;
        }
        const auto &song = data[id];
        SequenceInfo info;
        info.id = id;
        info.arranged = is_arranged(id);
        if (info.arranged) {
            info.arrangement_entries =
                uint32_t(song.tracks[ARRANGEMENT_STREAM_INDEX].events.size());
        }
        info.flags = song.flags;
        info.rate = song.rate;
        info.linked = song.linked;
        info.numerator = song.numerator;
        info.exponent = song.exponent;
        info.bars = song.bars;
        info.ticks_per_bar = song.ticks_per_bar;
        info.header_loop_start = song.loop_start;
        info.header_loop_end = song.loop_end;
        for (size_t t = 0; t < TRACK_COUNT; ++t) {
            SequenceTrackInfo track;
            track.track = uint8_t(t + 1);
            track.events = uint32_t(song.tracks[FIRST_MUSIC_STREAM_INDEX + t].events.size());
            track.initial_program = song.initial[t].program;
            info.tracks.push_back(track);
        }
        return info;
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
            for (auto &track : s.tracks) {
                note_pool.release_all(track.head, sink, tick);
            }
            if (observer) {
                observer->rate_change(id, 0, tick);
            }
            break;

        case CMD_RESUME:
            s.resume_playback();
            if (observer) {
                observer->rate_change(id, s.rate, tick);
            }
            break;

        case CMD_SEEK: {
            if (!s.is_active()) {
                start_sequence(id, tick);
            }
            for (auto &track : s.tracks) {
                note_pool.release_all(track.head, sink, tick);
            }
            initialize_tracks(id, id, tick);
            const uint32_t target_pulses = (uint32_t(p[3]) << 8) | p[4];
            for (uint64_t n = 0; n < target_pulses && s.is_active(); ++n) {
                tick_sequence(id, tick, /*silent=*/true);
            }
            s.park_playback();
            break;
        }

        case CMD_SEQUENCE_VOLUME:
            s.set_immediate_volume(p[3]);
            sync_all_live_tracks(id, tick);
            break;

        case CMD_SEQUENCE_PAN:
            for (size_t t = 0; t < TRACK_COUNT; ++t) {
                if (s.tracks[t].is_live()) {
                    s.tracks[t].set_pan(p[3]);
                    sync_channel_to_sink(id, t, tick);
                }
            }
            break;

        case CMD_SEQUENCE_RATE:
            s.override_rate(p[3]);
            if (observer) {
                observer->rate_change(id, s.rate, tick);
            }
            break;

        case CMD_SEQUENCE_FADE: {
            const RomView rom_view{rom, current_bank};
            const auto multiplier =
                int16_t(rom_view.read_u16(sound_5a7e9117::RATE_TABLE + uint32_t(p[5]) * 2));
            s.start_volume_fade(p[3], p[4], multiplier);
            sync_all_live_tracks(id, tick);
            break;
        }

        default:
            execute_channel_command(op, id, p, instance, tick);
            break;
        }
    }

    void execute_channel_command(uint8_t op, uint8_t id, std::span<const uint8_t> p,
                                 uint64_t instance, uint64_t tick) {
        const uint8_t logical = p[3];
        if (!logical || logical > TRACK_COUNT) {
            return;
        }
        const size_t t = logical - 1;
        auto &track = state[id].tracks[t];
        if (!track.is_live()) {
            return;
        }

        switch (op) {
        case CMD_CHANNEL_VOLUME:
            track.set_volume(p[4]);
            sync_channel_to_sink(id, t, tick);
            break;

        case CMD_CHANNEL_PAN:
            track.set_pan(p[4]);
            sync_channel_to_sink(id, t, tick);
            break;

        case CMD_CHANNEL_PARAM:
            track.set_controller(p[4], p[5], "mailbox HLE controller outside parameter record");
            sync_channel_to_sink(id, t, tick);
            break;

        case CMD_CHANNEL_PROGRAM:
            track.select_program(uint16_t((uint16_t(p[4]) << 8) | p[5]));
            sync_channel_to_sink(id, t, tick);
            break;

        case CMD_DIRECT_NOTE_ON:
            if (instance & (uint64_t(1) << 63)) {
                invalid("direct-note instance uses reserved music ID bit");
            }
            trigger_note(id, t, p[4], p[5], 0x7fff, instance, tick, /*transpose=*/false);
            break;

        case CMD_DIRECT_NOTE_OFF:
            note_pool.release_matching_key(track.head, uint8_t(p[4] + 21), sink, tick);
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

        for (size_t t = 0; t < TRACK_COUNT; ++t) {
            auto &track = s.tracks[t];
            note_pool.release_all(track.head, sink, tick);
            track.channel = song.initial[t];

            if (track.channel.program != NO_PROGRAM && !track.physical) {
                track.physical = voice_pool.allocate();
            }

            track.channel.parameters[PARAM_PREVIOUS_KEY] = voice_pool.previous_key(track.physical);
            track.cursor = 0;
            track.countdown = 1;

            const bool can_stream = track.physical &&
                                    !song.tracks[FIRST_MUSIC_STREAM_INDEX + t].events.empty() &&
                                    track.channel.program != NO_PROGRAM;
            track.status = can_stream ? TrackStatus::Streaming : TrackStatus::Inactive;
            if (track.is_live()) {
                sync_channel_to_sink(owner, t, tick);
            }
        }

        if (!s.rate_override) {
            s.saved_rate = s.rate = song.rate;
        }

        if (observer) {
            SectionInfo info;
            info.owner = owner;
            info.sequence = selected;
            info.phase = state[owner].phase;
            info.repeat_index = state[owner].arrangement_repeat;
            info.bars = song.bars;
            info.ticks_per_bar = song.ticks_per_bar;
            info.numerator = song.numerator;
            info.exponent = song.exponent;
            info.rate = s.rate;
            info.transpose = s.transpose;
            info.transpose_mask = s.transpose_mask;
            observer->section_start(info, tick);
            observer->bar(owner, 0, tick);
        }

        process_due_events(owner, tick, /*silent=*/false, /*initial=*/true);
    }

    void stop_sequence(uint8_t id, uint64_t tick) {
        auto &s = state[id];
        for (auto &track : s.tracks) {
            note_pool.release_all(track.head, sink, tick);
            if (track.physical) {
                voice_pool.release(track.physical);
            }
            track.physical = 0;
        }
        s.status = SequenceStatus::Stopped;
        if (observer) {
            observer->section_end(id, SectionEnd::Stopped, tick);
        }
    }

    void finish_sequence(uint8_t id, uint64_t tick) {
        // Natural end parks the sequencer but retains allocated track/channel
        // records. c13072 still accepts direct 8d/8e commands on those tracks.
        // Explicit 82 instead removes the active sequence through c12c30.
        auto &s = state[id];
        for (auto &track : s.tracks) {
            note_pool.release_all(track.head, sink, tick);
        }
        s.park_playback();
        if (observer) {
            observer->section_end(id, SectionEnd::Finished, tick);
        }
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
                if (observer) {
                    observer->arrangement_wrap(id, s.phase, tick);
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
            if (!child || e.arrangement[0] >= SLOTS || !data[child].present) {
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
                next.volume = previous->volume;
                next.fade_step = previous->fade_step;
                next.fade_target = previous->fade_target;
                if (previous->rate_override) {
                    next.rate_override = true;
                    next.rate = next.saved_rate = previous->saved_rate;
                }
                sync_all_live_tracks(child, tick);
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
        if (observer && song.ticks_per_bar > 0 && s.position % song.ticks_per_bar == 0) {
            observer->bar(id, s.position / song.ticks_per_bar, tick);
        }

        for (auto &track : s.tracks) {
            note_pool.step_durations(track.head, sink, tick);
        }

        process_due_events(id, tick, silent);

        // c14cc0..c14d12 performs the header's bar-boundary transition even
        // when an individual track has one final tick beyond that boundary.
        const uint32_t boundary = uint32_t(song.bars) * song.ticks_per_bar;
        bool finished = boundary ? (s.position >= boundary) : s.conductor_ended;
        if (!boundary) {
            for (const auto &track : s.tracks) {
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
            if (observer) {
                observer->section_end(id, repeat_done ? SectionEnd::Boundary : SectionEnd::Repeat,
                                      tick);
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
            auto &track = s.tracks[t];
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

    void apply_track_control(uint8_t id, size_t track_id, const Event &event, uint64_t tick,
                             bool silent) {
        auto &track = state[id].tracks[track_id];
        const uint8_t op = event.opcode;
        const uint8_t value = event.operand & 0x7f;

        if (op >= EV_CONTROLLER_MIN && op < EV_CONTROLLER_MAX) {
            track.set_controller(op - EV_CONTROLLER_MIN, value,
                                 "HLE controller outside channel parameter record");
        } else if (op == EV_PROGRAM_SELECTOR) {
            track.apply_program_selector(value);
        } else if (op == EV_SPECIAL_TRACK_DA || op == EV_SPECIAL_TRACK_DB) {
            invalid("HLE special-track control absent from supplied data");
        } else if (op == EV_NOTIFICATION) {
            invalid("HLE notification absent from supplied data");
        } else if (op != EV_DELAY && op != EV_END) {
            invalid("unsupported HLE track control");
        }

        if (!silent && op < EV_DELAY) {
            sync_channel_to_sink(id, track_id, tick);
        }
    }

    // -------------------------------------------------------------------------
    // 6. Note Triggering & VoiceSink Synchronization
    // -------------------------------------------------------------------------

    void trigger_note(uint8_t id, size_t track_id, uint8_t key, uint8_t velocity, uint16_t duration,
                      uint64_t instance, uint64_t tick, bool transpose = true) {
        auto &s = state[id];
        auto &track = s.tracks[track_id];
        if (!track.is_live() || track.channel.program == NO_PROGRAM) {
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
        note_pool.allocate_note(track.head, instance, key, kernel_key, duration);

        Note note;
        note.instance = instance;
        note.tick = tick;
        note.sequence = id;
        note.track = uint8_t(track_id + 1);
        note.key = key;
        note.kernel_key = kernel_key;
        note.velocity = velocity;
        note.channel = track.channel;
        note.channel.sequence_volume = uint8_t(s.volume >> 8);
        sink.note_on(note);

        // c172c2 consumes the previous key; c1723e stores the new factory key
        // only after all instrument layers have received their onset context.
        track.channel.parameters[PARAM_PREVIOUS_KEY] = kernel_key;
        voice_pool.set_previous_key(track.physical, kernel_key);
    }

    void sync_channel_to_sink(uint8_t id, size_t track, uint64_t tick) {
        auto &channel = state[id].tracks[track].channel;
        channel.sequence_volume = uint8_t(state[id].volume >> 8);
        sink.channel_update(id, uint8_t(track + 1), channel, tick);
    }

    void sync_all_live_tracks(uint8_t id, uint64_t tick) {
        for (size_t t = 0; t < TRACK_COUNT; ++t) {
            if (state[id].tracks[t].is_live()) {
                sync_channel_to_sink(id, t, tick);
            }
        }
    }
};

// =============================================================================
// Public Sequencer Facade
// =============================================================================

Sequencer::Sequencer(std::span<const uint8_t> rom, VoiceSink &sink)
    : m_impl(std::make_unique<Impl>(rom, sink)) {}

Sequencer::~Sequencer() = default;

void Sequencer::command(std::span<const uint8_t> packet, uint64_t instance, uint64_t tick) {
    m_impl->dispatch_command_packet(packet, instance, tick);
}

void Sequencer::advance_to(uint64_t tick) { m_impl->advance_clock(tick); }

void Sequencer::reset(uint64_t tick) {
    if (tick < m_impl->now) {
        invalid("HLE reset clock moved backwards");
    }
    for (uint8_t id = 0; id < SLOTS; ++id) {
        m_impl->stop_sequence(id, tick);
    }
    m_impl->state = {};
    m_impl->note_pool.reset();
    m_impl->voice_pool.reset();
    m_impl->now = tick;
    m_impl->timer = tick + TIMER_TICKS;
    m_impl->fade_timer = tick + uint64_t(12) * TIMER_TICKS;
    if (m_impl->current_bank != 1) {
        m_impl->load_bank(1);
    }
    m_impl->sink.reset(tick);
}

void Sequencer::set_bank(uint8_t bank, [[maybe_unused]] uint64_t tick) {
    if (m_impl->current_bank == bank) {
        return;
    }
    m_impl->load_bank(bank);
}

uint64_t Sequencer::next_tick() const { return std::min(m_impl->timer, m_impl->fade_timer); }

void Sequencer::set_observer(SequenceObserver *observer) { m_impl->observer = observer; }

bool Sequencer::sequences_running() const { return m_impl->sequences_running(); }

std::optional<SequenceInfo> Sequencer::describe(uint8_t id) const { return m_impl->describe(id); }

bool Sequencer::validate(std::span<const uint8_t> sound_rom) noexcept {
    if (sound_rom.size() < SOUND_ROM_SIZE) {
        return false;
    }
    try {
        struct NullSink : VoiceSink {
            void note_on(const Note &) override {}
            void note_off(uint64_t, uint64_t) override {}
            void channel_update(uint8_t, uint8_t, const Channel &, uint64_t) override {}
            void effect(uint8_t, uint64_t) override {}
            void effect_parameter(uint8_t, uint8_t, uint64_t) override {}
            void reset(uint64_t) override {}
        } null_sink;
        Sequencer seq(sound_rom, null_sink);
        return true;
    } catch (...) {
        return false;
    }
}

} // namespace f3rt::hle
