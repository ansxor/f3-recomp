#include "audio/hle/sequencer.hpp"
#include "f3rt/memory_map.hpp"
#include "audio/hle/sound_rom_5a7e9117.hpp"
#include <algorithm>
#include <array>
#include <limits>
#include <stdexcept>
#include <vector>

namespace f3rt::hle {
namespace {
constexpr uint32_t ROM_BASE = SOUND_ROM_BASE, STREAM_BASE = sound_5a7e9117::SEQ_STREAM_BASE;
constexpr size_t SLOTS = 100, TRACK_COUNT = 8, NOTE_CAPACITY = 4096;
constexpr uint16_t NO_NOTE = 0xffff;
[[noreturn]] void invalid(const char *message) { throw std::runtime_error(message); }
struct Event {
    uint32_t address = 0, delay = 0;
    uint16_t operand = 0, duration = 0;
    uint8_t opcode = 0, key = 0, velocity = 0;
    std::array<uint16_t, 4> arrangement{};
};
struct TrackData {
    std::vector<Event> events;
};
struct SongData {
    bool present = false;
    uint8_t flags = 0, rate = 0;
    uint16_t bars = 0, ticks_per_bar = 0;
    uint32_t loop_start = 0, loop_end = 0;
    std::array<Channel, TRACK_COUNT> initial{};
    std::array<TrackData, 11> tracks;
    uint8_t linked = 0;
};
struct TrackState {
    Channel channel;
    size_t cursor = 0;
    uint32_t countdown = 1;
    uint16_t head = NO_NOTE;
    uint8_t physical = 0;
    bool live = false, ended = false;
};
struct SongState {
    std::array<TrackState, TRACK_COUNT> tracks;
    bool active = false, looping = false, rate_override = false;
    uint8_t selected = 0, phase = 0, arrangement_owner = 0, fade_target = 127;
    uint16_t rate = 0, saved_rate = 0, accumulator = 0;
    uint16_t volume = 0x7fff;
    int16_t fade_step = 0, transpose = 0;
    uint16_t transpose_mask = 0, arrangement_repeat = 1;
    uint32_t position = 0;
    size_t conductor = 0;
    uint32_t conductor_countdown = 1;
    bool conductor_ended = false;
};
struct ActiveNote {
    uint64_t instance = 0;
    uint16_t next = NO_NOTE, remaining = 0;
    uint8_t key = 0, kernel_key = 0;
};
} // namespace

struct Sequencer::Impl {
    std::span<const uint8_t> rom;
    VoiceSink &sink;
    std::array<SongData, SLOTS> data{};
    std::array<SongState, SLOTS> state{};
    std::array<ActiveNote, NOTE_CAPACITY> notes{};
    // c128a8 allocates channels 56..1. Their previous-key byte survives
    // sequence destruction; portamento must not use logical-track history.
    std::array<bool, 57> channel_used{};
    std::array<uint8_t, 57> previous_key{};
    uint16_t free_head = 0;
    uint64_t now = 0, timer = TIMER_TICKS, fade_timer = uint64_t(12) * TIMER_TICKS;
    uint64_t serial = 0;

    explicit Impl(std::span<const uint8_t> image, VoiceSink &output) : rom(image), sink(output) {
        if (rom.size() != SOUND_ROM_SIZE) invalid("HLE requires a 512 KiB padded sound ROM");
        const uint32_t stream_limit = ROM_BASE + uint32_t(rom.size());
        const uint32_t header_rel = u32(STREAM_BASE);
        if (header_rel < 8 + SLOTS * 4 || (header_rel & 1) ||
            STREAM_BASE + header_rel >= stream_limit)
            invalid("unsupported HLE sequence bank layout");
        const uint32_t header = STREAM_BASE + header_rel;
        const uint32_t header_size = u32(header);
        if (header_size < 8 + SLOTS * 2 + sound_5a7e9117::SEQ_HEADER_SIZE ||
            header + header_size > stream_limit)
            invalid("unsupported HLE sequence bank layout");
        for (size_t id = 0; id < SLOTS; ++id) {
            const uint32_t relative = u32(STREAM_BASE + 8 + uint32_t(id) * 4);
            if (!relative) continue;
            const uint32_t start = STREAM_BASE + relative, end = start + u32(start);
            if (relative < 408 || (relative & 1) || end > header || end < start + 48)
                invalid("invalid HLE sequence extent");
            auto &song = data[id];
            const uint16_t header_offset = u16(header + 8 + uint32_t(id) * 2);
            if (header_offset < 208 ||
                uint32_t(header_offset) + sound_5a7e9117::SEQ_HEADER_SIZE > u32(header))
                invalid("invalid HLE sequence header");
            const uint32_t h = header + header_offset;
            song.present = true;
            song.flags = byte(h + 1);
            song.rate = byte(h + sound_5a7e9117::SEQ_HEADER_RATE_OFFSET);
            song.bars = u16(h + sound_5a7e9117::SEQ_HEADER_BARS_OFFSET);
            const uint8_t exponent = byte(h + sound_5a7e9117::SEQ_HEADER_TIME_SIG_EXP_OFFSET);
            if (exponent > 8) invalid("unsupported HLE time signature");
            song.ticks_per_bar = uint16_t(
                (384u >> exponent) * byte(h + sound_5a7e9117::SEQ_HEADER_TIME_SIG_FACTOR_OFFSET));
            song.loop_start = u32(h + sound_5a7e9117::SEQ_HEADER_LOOP_START_OFFSET);
            song.loop_end = u32(h + sound_5a7e9117::SEQ_HEADER_LOOP_END_OFFSET);
            song.linked = byte(h + sound_5a7e9117::SEQ_HEADER_LINKED_OFFSET);
            for (size_t t = 0; t < TRACK_COUNT; ++t) {
                auto &channel = song.initial[t];
                // c13d1e/c13ca4 restore even-offset controller defaults from c13556.
                for (uint32_t index = 0; index <= 0x14; index += 2)
                    channel.parameters[sound_5a7e9117::VOICE_OWNER_OFFSET + index] =
                        byte(sound_5a7e9117::CONTROLLER_DEFAULTS_TABLE + index);
                const uint32_t record = h + sound_5a7e9117::SEQ_HEADER_TRACKS_OFFSET +
                                        uint32_t(t) * sound_5a7e9117::SEQ_TRACK_RECORD_SIZE;
                for (uint32_t n = 0; n < 14; ++n) channel.parameters[0x2a + n] = byte(record + n);
                channel.program = u16(record);
                channel.parameters[4] = uint8_t(channel.program >> 8);
                channel.parameters[5] = uint8_t(channel.program);
                for (const auto pair :
                     {std::array<size_t, 2>{sound_5a7e9117::CHANNEL_PARAM_VOLUME,
                                            sound_5a7e9117::CHANNEL_PARAM_VOLUME_RECORD},
                      {sound_5a7e9117::CHANNEL_PARAM_PAN, sound_5a7e9117::CHANNEL_PARAM_PAN_RECORD},
                      {0x1a, 0x2e},
                      {0x1c, 0x2f},
                      {0x33, 0x30}})
                    channel.parameters[pair[0]] = channel.parameters[pair[1]];
            }
            std::array<uint32_t, 11> offsets{};
            for (uint32_t t = 0; t < 11; ++t) offsets[t] = u32(start + 4 + t * 4);
            for (size_t t = 0; t < 11; ++t) {
                if (!offsets[t]) continue;
                if (offsets[t] < 48 || offsets[t] & 1 || start + offsets[t] + 4 > end)
                    invalid("invalid HLE track offset");
                uint32_t bound = end;
                for (const auto offset : offsets)
                    if (offset > offsets[t]) bound = std::min(bound, start + offset);
                parse_track(song.tracks[t], start + offsets[t] + 4, bound);
            }
        }
        init_pool();
    }
    uint8_t byte(uint32_t address) const {
        if (address < ROM_BASE || address - ROM_BASE >= rom.size())
            invalid("HLE ROM read outside image");
        return rom[address - ROM_BASE];
    }
    uint16_t u16(uint32_t address) const {
        return uint16_t(uint16_t(byte(address)) << 8 | byte(address + 1));
    }
    uint32_t u32(uint32_t address) const { return uint32_t(u16(address)) << 16 | u16(address + 2); }
    void parse_track(TrackData &out, uint32_t p, uint32_t end) {
        bool finished = false;
        while (p + 2 <= end) {
            Event e;
            e.address = p;
            const uint16_t token = u16(p);
            p += 2;
            if (!(token & 0x8000)) continue;
            e.opcode = uint8_t(token);
            e.delay = token >> 8 & 0x7f;
            auto operand = [&]() {
                if (p + 2 > end) invalid("truncated HLE event operand");
                const auto word = u16(p);
                p += 2;
                return word;
            };
            if (e.opcode < 0x58) {
                e.key = e.opcode;
                e.operand = operand();
                e.duration = e.operand & 0x3ff;
                if (!e.duration) e.duration = operand();
                const auto velocity = uint8_t(e.operand >> 8 & 0x7c);
                e.velocity = velocity ? uint8_t(velocity | velocity >> 5) : 1;
            } else if (e.opcode < 0xdc) e.operand = operand();
            else if (e.opcode == 0xdc) {
                invalid("relative-note references are absent from supported ROM bank");
            } else if (e.opcode == 0xe6) {
                e.operand = operand();
                e.delay = e.operand | (uint16_t(token << 1) & 0x8000);
            } else if (e.opcode == 0xe7) {
                for (auto &word : e.arrangement) word = operand();
                e.delay = 0;
            } else if (e.opcode == 0xe8 || e.opcode == 0xe9) e.delay = 0;
            else invalid("unsupported HLE sequence opcode");
            out.events.push_back(e);
            if (e.opcode == 0xe9) {
                finished = true;
                break;
            }
        }
        if (!finished) invalid("HLE track missing end event");
    }
    void init_pool() {
        free_head = 0;
        for (uint16_t n = 0; n < NOTE_CAPACITY; ++n) {
            notes[n] = {};
            notes[n].next = n + 1 == NOTE_CAPACITY ? NO_NOTE : uint16_t(n + 1);
        }
    }
    void release(uint16_t index, uint64_t tick) {
        sink.note_off(notes[index].instance, tick);
        notes[index].next = free_head;
        free_head = index;
    }
    void release_track(TrackState &track, uint64_t tick) {
        while (track.head != NO_NOTE) {
            const uint16_t n = track.head;
            track.head = notes[n].next;
            release(n, tick);
        }
    }
    void stop(uint8_t id, uint64_t tick) {
        auto &s = state[id];
        for (auto &track : s.tracks) {
            release_track(track, tick);
            if (track.physical) channel_used[track.physical] = false;
            track.physical = 0;
        }
        s.active = false;
    }
    void finish(uint8_t id, uint64_t tick) {
        // Natural end parks the sequencer but retains allocated track/channel
        // records. c13072 still accepts direct 8d/8e commands on those tracks.
        // Explicit 82 instead removes the active sequence through c12c30.
        auto &s = state[id];
        for (auto &track : s.tracks) release_track(track, tick);
        s.rate = 0;
    }
    void update(uint8_t id, size_t track, uint64_t tick) {
        auto &channel = state[id].tracks[track].channel;
        channel.sequence_volume = uint8_t(state[id].volume >> 8);
        sink.channel_update(id, uint8_t(track + 1), channel, tick);
    }
    void select_program(Channel &channel, uint16_t program) {
        channel.program = program;
        channel.parameters[4] = uint8_t(program >> 8);
        channel.parameters[5] = uint8_t(program);
    }
    void selector(Channel &channel, uint8_t value) {
        channel.parameters[0x33] = value;
        if (value >= 0x64 && value < 0x7b) return;
        if (value == 0x7b) {
            channel.parameters[0x32] |= 0x40;
            return;
        }
        if (value >= 0x7c) {
            channel.bank = value - 0x7c;
            return;
        }
        channel.parameters[0x32] &= uint8_t(~0x40);
        select_program(channel, uint16_t(uint16_t(channel.bank) << 14 | value));
        // c144e2 passes the channel with bit7 set: c0edda clears bank after selection.
        channel.bank = 0;
    }
    void initialize_tracks(uint8_t owner, uint8_t selected, uint64_t tick) {
        auto &s = state[owner];
        const auto &song = data[selected];
        if (!song.present) invalid("arrangement selects absent HLE sequence");
        s.selected = selected;
        s.position = 0;
        s.conductor = 0;
        s.conductor_countdown = 1;
        s.conductor_ended = song.tracks[0].events.empty();
        for (size_t t = 0; t < TRACK_COUNT; ++t) {
            auto &track = s.tracks[t];
            release_track(track, tick);
            track.channel = song.initial[t];
            if (track.channel.program != 0xffff && !track.physical) {
                for (uint8_t n = 56; n; --n)
                    if (!channel_used[n]) {
                        track.physical = n;
                        channel_used[n] = true;
                        break;
                    }
            }
            track.channel.parameters[0x22] = previous_key[track.physical];
            track.cursor = 0;
            track.countdown = 1;
            track.live = track.physical && !song.tracks[t + 1].events.empty() &&
                         track.channel.program != 0xffff;
            track.ended = !track.live;
            if (track.live) update(owner, t, tick);
        }
        if (!s.rate_override) s.saved_rate = s.rate = song.rate;
        events(owner, tick, false, true);
    }
    bool arranged(uint8_t id) const {
        const auto &events = data[id].tracks[10].events;
        return !events.empty() && events.front().opcode == 0xe7;
    }
    uint8_t selected_child(uint8_t id) const {
        const auto &s = state[id];
        const auto &events = data[id].tracks[10].events;
        if (!s.phase || s.phase > events.size()) return 0;
        const auto &e = events[s.phase - 1];
        return e.opcode == 0xe7 && e.arrangement[3] != 100 ? uint8_t(e.arrangement[0]) : 0;
    }
    void stop_selected(uint8_t id, uint64_t tick) {
        if (const uint8_t child = selected_child(id)) stop(child, tick);
        state[id].phase = 0;
    }
    void start(uint8_t id, bool looping, uint64_t tick) {
        if (!data[id].present) return;
        const uint8_t phase = state[id].phase;
        stop(id, tick);
        auto &s = state[id];
        s = {};
        s.active = true;
        s.looping = looping;
        s.rate = s.saved_rate = data[id].rate;
        s.phase = phase;
        // An ordinary start never interprets the selection list in track 10.
        initialize_tracks(id, id, tick);
    }
    uint8_t arrangement_next(uint8_t id, bool looping, uint64_t tick,
                             const SongState *previous = nullptr) {
        auto &s = state[id];
        const auto &list = data[id].tracks[10].events;
        // c12bea/c12d0e: 100 is a loop-selection marker, not a playable phase.
        // At the terminator, return to the phase following the last marker.
        for (size_t attempt = 0; attempt <= list.size(); ++attempt) {
            ++s.phase;
            if (s.phase > list.size() || list[s.phase - 1].opcode == 0xe9) {
                s.phase = 0;
                for (size_t n = 0; n < list.size(); ++n)
                    if (list[n].opcode == 0xe7 && list[n].arrangement[3] == 100)
                        s.phase = uint8_t(n + 1);
                if (!s.phase) return 0;
                continue;
            }
            const auto &e = list[s.phase - 1];
            if (e.arrangement[3] == 100) continue;
            const uint8_t child = uint8_t(e.arrangement[0]);
            if (!child || e.arrangement[0] >= SLOTS || !data[child].present)
                invalid("invalid HLE arrangement selection");
            s.arrangement_repeat = e.arrangement[3] & 0x7f;
            if (state[child].active) return child;
            start(child, looping, tick);
            auto &next = state[child];
            next.arrangement_owner = id;
            next.transpose_mask = e.arrangement[2];
            next.transpose = static_cast<int16_t>(int16_t(uint16_t(e.arrangement[3] * 2)) >> 11);
            if (previous) {
                next.volume = previous->volume;
                next.fade_step = previous->fade_step;
                next.fade_target = previous->fade_target;
                if (previous->rate_override) {
                    next.rate_override = true;
                    next.rate = next.saved_rate = previous->saved_rate;
                }
                for (size_t t = 0; t < TRACK_COUNT; ++t)
                    if (next.tracks[t].live) update(child, t, tick);
            }
            return child;
        }
        return 0;
    }
    void onset(uint8_t id, size_t track_id, uint8_t key, uint8_t velocity, uint16_t duration,
               uint64_t instance, uint64_t tick, bool transpose = true) {
        auto &s = state[id];
        auto &track = s.tracks[track_id];
        if (!track.live || track.channel.program == 0xffff) return;
        int effective = key;
        if (transpose && (s.transpose_mask & (1u << track_id))) {
            effective += s.transpose;
            if (effective < 0 || effective >= 0x58) return; // c141b6/c141bc.
        }
        if (free_head == NO_NOTE) invalid("HLE note bookkeeping pool exhausted");
        const uint16_t index = free_head;
        free_head = notes[index].next;
        auto &n = notes[index];
        n.instance = instance;
        n.key = key;
        n.kernel_key = uint8_t(effective + 21);
        n.remaining = duration;
        n.next = track.head;
        track.head = index;
        Note note;
        note.instance = instance;
        note.tick = tick;
        note.sequence = id;
        note.track = uint8_t(track_id + 1);
        note.key = key;
        note.kernel_key = n.kernel_key;
        note.velocity = velocity;
        note.channel = track.channel;
        note.channel.sequence_volume = uint8_t(s.volume >> 8);
        sink.note_on(note);
        // c172c2 consumes the previous key; c1723e stores the new factory key
        // only after all instrument layers have received their onset context.
        track.channel.parameters[0x22] = n.kernel_key;
        previous_key[track.physical] = n.kernel_key;
    }
    void control(uint8_t id, size_t track_id, const Event &event, uint64_t tick, bool silent) {
        auto &channel = state[id].tracks[track_id].channel;
        const uint8_t op = event.opcode, value = event.operand & 0x7f;
        if (op >= 0xb0 && op < 0xd9) {
            const size_t offset = sound_5a7e9117::VOICE_OWNER_OFFSET + (op - 0xb0) * 2;
            if (offset >= channel.parameters.size())
                invalid("HLE controller outside channel parameter record");
            channel.parameters[offset] = value;
            if (offset == sound_5a7e9117::PAN_CONTROLLER_OFFSET)
                channel.parameters[offset] = uint8_t(value - 0x40);
        } else if (op == 0xd9) selector(channel, value);
        else if (op >= 0x58 && op < 0xb0) invalid("HLE key continuation absent from supplied data");
        else if (op == 0xda || op == 0xdb)
            invalid("HLE special-track control absent from supplied data");
        else if (op == 0xe8) invalid("HLE notification absent from supplied data");
        else if (op != 0xe6 && op != 0xe9) invalid("unsupported HLE track control");
        if (!silent && op < 0xe6) update(id, track_id, tick);
    }
    void events(uint8_t id, uint64_t tick, bool silent, bool initial = false) {
        auto &s = state[id];
        const auto &song = data[s.selected];
        bool conductor_due = initial;
        if (!s.conductor_ended && !conductor_due && s.conductor_countdown) {
            conductor_due = --s.conductor_countdown == 0;
        }
        if (!s.conductor_ended && conductor_due) {
            const auto &stream = song.tracks[0].events;
            while (s.conductor < stream.size()) {
                const auto &event = stream[s.conductor++];
                if (event.opcode == 0xe9) {
                    s.conductor_ended = true;
                    break;
                }
                if (event.opcode != 0xe6) invalid("unsupported HLE conductor control");
                s.conductor_countdown = event.delay;
                if (event.delay) break;
            }
        }
        for (size_t t = 0; t < TRACK_COUNT; ++t) {
            auto &track = s.tracks[t];
            if (!track.live || track.ended) continue;
            if (!initial) {
                if (!track.countdown) continue;
                --track.countdown;
                if (track.countdown != 0) continue;
            }
            const auto &stream = song.tracks[t + 1].events;
            while (track.cursor < stream.size()) {
                const auto &event = stream[track.cursor++];
                if (event.opcode == 0xe9) {
                    track.ended = true;
                    break;
                }
                if (event.opcode < 0x58) {
                    if (!silent)
                        onset(id, t, event.key, event.velocity, event.duration,
                              (uint64_t(1) << 63) | ++serial, tick);
                } else control(id, t, event, tick, silent);
                track.countdown = event.delay;
                if (event.delay) break;
            }
        }
    }
    void pulse(uint8_t id, uint64_t tick, bool silent = false) {
        auto &s = state[id];
        if (!s.active) return;
        ++s.position;
        for (size_t t = 0; t < TRACK_COUNT; ++t) {
            auto &track = s.tracks[t];
            uint16_t *link = &track.head;
            while (*link != NO_NOTE) {
                const uint16_t n = *link;
                if (notes[n].remaining) --notes[n].remaining;
                if (!notes[n].remaining) {
                    *link = notes[n].next;
                    release(n, tick);
                } else link = &notes[n].next;
            }
        }
        events(id, tick, silent);
        // c14cc0..c14d12 performs the header's bar-boundary transition even
        // when an individual track has one final tick beyond that boundary.
        const auto &song = data[s.selected];
        const uint32_t boundary = uint32_t(song.bars) * song.ticks_per_bar;
        bool finished = boundary ? s.position >= boundary : s.conductor_ended;
        if (!boundary)
            for (const auto &track : s.tracks)
                if (track.live && !track.ended) finished = false;
        if (!finished) return;
        if (s.arrangement_owner) {
            auto &owner = state[s.arrangement_owner];
            bool repeat_done = false;
            if (owner.arrangement_repeat) {
                --owner.arrangement_repeat;
                repeat_done = owner.arrangement_repeat == 0;
            }
            if (repeat_done) {
                const uint8_t child = arrangement_next(s.arrangement_owner, true, tick, &s);
                if (child != id) {
                    stop(id, tick);
                    return;
                }
            }
            initialize_tracks(id, id, tick);
        } else if (s.looping) initialize_tracks(id, s.selected, tick);
        else finish(id, tick);
    }
    void advance(uint64_t tick) {
        if (tick < now) invalid("HLE sequencer clock moved backwards");
        while (std::min(timer, fade_timer) <= tick) {
            const uint64_t at = std::min(timer, fade_timer);
            if (at == timer) {
                for (uint8_t id = 0; id < SLOTS; ++id) {
                    auto &s = state[id];
                    if (!s.active || !s.rate) continue;
                    s.accumulator = uint16_t(s.accumulator + s.rate);
                    if (s.accumulator >= 625) {
                        s.accumulator -= 625;
                        pulse(id, at);
                    }
                }
                timer += TIMER_TICKS;
            }
            if (at == fade_timer) {
                for (uint8_t id = 0; id < SLOTS; ++id) {
                    auto &s = state[id];
                    if (!s.active || !s.fade_step) continue;
                    s.volume = uint16_t(s.volume + s.fade_step);
                    const int8_t value = int8_t(s.volume >> 8), target = int8_t(s.fade_target);
                    if ((s.fade_step > 0 && value >= target) ||
                        (s.fade_step < 0 && value < target)) {
                        s.volume = uint16_t(uint16_t(s.fade_target) << 8 | uint8_t(s.volume));
                        s.fade_step = 0;
                    }
                    for (size_t t = 0; t < TRACK_COUNT; ++t)
                        if (s.tracks[t].live) update(id, t, at);
                }
                fade_timer += uint64_t(12) * TIMER_TICKS;
            }
        }
        now = tick;
    }
    void packet(std::span<const uint8_t> p, uint64_t instance, uint64_t tick) {
        advance(tick);
        if (p.size() < 2 || p[0] != p.size()) invalid("invalid HLE length-prefixed packet");
        const uint8_t op = p[1];
        if (op == 0x20) {
            if (p.size() != 3) return;
            if (p[2] < 13) sink.effect(p[2], tick);
            return;
        }
        if (op == 0x21) {
            if (p.size() != 4) return;
            sink.effect_parameter(p[2], p[3], tick);
            return;
        }
        if (op < 0x80 || op > 0x90) return; // Firmware dispatcher consumes rejected opcodes.
        constexpr std::array<uint8_t, 17> LENGTHS{3, 3, 3, 3, 3, 5, 4, 4, 4,
                                                  7, 5, 5, 6, 6, 6, 5, 6};
        if (p.size() != LENGTHS[op - 0x80]) return;
        const bool selection = p[2] & 0x80;
        uint8_t id = p[2] & 0x7f;
        if (id >= SLOTS) return;
        // c12de6 applies a command to this sequence and follows linked aliases.
        std::array<bool, SLOTS> visited{};
        while (true) {
            if (visited[id]) invalid("cyclic HLE linked sequence alias");
            visited[id] = true;
            apply(op, id, selection, p, instance, tick);
            if (!data[id].present || !data[id].linked) break;
            id = data[id].linked;
            if (id >= SLOTS) invalid("invalid HLE linked sequence alias");
        }
    }
    void apply(uint8_t op, uint8_t id, bool selection, std::span<const uint8_t> p,
               uint64_t instance, uint64_t tick) {
        if (selection && arranged(id)) {
            if (op == 0x80 || op == 0x81) {
                stop_selected(id, tick);
                arrangement_next(id, op == 0x81, tick);
                return;
            }
            if (op == 0x82) {
                stop_selected(id, tick);
                return;
            }
            const uint8_t child = selected_child(id);
            if (!child) return;
            apply(op, child, false, p, instance, tick);
            return;
        }
        auto &s = state[id];
        if (op == 0x80 || op == 0x81) {
            start(id, op == 0x81, tick);
            return;
        }
        if (op == 0x82) {
            stop(id, tick);
            return;
        }
        if (!s.active && op != 0x85) return;
        switch (op) {
        case 0x83:
            s.rate = 0;
            s.rate_override = true;
            for (auto &track : s.tracks) release_track(track, tick);
            break;
        case 0x84:
            s.rate = s.saved_rate;
            break;
        case 0x85: {
            if (!s.active) start(id, true, tick);
            for (auto &track : s.tracks) release_track(track, tick);
            initialize_tracks(id, id, tick);
            const uint32_t position = uint32_t(p[3]) << 8 | p[4];
            const uint64_t pulses = position;
            for (uint64_t n = 0; n < pulses && s.active; ++n) pulse(id, tick, true);
            s.rate = 0;
            break;
        }
        case 0x86:
            s.volume = uint16_t(uint16_t(p[3]) << 8 | uint8_t(s.volume));
            s.fade_step = 0;
            for (size_t t = 0; t < TRACK_COUNT; ++t)
                if (s.tracks[t].live) update(id, t, tick);
            break;
        case 0x87:
            for (size_t t = 0; t < TRACK_COUNT; ++t)
                if (s.tracks[t].live) {
                    s.tracks[t].channel.parameters[sound_5a7e9117::CHANNEL_PARAM_PAN] =
                        s.tracks[t].channel.parameters[sound_5a7e9117::CHANNEL_PARAM_PAN_RECORD] =
                            p[3];
                    update(id, t, tick);
                }
            break;
        case 0x88:
            s.rate = s.saved_rate = p[3];
            s.rate_override = true;
            break;
        case 0x89: {
            s.volume = uint16_t(uint16_t(p[3]) << 8 | uint8_t(s.volume));
            s.fade_target = p[4];
            const int16_t difference = int16_t(uint16_t(uint8_t(p[4] - p[3])) << 8);
            const int16_t multiplier =
                int16_t(u16(sound_5a7e9117::RATE_TABLE + uint32_t(p[5]) * 2));
            const int32_t product = int32_t(difference) * multiplier;
            s.fade_step = int16_t(uint32_t(product) * 2 >> 16);
            for (size_t t = 0; t < TRACK_COUNT; ++t)
                if (s.tracks[t].live) update(id, t, tick);
            break;
        }
        default: {
            const uint8_t logical = p[3];
            if (!logical || logical > TRACK_COUNT || !s.tracks[logical - 1].live) return;
            const size_t t = logical - 1;
            auto &track = s.tracks[t];
            if (op == 0x8a || op == 0x8b) {
                const size_t offset = op == 0x8a ? sound_5a7e9117::CHANNEL_PARAM_VOLUME
                                                 : sound_5a7e9117::CHANNEL_PARAM_PAN;
                track.channel.parameters[offset] = p[4];
                track.channel.parameters[op == 0x8a ? sound_5a7e9117::CHANNEL_PARAM_VOLUME_RECORD
                                                    : sound_5a7e9117::CHANNEL_PARAM_PAN_RECORD] =
                    p[4];
                update(id, t, tick);
            } else if (op == 0x8c) {
                const size_t offset = sound_5a7e9117::VOICE_OWNER_OFFSET + size_t(p[4]) * 2;
                if (offset >= track.channel.parameters.size())
                    invalid("mailbox HLE controller outside parameter record");
                track.channel.parameters[offset] =
                    offset == sound_5a7e9117::PAN_CONTROLLER_OFFSET ? uint8_t(p[5] - 0x40) : p[5];
                update(id, t, tick);
            } else if (op == 0x8d) {
                select_program(track.channel, uint16_t(uint16_t(p[4]) << 8 | p[5]));
                update(id, t, tick);
            } else if (op == 0x8e) {
                if (instance & (uint64_t(1) << 63))
                    invalid("direct-note instance uses reserved music ID bit");
                onset(id, t, p[4], p[5], 0x7fff, instance, tick, false);
            } else if (op == 0x8f) {
                uint16_t *link = &track.head;
                while (*link != NO_NOTE) {
                    const uint16_t n = *link;
                    if (notes[n].kernel_key == uint8_t(p[4] + 21)) {
                        *link = notes[n].next;
                        release(n, tick);
                        break;
                    }
                    link = &notes[n].next;
                }
            } else if (op == 0x90) {
                // c1310e retains return-PC c13148 in A3; c13148 writes 6(A3)
                // = ROM c1314e, not note RAM. The board ignores this ROM write.
            }
            break;
        }
        }
    }
};

Sequencer::Sequencer(std::span<const uint8_t> rom, VoiceSink &sink)
    : m_impl(std::make_unique<Impl>(rom, sink)) {}
Sequencer::~Sequencer() = default;
void Sequencer::command(std::span<const uint8_t> packet, uint64_t instance, uint64_t tick) {
    m_impl->packet(packet, instance, tick);
}
void Sequencer::advance_to(uint64_t tick) { m_impl->advance(tick); }
void Sequencer::reset(uint64_t tick) {
    if (tick < m_impl->now) invalid("HLE reset clock moved backwards");
    for (uint8_t id = 0; id < SLOTS; ++id) m_impl->stop(id, tick);
    m_impl->state = {};
    m_impl->init_pool();
    m_impl->now = tick;
    m_impl->channel_used = {};
    m_impl->previous_key = {};
    m_impl->timer = tick + TIMER_TICKS;
    m_impl->fade_timer = tick + uint64_t(12) * TIMER_TICKS;
    m_impl->sink.reset(tick);
}
uint64_t Sequencer::next_tick() const { return std::min(m_impl->timer, m_impl->fade_timer); }
bool Sequencer::validate(std::span<const uint8_t> sound_rom) noexcept {
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
