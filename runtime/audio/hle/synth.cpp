#include "audio/hle/synth.hpp"
#include "audio/hle/sound_bank.hpp"
#include "audio/hle/voice.hpp"
#include "audio/hle/note_timing.hpp"
#include "audio/hle/faust/generated/voice_filter.hpp"
#include "f3rt/bytes.hpp"
#include "audio/hle/sound_rom_5a7e9117.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <stdexcept>
#include <utility>
#include <vector>

namespace f3rt::hle {
using sound_5a7e9117::program_slot;

namespace {
// Frames per voice-filter block: each active voice runs one faust::VoiceFilterDsp::compute() per block.
constexpr size_t VOICE_BLOCK_FRAMES = 64;
using f3rt::s16;
using f3rt::s8;
inline uint32_t longword(const uint8_t *p) { return read_be32(p); }
} // namespace

struct Synth::Impl {
    struct Pending {
        Note note{};
        const uint8_t *program = nullptr;
        const uint8_t *patch = nullptr;
        uint64_t tick = 0;
        uint8_t layer = 0;
        bool active = false, release_trigger = false, direct = false;
    };
    SoundBank bank;
    std::array<Voice, 32> voices{};
    std::array<Pending, 96> pending{};
    DriverClock driver_clock;
    NoteQueue<Pending, 96> queue; // notes the driver clock pushed past their request tick
    std::array<uint8_t, 32> next{}, voice_prev{};
    std::function<void(const VoiceEvent &)> observer;
    struct BufferedEvent {
        size_t frame = 0;
        unsigned voice = 0;
        VoiceEvent event;
    };
    std::vector<BufferedEvent> buffered_events;
    std::vector<uint64_t> frame_ticks;
    bool buffering_events = false;
    size_t current_frame = 0;
    unsigned current_voice = 0;
    uint64_t tick = 0, timer = TIMER_TICKS, serial = 0;
    uint32_t tick_remainder = 0;
    uint16_t noise = 0;
    uint16_t noise_history = 0;
    uint8_t cursor = 0, budget_slot = 0;
    // Voices render one at a time, so one block of scratch serves all of them.
    alignas(64) float input[VOICE_BLOCK_FRAMES]{};
    alignas(64) float a1[VOICE_BLOCK_FRAMES]{};
    alignas(64) float a2[VOICE_BLOCK_FRAMES]{};
    alignas(64) float high_pole[VOICE_BLOCK_FRAMES]{};
    alignas(64) float gain_left[VOICE_BLOCK_FRAMES]{};
    alignas(64) float gain_right[VOICE_BLOCK_FRAMES]{};
    alignas(64) PlanarAudioBlock<2, VOICE_BLOCK_FRAMES> filter_out;
    // Filter state lives in each voice's DSP instance; it persists across blocks and is cleared on note start.
    std::array<faust::VoiceFilterDsp, 32> voice_filters{};
    std::array<float *, 32> mode_zone{};

    Impl(std::span<const uint8_t> r, std::span<const uint16_t> s) : bank(r, s) {
        frame_ticks.reserve(1024);
        buffered_events.reserve(32);
        for (size_t i = 0; i < voice_filters.size(); ++i) {
            voice_filters[i].init(int(SAMPLE_RATE));
            mode_zone[i] = faust::Controls(voice_filters[i]).zone("mode");
        }
        reset(0);
    }
    void set_bank(uint8_t b) { bank.set_bank(b); }
    // c1983c: ramp the level to 0 at the patch's release rate (patch +2).
    bool update_envelope(Voice &v, unsigned which) {
        Envelope &e = v.envelope[which];
        if (e.done) return false;
        if (!e.advance()) return false;
        uint16_t level = uint16_t(e.target >> 16);
        if (e.slope <= 0) level ^= 0x8000;
        e.position = uint32_t(level) << 16;
        if (v.direct) {
            // Timer expiry dispatches on the envelope stage (c1971c -> stage routine), not on note-off: stage 0 starts the
            // release ramp (c1983c: ends the voice if the level's top byte is already 0), stage 4 ends the voice (c19888).
            // A patch whose note-off is ignored (patch[0] bit 7) still decays when its timer expires.
            if (e.stage == 4 || (e.position >> 24) == 0) {
                e.done = true;
                return true;
            }
            start_direct_release(e, v, bank);
            return false;
        }
        if (e.stage == 4) {
            e.done = true;
            return which == 2;
        }
        if (e.intermediate) {
            e.intermediate = false;
            segment(e, e.levels[1], e.rates[0], bank);
            return false;
        }
        ++e.stage;
        if (e.stage == 1) segment(e, e.levels[2], e.rates[1], bank);
        else if (e.stage == 2) segment(e, e.levels[3], e.rates[2], bank);
        else if (e.flags & 0x40) {
            e.stage = 4;
            segment(e, 0, e.rates[3], bank);
        } else if (e.flags & 0x80) {
            e.stage = 0;
            segment(e, e.levels[1], e.rates[0], bank);
            if (which == 2 && (v.patch[0x6c] & 127) < 0x54) {
                v.position = v.reverse ? v.end : accumulator_start(v);
                if (v.sample_finished) {
                    v.sample_finished = false;
                    emit(v, VoiceEvent::Kind::Start, tick);
                }
            }
        } else {
            e.done = true;
            return which == 2 && e.level() == 0;
        }
        return false;
    }
    void parameters(Voice &v, bool initial, uint64_t at, bool notify = true) {
        const uint8_t *p = v.patch;
        const Channel &c = v.owner.channel;
        int common = bank.attenuation[c.parameters[sound_5a7e9117::CHANNEL_PARAM_VOLUME] & 127] +
                     bank.attenuation[c.sequence_volume & 127];
        if (c.parameters[sound_5a7e9117::CHANNEL_PARAM_EXPRESSION])
            common += uint8_t(c.parameters[sound_5a7e9117::CHANNEL_PARAM_EXPRESSION] + 127);
        int pan_position;
        if (v.direct) {
            int curve = (p[0x12] >> 6) * 128 + v.owner.velocity;
            int sensitivity = (p[8] >> 4) * 8 + (p[8] >> 5);
            int scale =
                s16(0x4000 - (128 - bank.velocity_curves[unsigned(curve)]) * sensitivity) >> 6;
            if (initial) {
                Envelope &e = v.envelope[2];
                e.position = (uint32_t(uint16_t((p[0x10] & 127) * scale)) << 16) | 0x7fff;
                e.slope = -s16(bank.rom.read_u16(sound_5a7e9117::RATE_TABLE + 2 * (p[0] & 127)));
                e.target = (uint32_t(uint16_t((e.position >> 16) ^ 0x8000))) << 16;
            }
            pan_position = c.parameters[sound_5a7e9117::CHANNEL_PARAM_PAN] == 128
                               ? ((p[0x12] & 15) ^ 8) << 3
                               : (s8(c.parameters[sound_5a7e9117::CHANNEL_PARAM_PAN]) >> 1) + 64;
            int level = (v.envelope[2].level() >> 5) + ((p[0x10] & 128) ? 0xa8 : 0);
            v.left_volume =
                volume_target((common + bank.pan[unsigned(127 - pan_position)]) * 4 + level, 0xfa0);
            v.right_volume =
                volume_target((common + bank.pan[unsigned(pan_position)]) * 4 + level, 0xfa0);
            int filter_index = direct_filter_index(p[0xe], v.owner.velocity);
            v.k1 = v.k2 = bank.coefficient(filter_index);
        } else {
            int amp_source = source(v, p[0x60] & 15), base;
            if ((p[0x60] & 15) == 15) base = 254;
            else {
                int x = (p[0x60] & 15) == 0 ? (amp_source >> 1) + 0x4000 : std::max(0, amp_source);
                int factor = s8(p[0x62]);
                int value = (x * factor) >> 15;
                if (factor >= 0) value -= factor;
                base = (value + 127) * 2;
            }
            base += bank.attenuation[p[0x5e] & 127] + ((p[0x5e] & 128) ? 43 : 0) + common +
                    bank.attenuation[v.weight & 127];
            int pan_value = s8(c.parameters[sound_5a7e9117::CHANNEL_PARAM_PAN]);
            if (pan_value <= -127) pan_value = s8(p[0x4e] & 0xf0);
            pan_position = (pan_value >> 1) + 64;
            int env = v.envelope[2].level() >> 5;
            v.left_volume =
                volume_target((base + bank.pan[unsigned(127 - pan_position)]) * 4 + env, 0x1b64);
            v.right_volume =
                volume_target((base + bank.pan[unsigned(pan_position)]) * 4 + env, 0x1b64);
            int filter_env = v.envelope[1].level();
            int key = int(v.effective_key) - 60;
            int first = (filter_env * s8(p[0x4c]) + source(v, p[0x4e] & 15) * s8(p[0x50])) >> 12;
            first += (key * s8(p[0x4a])) >> 3;
            first += (p[0x48] & 127) * 8;
            int second = (filter_env * s8(p[0x56]) +
                          ((p[0x52] & 128) ? source(v, p[0x4e] & 15) : 0) * s8(p[0x50])) >>
                         12;
            second += (key * s8(p[0x54])) >> 3;
            second += (p[0x52] & 127) * 8;
            v.k1 = bank.coefficient(first);
            v.k2 = bank.coefficient(second, (p[0x44] & 128) ? 0 : 1024);
            int pitch_env = s16(v.envelope[0].level() - ((p[0xc] & 127) << 8));
            int64_t pitch_mod = int64_t(pitch_env) * s8(p[0x40]) + int64_t(v.lfo) * s8(p[0x42]);
            int depth = s8(p[0x46]);
            int multiplier =
                bank.rom.read_u16(sound_5a7e9117::PITCH_MOD_TABLE + unsigned(std::abs(depth)) * 2);
            if (depth < 0) multiplier = -s16(multiplier);
            pitch_mod += (int64_t(source(v, p[0x44] & 15)) * multiplier) >> 4;
            int bend = s16(parameter_word(c, sound_5a7e9117::VOICE_OWNER_OFFSET) - 0x4000);
            pitch_mod >>= 4;
            pitch_mod += int64_t(bend) * bank.bend_range;
            pitch_mod >>= 6;
            v.pitch = s16(v.glide_target + v.split.tuning + int(pitch_mod));
            if (v.split.mode & 128) {
                int amount = (int(s8(p[0x70])) * source(v, p[0x6e] & 15)) >> 7;
                amount = std::clamp(amount + (p[0x72] << 8), 0, 32767);
                int delta = s16((int64_t(amount) * s16(v.split.accumulator >> 16)) >> 16);
                int span = int16_t(uint16_t((v.split.end - v.split.start) >> 4));
                uint32_t offset = uint32_t(uint32_t(uint16_t(span)) * bank.frequency(delta)) >> 6;
                uint32_t start = (v.split.end - offset) | uint32_t((v.selector & 1) << 28);
                v.start = double((v.selector >> 1) << 20) + double(start & 0x1fffffe0) / 512;
                v.pitch = s16(v.pitch + delta);
            }
            v.frequency = bank.frequency(v.pitch);
        }
        if (v.direct) v.frequency = bank.frequency(v.pitch + v.split.tuning);
        v.increment = double(v.frequency) * NATIVE_RATE / (1024.0 * SAMPLE_RATE);
        unsigned choice = c.parameters[0x31];
        if (choice >= 3) {
            if (choice == 5) choice = 3;
            else choice = (v.direct ? p[0x12] >> 4 : p[0x60] >> 4) & 3;
        }
        v.pair = uint8_t((choice + 1) & 3);
        v.target_left = v.left_volume;
        v.target_right = v.right_volume;
        if (initial) {
            v.left = v.target_left;
            v.right = v.target_right;
            v.gain_frames = 0;
        } else
            v.gain_frames =
                (v.left == v.target_left && v.right == v.target_right) ? 0 : VOLUME_RAMP;
        if (v.gain_frames) {
            v.step_left = (v.target_left - v.left) / v.gain_frames;
            v.step_right = (v.target_right - v.right) / v.gain_frames;
        }
        v.left_segment.frames = v.right_segment.frames = 0;
        v.gain_dirty = true;
        v.filter.targets(v.k1, v.k2, initial);
        if (!initial && notify) emit(v, VoiceEvent::Kind::Parameters, at);
    }
    void emit(const Voice &v, VoiceEvent::Kind kind, uint64_t at,
              const std::array<uint16_t, 4> *onset = nullptr) {
        if (!observer) return;
        VoiceEvent e{};
        e.kind = kind;
        e.instance = v.owner.instance;
        e.tick = at;
        e.sequence = v.owner.sequence;
        e.track = v.owner.track;
        e.key = v.owner.key;
        e.layer = v.layer;
        e.output_pair = v.pair;
        e.sample_start = uint32_t(v.start);
        e.sample_end = uint32_t(v.end);
        e.frequency = v.frequency;
        e.left_volume = v.left_volume;
        e.right_volume = v.right_volume;
        e.k1 = v.k1;
        e.k2 = v.k2;
        if (onset) {
            e.left_volume = (*onset)[0];
            e.right_volume = (*onset)[1];
            e.k1 = (*onset)[2];
            e.k2 = (*onset)[3];
        }
        e.loop = v.loop;
        e.reverse = v.reverse;
        e.slot = v.slot;
        e.sounding_key = v.effective_key;
        e.velocity = v.owner.velocity;
        e.filter_mode = v.filter.mode;
        e.direct = v.direct;
        e.program = v.owner.channel.program;
        e.pitch = v.pitch;
        e.glide = v.glide_target;
        e.lfo = v.lfo;
        for (unsigned i = 0; i < 3; ++i) e.env[i] = v.envelope[i].level();
        if (buffering_events) buffered_events.push_back({current_frame, current_voice, e});
        else observer(e);
    }
    void stop(Voice &v, uint64_t at) {
        if (v.active && !v.sample_finished) emit(v, VoiceEvent::Kind::Stop, at);
        v.active = false;
    }
    void end_sample(Voice &v, uint64_t at) {
        if (v.allocation) {
            emit(v, VoiceEvent::Kind::Stop, at);
            v.sample_finished = true;
        } else stop(v, at);
    }
    unsigned allocate(const Note &n, unsigned layer, unsigned priority, unsigned mode) {
        if (mode >= 2) {
            Voice *found = nullptr;
            for (Voice &v : voices)
                if (v.active && !v.released && !v.direct && v.owner.sequence == n.sequence &&
                    v.owner.track == n.track && v.layer == layer)
                    if (!found || v.serial < found->serial) found = &v;
            if (found) return unsigned(found - voices.data());
        }
        if (mode == 1) {
            for (Voice &v : voices)
                if (v.active && !v.released && !v.direct && v.owner.sequence == n.sequence &&
                    v.owner.track == n.track && v.layer == layer) {
                    release(v, n.tick);
                    break;
                }
        }
        for (unsigned i = 32; i--;)
            if (!voices[i].active) return i;
        for (unsigned klass : std::array<unsigned, 3>{0, 64, 128}) {
            if (klass == 64 && priority == 0) break;
            if (klass == 128 && priority <= 64) break;
            for (bool released : std::array<bool, 2>{true, false}) {
                Voice *old = nullptr;
                for (Voice &v : voices)
                    if (v.released == released && (klass == 128 || v.priority == klass)) {
                        if (!old) old = &v;
                        else {
                            const uint64_t v_k = released ? v.release_serial : v.serial;
                            const uint64_t o_k = released ? old->release_serial : old->serial;
                            if (v_k < o_k) old = &v;
                        }
                    }
                if (old) {
                    unsigned index = unsigned(old - voices.data());
                    stop(*old, n.tick);
                    return index;
                }
            }
        }
        return 32;
    }
    void move_before_cursor(unsigned i) {
        unsigned a = voice_prev[i], b = next[i];
        next[a] = uint8_t(b);
        voice_prev[b] = uint8_t(a);
        if (cursor == i) cursor = uint8_t(b);
        a = voice_prev[cursor];
        next[a] = uint8_t(i);
        voice_prev[i] = uint8_t(a);
        next[i] = cursor;
        voice_prev[cursor] = uint8_t(i);
    }
    void begin(const Note &n, const uint8_t *prog, const uint8_t *patch, unsigned layer,
               bool direct) {
        int kernel = uint8_t(n.kernel_key + n.channel.parameters[0x35]);
        int weight = direct ? 127 : bank.key_weight(patch, kernel);
        if (weight < 0) return;
        if (!direct) {
            int threshold = s8(patch[0x6e]);
            int boundary = (std::abs(threshold & ~15) * 9 / 8) & 0x7c;
            if (threshold < 0 ? n.velocity >= boundary : n.velocity < boundary) return;
        }
        unsigned mode = direct ? 0 : (patch[0x44] >> 4) & 3;
        unsigned priority = direct ? 64 : patch[0x60] & 0xc0;
        int prior_pitch = 0;
        bool prior_found = false;
        if (mode == 1)
            for (const Voice &old : voices)
                if (old.active && old.owner.sequence == n.sequence && old.owner.track == n.track &&
                    old.layer == layer) {
                    prior_pitch = old.glide_target;
                    prior_found = true;
                    break;
                }
        unsigned index = allocate(n, layer, priority, mode);
        if (index == 32) return;
        Voice &v = voices[index];
        bool legato = v.active && mode >= 2 && !direct;
        double old_board = double((v.selector >> 1) << 20);
        double old_start = v.start - old_board, old_end = v.end - old_board;
        bool old_hardware = v.hardware_valid;
        if (legato) {
            uint8_t velocity = v.owner.velocity;
            v.owner = n;
            v.owner.velocity = velocity;
            v.released = false;
            int16_t destination = s16(((patch[0x48] & 128) ? kernel : 60) * 256 +
                                      s8(patch[0x3e]) * 2 + (patch[0x3c] << 8));
            set_glide(v, destination, prog[sound_5a7e9117::PROGRAM_GLIDE_OFFSET], bank);
            parameters(v, false, n.tick);
            return;
        }
        stop(v, n.tick);
        v = Voice{};
        v.slot = uint8_t(index);
        voice_filters[index].instanceClear();
        v.owner = n;
        v.patch = patch;
        v.layer = uint8_t(layer);
        v.direct = direct;
        v.priority = uint8_t(priority);
        v.allocation = uint8_t(mode);
        v.effective_key = uint8_t(std::clamp(kernel, 21, 108));
        v.weight = uint8_t(weight);
        v.serial = ++serial;
        if (direct) {
            int key = (patch[2] & 128) ? uint8_t(kernel - patch[10] + 60) : 60;
            key = s8(key + s8(n.channel.parameters[sound_5a7e9117::CHANNEL_PARAM_TRANSPOSE]) +
                     s8(patch[6]));
            v.pitch = s16(std::clamp(key * 256, 0, 0x7f00) | ((patch[8] & 15) << 4));
        } else {
            v.glide_target = s16(((patch[0x48] & 128) ? kernel : 60) * 256 + s8(patch[0x3e]) * 2 +
                                 (patch[0x3c] << 8));
            v.pitch = static_cast<int16_t>(std::clamp(int(v.glide_target), 0, 0x7f00));
            if (prog[sound_5a7e9117::PROGRAM_GLIDE_OFFSET] && (mode == 2 || prior_found)) {
                int16_t destination = v.glide_target;
                v.glide_target =
                    prior_found
                        ? s16(prior_pitch)
                        : s16(uint8_t(n.channel.parameters[sound_5a7e9117::CHANNEL_PARAM_PITCH] +
                                      patch[0x3c]) *
                              256);
                set_glide(v, destination, prog[sound_5a7e9117::PROGRAM_GLIDE_OFFSET], bank);
            }
            for (unsigned i = 0; i < 3; ++i)
                initialize_envelope(v.envelope[i], patch + size_t(i) * 20, v, i == 2, bank);
            v.lfo_target = int16_t((patch[0x66] & 127) << 8);
            v.lfo_phase = (patch[0x66] & 128) ? 0 : noise;
            v.lfo_step =
                int16_t((uint32_t(v.lfo_target) *
                         bank.rom.read_u16(sound_5a7e9117::RATE_TABLE + (patch[0x6a] & 0xf0))) >>
                        16);
            v.filter.mode = uint8_t((patch[0x44] >> 6) & 3);
            unsigned period = (patch[0x64] & 0xf0) >> 1;
            period = (period | (period >> 4));
            v.random_period = v.random_countdown = uint16_t((-int(period)) & 127);
            noise_history = uint16_t(noise_history + noise);
            noise = uint16_t(noise + noise_history);
            v.random = v.previous_random = s16(noise);
        }
        v.split = bank.split_for(patch, layer, direct, v.pitch, v.selector, n.channel.program);
        unsigned id = (direct ? patch[4] : patch[0x6c]) & 127;
        if (direct && id == 0x78)
            throw std::runtime_error("ROM direct sample78 enters infinite branch c1968e");
        uint32_t bit = uint32_t(v.selector & 1) << 28;
        unsigned board = ((v.selector >> 1) & bank.board_bank_mask) << 20;
        uint8_t control = (!direct && id >= 0x6f && id < 0x78)
                              ? 0x0b
                              : bank.rom.read_byte(sound_5a7e9117::SPLIT_MODE_TABLE + v.split.mode);
        v.reverse = ((direct ? patch[4] : patch[0x6c]) & 128) && id < 0x54;
        if (v.reverse) control = 0x43;
        uint32_t start = v.reverse ? v.split.accumulator : v.split.start;
        uint32_t phase = v.reverse ? v.split.end : v.split.accumulator;
        uint32_t end = v.split.end;
        if (id >= 0x54 && id < 0x6f) phase &= 0xfffffe00;
        if (!direct && id < 0x78) phase &= 0xffffff00;
        if (!direct && id >= 0x78) {
            uint32_t d0 = patch[0x72];
            uint32_t d1 = std::min(uint32_t(d0 + patch[0x70]), 0xfau);
            d0 *= 14;
            d1 *= 14;
            const uint32_t split_table = sound_5a7e9117::SPECIAL_SPLIT_TABLE;
            uint32_t d4 = longword(bank.rom.pointer(split_table + d0 + 2, 4));
            if (d4 == 0x1dcaa07f) {
                d0 += 14;
                d4 = longword(bank.rom.pointer(split_table + d0 + 2, 4));
                if (d0 == d1) d1 += 14;
            }
            uint32_t d5 = longword(bank.rom.pointer(split_table + d1 + 2, 4));
            if (d5 == 0x1dcaa07f)
                d5 = longword(
                    bank.rom.pointer(split_table + d1 + sound_5a7e9117::SPLIT_RECORD_STRIDE, 4));
            v.reverse = (patch[0x6c] & 128) != 0;
            control = v.reverse ? 0x4b : 0x0b;
            start = d4;
            end = d5;
            phase = v.reverse ? d5 : d4;
            if (v.reverse) start += sound_5a7e9117::REVERSE_START_OFFSET;
        }
        v.loop = (control & 8) != 0;
        v.bidirectional = (control & 16) != 0;
        v.start = double(board) + double((start | bit) & 0x1fffffe0) / 512;
        v.end = double(board) + double((end | bit) & 0x1fffffe0) / 512;
        v.position = double(board) + double((phase | bit) & 0x1fffffff) / 512;
        if (!direct && id < 0x54) {
            int amount = s8(patch[0x70]) * n.velocity + (patch[0x72] << 7);
            if (amount > 0) {
                amount = std::min(32767, amount * 2);
                int length = s16((v.split.end - v.split.accumulator) >> 10);
                int32_t offset = wrap_product(int64_t(length) * amount) >> 5;
                v.position += double(v.reverse ? -offset : offset) / 512;
                v.position = std::clamp(v.position, v.start, v.end);
            }
        }
        if (!direct && id >= 0x6f && id < 0x78) {
            // c17e8c loads ACC from this bank's probe. c17fbe leaves START/END
            // unchanged, including when the physical slot was previously free.
            v.boundary = true;
            v.loop = true;
            v.start = double(board) + (old_hardware ? old_start : double(bank.reset_start) / 512);
            v.end = double(board) + (old_hardware ? old_end : double(bank.reset_end) / 512);
            v.position = double(board) + double(bank.banks[v.selector].probe & 0x1fffffff) / 512;
        }
        v.active = v.hardware_valid = true;
        move_before_cursor(index);
        if (!direct) update_lfo(v, bank);
        parameters(v, true, n.tick);
        std::array<uint16_t, 4> onset{v.left_volume, v.right_volume, v.k1, v.k2};
        if (!direct) {
            bool ended = false;
            for (unsigned i = 0; i < 3; ++i) ended |= update_envelope(v, i);
            if (ended) {
                v.active = false;
                return;
            }
            update_lfo(v, bank);
            update_random(v, noise, noise_history);
            advance_glide(v);
            parameters(v, false, n.tick, false); // c181da calls c18372 before key-on.
        }
        emit(v, VoiceEvent::Kind::Start, n.tick, &onset);
        if (!direct) emit(v, VoiceEvent::Kind::Parameters, n.tick);
    }
    // Starts a layer at `start`: now when the driver is idle, otherwise through the queue so that the voice,
    // its envelopes and its events carry the delayed tick.
    void launch(const Note &n, const uint8_t *prog, const uint8_t *patch, unsigned layer,
                bool direct, uint64_t start) {
        if (start == n.tick && queue.empty()) {
            begin(n, prog, patch, layer, direct);
            return;
        }
        Pending q{n, prog, patch, start, uint8_t(layer), true, false, direct};
        if (!queue.push(q, n.instance, n.tick, start))
            throw std::runtime_error("HLE driver key-on queue exhausted");
    }
    void service_queue(uint64_t limit) {
        queue.service(
            limit,
            [&](const Pending &q, uint64_t start) {
                Note n = q.note;
                n.tick = start;
                begin(n, q.program, q.patch, q.layer, q.direct);
            },
            [&](uint64_t instance, uint64_t at) {
                for (Voice &v : voices)
                    if (v.active && v.owner.instance == instance) release(v, at);
            });
    }
    void note_on(const Note &submitted) {
        Note n = submitted;
        n.velocity &= 127; // c16faa clears the velocity flag bit.
        const uint8_t *prog = bank.program(n.channel.program);
        if (!prog) return;
        unsigned enabled = bank.programs[program_slot(n.channel.program)].enabled;
        if (!enabled) {
            int key = s8(uint8_t(n.kernel_key + n.channel.parameters[0x35]));
            for (unsigned i = 0; i < 17; ++i) {
                const uint8_t *p = prog + size_t(i) * 20;
                if (key >= s8(p[10]) && key <= s8(p[12])) {
                    launch(n, prog, p, i, true, driver_clock.start(n.tick, 1));
                    return;
                }
            }
            return;
        }
        if ((n.channel.parameters[0x32] & 0x30) == 0x30) return;
        if (n.channel.parameters[0x1c]) enabled = n.channel.parameters[0x1c];
        unsigned immediate = 0;
        for (unsigned layer = 0; layer < 3; ++layer)
            if ((enabled & (1u << layer)) && !prog[size_t(layer) * 0x76 + 0x74]) ++immediate;
        const uint64_t start = immediate ? driver_clock.start(n.tick, immediate) : n.tick;
        for (unsigned layer = 3; layer--;) {
            if (!(enabled & (1u << layer))) continue;
            const uint8_t *p = prog + size_t(layer) * 0x76;
            if (p[0x74]) {
                auto slot = std::find_if(pending.begin(), pending.end(),
                                         [](const Pending &x) { return !x.active; });
                if (slot == pending.end())
                    throw std::runtime_error("HLE delayed-note pool exhausted");
                *slot = {n,
                         prog,
                         p,
                         n.tick + uint64_t(p[0x74]) * TIMER_TICKS,
                         uint8_t(layer),
                         true,
                         p[0x74] == 0xfb};
            } else launch(n, prog, p, layer, false, start);
        }
    }
    void release(Voice &v, uint64_t at) {
        if (v.released) return;
        v.released = true;
        v.release_serial = ++serial;
        emit(v, VoiceEvent::Kind::Release, at);
        if (v.direct) {
            if (v.patch[0] & 128) return;
            start_direct_release(v.envelope[2], v, bank);
        } else {
            for (Envelope &e : v.envelope) {
                if (e.flags & 64) continue;
                e.done = false;
                e.stage = 4;
                segment(e, 0, e.rates[3], bank);
            }
            if (!(v.envelope[2].flags & 64) && v.envelope[2].level() == 0) {
                stop(v, at);
                return;
            }
            if (v.boundary) {
                uint32_t value = v.split.end;
                unsigned board = (v.selector >> 1) << 20;
                if (value & 0x80000000) {
                    v.start = double(board) + double(value & 0x1fffffe0) / 512;
                    v.position = v.end;
                    v.reverse = true;
                } else {
                    v.end = double(board) + double(value & 0x1fffffe0) / 512;
                    v.position = v.start;
                }
                v.loop = false;
            }
        }
    }
    void note_off(uint64_t instance, uint64_t at) {
        queue.note_off(instance, at);
        for (Pending &p : pending)
            if (p.active && p.note.instance == instance) {
                if (p.release_trigger) {
                    Note n = p.note;
                    n.tick = at;
                    begin(n, p.program, p.patch, p.layer, false);
                }
                p.active = false;
            }
        for (Voice &v : voices)
            if (v.active && v.owner.instance == instance) release(v, at);
    }
    void channel_update(uint8_t sequence, uint8_t track, const Channel &c, uint64_t at) {
        queue.for_each([&](Pending &q) {
            if (q.note.sequence == sequence && q.note.track == track) q.note.channel = c;
        });
        for (Pending &p : pending)
            if (p.active && p.note.sequence == sequence && p.note.track == track)
                p.note.channel = c;
        for (Voice &v : voices)
            if (v.active && v.owner.sequence == sequence && v.owner.track == track) {
                v.owner.channel = c;
                parameters(v, false, at);
            }
    }
    void reset(uint64_t at) {
        for (unsigned i = 0; i < 32; ++i) {
            stop(voices[i], at);
            voices[i] = Voice{};
            voices[i].slot = uint8_t(i);
        }
        for (Pending &p : pending) p.active = false;
        queue.clear();
        driver_clock.reset(at);
        for (unsigned i = 0; i < 32; ++i) {
            next[i] = uint8_t((i + 1) % 32);
            voice_prev[i] = uint8_t((i + 31) % 32);
        }
        tick = at;
        tick_remainder = 0;
        timer = (at / TIMER_TICKS + 1) * TIMER_TICKS;
        cursor = 0;
        budget_slot = 0;
        noise = bank.rom.read_u16(sound_5a7e9117::NOISE_SEED_PTR);
        noise_history = bank.rom.read_u16(sound_5a7e9117::NOISE_HISTORY_PTR);
        serial = 0;
        buffered_events.clear();
        buffering_events = false;
    }
    void service(uint64_t at) {
        for (Pending &p : pending)
            if (p.active && !p.release_trigger && p.tick <= at) {
                Note n = p.note;
                n.tick = p.tick;
                p.active = false;
                launch(n, p.program, p.patch, p.layer, false, driver_clock.start(n.tick, 1));
            }
        unsigned count = bank.budgets[budget_slot];
        budget_slot = uint8_t((budget_slot + 1) % 12);
        while (count--) {
            unsigned index = cursor;
            cursor = next[cursor];
            Voice &v = voices[index];
            if (!v.active) continue;
            bool ended = false;
            if (v.direct) ended = update_envelope(v, 2);
            else {
                for (unsigned i = 0; i < 3; ++i) ended |= update_envelope(v, i);
                update_lfo(v, bank);
                update_random(v, noise, noise_history);
            }
            if (ended) {
                stop(v, at);
                continue;
            }
            advance_glide(v);
            parameters(v, false, at);
        }
    }
    double sample(size_t at) const {
        if (bank.samples.empty()) return 0;
        return int16_t(bank.samples[at & (bank.samples.size() - 1)]);
    }
    void render_voice_block(Voice &v, size_t chunk_start, size_t chunk_frames,
                            double samples_limit) {
        const double length = v.end - v.start, increment = v.increment;
        auto fill_remaining = [&](size_t start_row) {
            for (size_t idx = start_row; idx < chunk_frames; ++idx) {
                input[idx] = 0.f;
                gain_left[idx] = 0.f;
                gain_right[idx] = 0.f;
                a1[idx] = float(v.filter.a1);
                a2[idx] = float(v.filter.a2);
                high_pole[idx] = float(v.filter.high_pole);
            }
        };
        size_t r = 0;
        while (r < chunk_frames) {
            size_t n = 0;
            const bool ramping = v.gain_frames > 0;
            if (!v.sample_finished && v.filter.ramp == 0 &&
                (!ramping ||
                 (v.left_segment.frames > 0 && v.right_segment.frames > 0 && v.gain_frames > 1))) {
                bool pos_ok = false;
                double pos_bound = 0;
                if (!v.reverse) {
                    if (v.position >= 0 && v.position < samples_limit - 1 &&
                        v.end < samples_limit - 1) {
                        if (increment > 0) {
                            pos_bound = std::floor((v.end - v.position) / increment) - 1.0;
                            pos_ok = (pos_bound >= 1.0);
                        }
                    }
                } else {
                    if (v.position >= 0 && v.start >= 0 && v.position < samples_limit - 1) {
                        if (increment > 0) {
                            pos_bound = std::floor((v.position - v.start) / increment) - 1.0;
                            pos_ok = (pos_bound >= 1.0);
                        }
                    }
                }
                if (pos_ok) {
                    n = chunk_frames - r;
                    if (ramping) {
                        n = std::min(n, size_t(v.left_segment.frames));
                        n = std::min(n, size_t(v.right_segment.frames));
                        n = std::min(n, size_t(v.gain_frames - 1));
                    }
                    n = std::min(n, size_t(pos_bound));
                }
            }
            const bool slow = n < 1;
            if (slow) {
                const size_t frame = chunk_start + r;
                current_frame = frame;
                const uint64_t frame_tick = frame_ticks[frame];
                if (v.gain_frames) {
                    v.left += v.step_left;
                    v.right += v.step_right;
                    if (!--v.gain_frames) {
                        v.left = v.target_left;
                        v.right = v.target_right;
                        v.gain_dirty = true;
                    } else {
                        v.left_gain = ramp_gain(v.left, v.step_left, v.left_segment);
                        v.right_gain = ramp_gain(v.right, v.step_right, v.right_segment);
                        v.gain_dirty = false;
                    }
                }
                if (v.sample_finished) {
                    input[r] = 0.f;
                    gain_left[r] = 0.f;
                    gain_right[r] = 0.f;
                    a1[r] = float(v.filter.a1);
                    a2[r] = float(v.filter.a2);
                    high_pole[r] = float(v.filter.high_pole);
                    ++r;
                    continue;
                }
                if (v.position < 0 || v.position >= samples_limit) {
                    stop(v, frame_tick);
                    fill_remaining(r);
                    break;
                }
                const size_t index = size_t(v.position);
                const double fraction = v.position - double(index);
                const double first =
                    (!v.loop && v.reverse && index < size_t(v.start)) ? 0.0 : sample(index);
                const double second =
                    (!v.loop && !v.reverse && index + 1 >= size_t(v.end)) ? 0.0 : sample(index + 1);
                input[r] = interpolate_sample(first, second, fraction);
                v.filter.advance();
                a1[r] = float(v.filter.a1);
                a2[r] = float(v.filter.a2);
                high_pole[r] = float(v.filter.high_pole);
                if (v.gain_dirty) {
                    v.left_gain = interpolated_gain(v.left);
                    v.right_gain = interpolated_gain(v.right);
                    v.gain_dirty = false;
                }
                const float scale = 1.f / 524288.f;
                gain_left[r] = v.left_gain * scale;
                gain_right[r] = v.right_gain * scale;
                if (advance_voice_position(v.position, v.reverse, increment, v.start, v.end, length,
                                           v.loop, v.bidirectional) == SampleAdvance::Ended) {
                    end_sample(v, frame_tick);
                    fill_remaining(r + 1);
                    break;
                }
                ++r;
            } else {
                const float a1c = float(v.filter.a1);
                const float a2c = float(v.filter.a2);
                const float hpc = float(v.filter.high_pole);
                double pos = v.position;
                const double inc = v.reverse ? -increment : increment;
                if (ramping) {
                    double v_left = v.left, v_right = v.right;
                    double seg_l_val = v.left_segment.value;
                    const double seg_l_slope = v.left_segment.slope;
                    double seg_r_val = v.right_segment.value;
                    const double seg_r_slope = v.right_segment.slope;
                    const double step_l = v.step_left, step_r = v.step_right;
                    const float scale = 1.f / 524288.f;
                    for (size_t k = 0; k < n; ++k) {
                        const size_t idx = r + k;
                        const size_t i = size_t(pos);
                        const double frac = pos - double(i);
                        input[idx] = interpolate_sample(int16_t(bank.samples[i]),
                                                        int16_t(bank.samples[i + 1]), frac);
                        pos += inc;
                        a1[idx] = a1c;
                        a2[idx] = a2c;
                        high_pole[idx] = hpc;
                        v_left += step_l;
                        v_right += step_r;
                        seg_l_val += seg_l_slope;
                        seg_r_val += seg_r_slope;
                        gain_left[idx] = float(seg_l_val) * scale;
                        gain_right[idx] = float(seg_r_val) * scale;
                    }
                    v.left = v_left;
                    v.right = v_right;
                    v.left_segment.frames -= unsigned(n);
                    v.left_segment.value = seg_l_val;
                    v.right_segment.frames -= unsigned(n);
                    v.right_segment.value = seg_r_val;
                    v.gain_frames -= unsigned(n);
                    v.left_gain = float(seg_l_val);
                    v.right_gain = float(seg_r_val);
                    v.gain_dirty = false;
                } else {
                    if (v.gain_dirty) {
                        v.left_gain = interpolated_gain(v.left);
                        v.right_gain = interpolated_gain(v.right);
                        v.gain_dirty = false;
                    }
                    const float gl = v.left_gain * (1.f / 524288.f);
                    const float gr = v.right_gain * (1.f / 524288.f);
                    for (size_t k = 0; k < n; ++k) {
                        const size_t idx = r + k;
                        const size_t i = size_t(pos);
                        const double frac = pos - double(i);
                        input[idx] = interpolate_sample(int16_t(bank.samples[i]),
                                                        int16_t(bank.samples[i + 1]), frac);
                        pos += inc;
                        a1[idx] = a1c;
                        a2[idx] = a2c;
                        high_pole[idx] = hpc;
                        gain_left[idx] = gl;
                        gain_right[idx] = gr;
                    }
                }
                v.position = pos;
                current_frame = chunk_start + r + n - 1;
                r += n;
            }
        }
    }

    void render(MutableAudioBlockSpan<8> buses) {
        const size_t frames = buses.size();
        if (!frames) return;
        if (frame_ticks.size() < frames) frame_ticks.resize(frames);
        uint64_t cur_tick = tick;
        uint32_t cur_rem = tick_remainder;
        for (size_t f = 0; f < frames; ++f) {
            frame_ticks[f] = cur_tick;
            cur_rem += MAIN_CLOCK;
            cur_tick += cur_rem / SAMPLE_RATE;
            cur_rem %= SAMPLE_RATE;
        }
        const double samples_limit = double(bank.samples.size());
        size_t frame_start = 0;
        while (frame_start < frames) {
            const uint64_t now = frame_ticks[frame_start];
            for (;;) {
                const bool timer_due = timer <= now;
                const uint64_t queued = queue.next_event();
                const bool queue_due = queued <= now;
                if (!timer_due && !queue_due) break;
                tick = now;
                if (queue_due && (!timer_due || queued <= timer))
                    service_queue(timer_due ? timer : now);
                else {
                    service(timer);
                    timer += TIMER_TICKS;
                }
            }
            size_t frame_end = frame_start + 1;
            const uint64_t horizon = std::min(timer, queue.next_event());
            while (frame_end < frames && frame_ticks[frame_end] < horizon) ++frame_end;
            std::fill_n(buses.data() + frame_start * 8, (frame_end - frame_start) * 8, 0.f);
            if (observer) {
                buffering_events = true;
                buffered_events.clear();
            }
            for (size_t chunk_start = frame_start; chunk_start < frame_end;
                 chunk_start += VOICE_BLOCK_FRAMES) {
                const size_t chunk_frames = std::min(VOICE_BLOCK_FRAMES, frame_end - chunk_start);
                float *ins[6] = {input, a1, a2, high_pole, gain_left, gain_right};
                for (unsigned vi = 0; vi < 32; ++vi) {
                    Voice &v = voices[vi];
                    if (!v.active) continue;
                    *mode_zone[vi] = float(v.filter.mode);
                    current_voice = vi;

                    render_voice_block(v, chunk_start, chunk_frames, samples_limit);

                    voice_filters[vi].compute(int(chunk_frames), ins, filter_out.data());
                    if (auto bus = otis_target_bus(v.pair)) {
                        buses.subspan(chunk_start, chunk_frames).slice(*bus) += filter_out;
                    }
                }
            }
            if (observer) {
                buffering_events = false;
                if (!buffered_events.empty()) {
                    std::stable_sort(buffered_events.begin(), buffered_events.end(),
                                     [](const BufferedEvent &a, const BufferedEvent &b) {
                                         return a.frame != b.frame ? a.frame < b.frame
                                                                   : a.voice < b.voice;
                                     });
                    for (const auto &be : buffered_events) observer(be.event);
                    buffered_events.clear();
                }
            }
            float *sub_out = buses.data() + frame_start * 8;
            const size_t sub_floats = (frame_end - frame_start) * 8;
            for (size_t i = 0; i < sub_floats; ++i)
                sub_out[i] = std::clamp(sub_out[i], -1.f, 1.f - 1.f / 524288.f);
            frame_start = frame_end;
        }
        tick = cur_tick;
        tick_remainder = cur_rem;
    }

    std::optional<ProgramInfo> describe_program(uint16_t id) const {
        return bank.describe_program(id);
    }
};

Synth::Synth(std::span<const uint8_t> sound, std::span<const uint16_t> samples)
    : m_impl(std::make_unique<Impl>(sound, samples)) {}
Synth::~Synth() = default;
void Synth::note_on(const Note &n) { m_impl->note_on(n); }
void Synth::note_off(uint64_t instance, uint64_t tick) { m_impl->note_off(instance, tick); }
void Synth::channel_update(uint8_t sequence, uint8_t track, const Channel &c, uint64_t tick) {
    m_impl->channel_update(sequence, track, c, tick);
}
void Synth::reset(uint64_t tick) { m_impl->reset(tick); }
void Synth::set_bank(uint8_t bank) { m_impl->set_bank(bank); }
void Synth::render(MutableAudioBlockSpan<8> buses) { m_impl->render(buses); }
void Synth::set_observer(std::function<void(const VoiceEvent &)> observer) {
    m_impl->observer = std::move(observer);
}
std::optional<ProgramInfo> Synth::describe_program(uint16_t program) const {
    return m_impl->describe_program(program);
}
uint8_t Synth::bend_range() const { return m_impl->bank.bend_range; }
bool Synth::validate(std::span<const uint8_t> sound, std::span<const uint16_t> samples) noexcept {
    try {
        Synth synth(sound, samples);
        return true;
    } catch (...) {
        return false;
    }
}
} // namespace f3rt::hle
