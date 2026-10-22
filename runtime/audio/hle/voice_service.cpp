#include "audio/hle/voice_service.hpp"
#include "audio/hle/sound_rom_5a7e9117.hpp"
#include "f3rt/bytes.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace f3rt::hle {
using f3rt::s16;
using f3rt::s8;

namespace {
inline uint32_t longword(const uint8_t *p) { return read_be32(p); }

int16_t source(const Voice &v, ModSource selector) {
    const TrackParameters &parameters = v.track->config.parameters;
    switch (selector) {
    case ModSource::Lfo:
        return v.lfo;
    case ModSource::PitchEnvelope:
        return v.envelope[Voice::pitch_envelope].level();
    case ModSource::FilterEnvelope:
        return v.envelope[Voice::filter_envelope].level();
    case ModSource::AmplitudeEnvelope:
        return v.envelope[Voice::amplitude_envelope].level();
    case ModSource::Random:
        return v.random;
    case ModSource::PreviousRandom:
        return v.previous_random;
    case ModSource::Velocity:
        return s16(v.owner.velocity * 257);
    case ModSource::Key:
        return s16((int(v.effective_key) - 60) * 256);
    case ModSource::Mod1A:
        return s16(parameters.mod_1a());
    case ModSource::Mod12:
        return s16(parameters.mod_12());
    case ModSource::Bend:
        return s16(parameters.bend());
    case ModSource::Mod10:
        return s16(parameters.mod_10());
    case ModSource::Mod0C:
        return s16(parameters.mod_0c());
    case ModSource::Pressure:
        return s16(v.pressure * 256);
    case ModSource::Full:
        return 0x7fff;
    case ModSource::Off:
        return 0;
    }
    return 0;
}

void update_lfo(Voice &v, const SoundBank &bank) {
    const Layer layer = v.patch->layer();
    v.lfo_depth = s16(v.lfo_depth + v.lfo_step);
    if (uint16_t(v.lfo_depth) > uint16_t(v.lfo_target)) {
        v.lfo_step = 0;
        v.lfo_depth = v.lfo_target;
    }
    v.lfo_phase = uint16_t(v.lfo_phase + bank.phase_rates[layer.lfo_rate()]);
    unsigned wave = (unsigned(layer.lfo_wave()) << 8) | (v.lfo_phase >> 8);
    int modulation = source(v, layer.lfo_source());
    int depth = std::clamp(int(v.lfo_depth) + modulation, 0, 32767);
    v.lfo = s16((depth * int(s8(bank.rom.read_byte(sound_5a7e9117::LFO_WAVE_TABLE + wave)))) >> 7);
}

// Volume, pan, bend, filter, and pitch registers for one voice. The ring writes them after a step. A track event
// write does not step.
void voice_parameters(SoundBank &bank, Voice &v, bool initial) {
    const bool direct = v.patch->is_direct;
    const DirectPatch direct_patch = v.patch->direct();
    const Layer layer_patch = v.patch->layer();
    const TrackParameters &c = v.track->config.parameters;
    v.owner.program = v.track->config.program; // program in force, as the note's channel snapshot held it
    int common =
        bank.attenuation[c.volume_index()] + bank.attenuation[v.track->sequence_volume & 127];
    common += c.expression_offset();
    int pan_position;
    if (direct) {
        int curve = int(velocity_curve_index(direct_patch.velocity_curve(), v.owner.velocity));
        int sensitivity = velocity_level_scale(direct_patch.velocity_level());
        int scale = s16(0x4000 - (128 - bank.velocity_curves[unsigned(curve)]) * sensitivity) >> 6;
        if (initial) {
            Envelope &e = v.envelope[Voice::amplitude_envelope];
            e.position = (uint32_t(uint16_t(direct_patch.level() * scale)) << 16) | 0x7fff;
            e.slope =
                -s16(bank.rom.read_u16(sound_5a7e9117::RATE_TABLE + 2 * direct_patch.decay_rate()));
            e.target = (uint32_t(uint16_t((e.position >> 16) ^ 0x8000))) << 16;
        }
        pan_position = c.pan() == 128 ? direct_patch.centered_pan()
                                      : (c.signed_pan() >> 1) + 64;
        int level = (v.envelope[Voice::amplitude_envelope].level() >> 5) +
                    (direct_patch.level_plus_168() ? 0xa8 : 0);
        v.left_volume =
            volume_target((common + bank.pan[unsigned(127 - pan_position)]) * 4 + level, 0xfa0);
        v.right_volume =
            volume_target((common + bank.pan[unsigned(pan_position)]) * 4 + level, 0xfa0);
        int filter_index = direct_filter_index(direct_patch.filter_index(), v.owner.velocity);
        v.filter_cutoff_low = v.filter_cutoff_high = bank.coefficient(filter_index);
    } else {
        const ModSource amp = layer_patch.amplitude_source();
        int amp_source = source(v, amp), base;
        if (amp == ModSource::Off) base = 254;
        else {
            int x = amp == ModSource::Lfo ? (amp_source >> 1) + 0x4000 : std::max(0, amp_source);
            int factor = layer_patch.amplitude_source_depth();
            int value = (x * factor) >> 15;
            if (factor >= 0) value -= factor;
            base = (value + 127) * 2;
        }
        base += bank.attenuation[layer_patch.level()] + (layer_patch.level_plus_43() ? 43 : 0) +
                common + bank.attenuation[v.weight & 127];
        int pan_value = c.signed_pan();
        if (pan_value <= -127) pan_value = layer_patch.default_pan();
        pan_position = (pan_value >> 1) + 64;
        int env = v.envelope[Voice::amplitude_envelope].level() >> 5;
        v.left_volume =
            volume_target((base + bank.pan[unsigned(127 - pan_position)]) * 4 + env, 0x1b64);
        v.right_volume =
            volume_target((base + bank.pan[unsigned(pan_position)]) * 4 + env, 0x1b64);
        int filter_env = v.envelope[Voice::filter_envelope].level();
        int key = int(v.effective_key) - 60;
        int first = (filter_env * layer_patch.k1_envelope() +
                     source(v, layer_patch.filter_source()) * layer_patch.filter_source_depth()) >>
                    12;
        first += (key * layer_patch.k1_key()) >> 3;
        first += layer_patch.k1_base() * 8;
        int second = (filter_env * layer_patch.k2_envelope() +
                      (layer_patch.k2_apply_source() ? source(v, layer_patch.filter_source()) : 0) *
                          layer_patch.filter_source_depth()) >>
                     12;
        second += (key * layer_patch.k2_key()) >> 3;
        second += layer_patch.k2_base() * 8;
        v.filter_cutoff_low = bank.coefficient(first);
        v.filter_cutoff_high = bank.coefficient(second, layer_patch.k2_table());
        int pitch_env = s16(v.envelope[Voice::pitch_envelope].level() -
                            (int(layer_patch.pitch().pitch_zero()) << 8));
        int64_t pitch_mod = int64_t(pitch_env) * layer_patch.pitch_envelope_depth() +
                            int64_t(v.lfo) * layer_patch.lfo_pitch_depth();
        int depth = layer_patch.pitch_source_depth();
        int multiplier =
            bank.rom.read_u16(sound_5a7e9117::PITCH_MOD_TABLE + unsigned(std::abs(depth)) * 2);
        if (depth < 0) multiplier = -s16(multiplier);
        pitch_mod += (int64_t(source(v, layer_patch.pitch_source())) * multiplier) >> 4;
        int bend = s16(c.bend() - TrackParameters::bend_center);
        pitch_mod >>= 4;
        pitch_mod += int64_t(bend) * bank.bend_range;
        pitch_mod >>= 6;
        v.pitch = s16(v.glide_target + v.split.tuning + int(pitch_mod));
        if (v.split.mode & 128) {
            int amount =
                (int(layer_patch.start_amount()) * source(v, layer_patch.start_source())) >> 7;
            amount = std::clamp(amount + (int(layer_patch.start_offset()) << 8), 0, 32767);
            int delta = s16((int64_t(amount) * s16(v.split.accumulator >> 16)) >> 16);
            int span = int16_t(uint16_t((v.split.end - v.split.start) >> 4));
            uint32_t offset = uint32_t(uint32_t(uint16_t(span)) * bank.frequency(delta)) >> 6;
            uint32_t start = (v.split.end - offset) | uint32_t((v.selector & 1) << 28);
            v.start = double((v.selector >> 1) << 20) + double(start & 0x1fffffe0) / 512;
            v.pitch = s16(v.pitch + delta);
        }
        v.frequency = bank.frequency(v.pitch);
    }
    if (direct) v.frequency = bank.frequency(v.pitch + v.split.tuning);
    v.increment = double(v.frequency) * NATIVE_RATE / (1024.0 * SAMPLE_RATE);
    unsigned choice = c.pair_select();
    if (choice >= 3) {
        if (choice == 5) choice = 3;
        else choice = direct ? direct_patch.pair_choice() : layer_patch.pair_choice();
    }
    v.pair = uint8_t((choice + 1) & 3);
    v.target_left = v.left_volume;
    v.target_right = v.right_volume;
    if (initial) {
        v.left = v.target_left;
        v.right = v.target_right;
        v.gain_frames = 0;
    } else
        v.gain_frames = (v.left == v.target_left && v.right == v.target_right) ? 0 : VOLUME_RAMP;
    if (v.gain_frames) {
        v.step_left = (v.target_left - v.left) / v.gain_frames;
        v.step_right = (v.target_right - v.right) / v.gain_frames;
    }
    v.left_segment.frames = v.right_segment.frames = 0;
    v.gain_dirty = true;
    v.filter.targets(v.filter_cutoff_low, v.filter_cutoff_high, initial);
}

} // namespace

VoiceService::VoiceService(SoundBank &bank) : m_bank(bank) {
    m_buffered.reserve(32);
    reset(0);
}

void VoiceService::set_observer(std::function<void(const VoiceEvent &)> observer) {
    m_observer = std::move(observer);
}

void VoiceService::reset(uint64_t tick) {
    for (uint8_t i = 0; i < 32; ++i) {
        stop(m_voices[i], tick);
        m_voices[i] = Voice{};
        m_voices[i].slot = i;
    }
    for (Pending &p : m_pending) p.active = false;
    m_queue.clear();
    m_driver.reset(tick);
    for (unsigned i = 0; i < 32; ++i) {
        m_next[i] = uint8_t((i + 1) % 32);
        m_prev[i] = uint8_t((i + 31) % 32);
    }
    m_cursor = 0;
    m_budget_slot = 0;
    m_tick = (tick / TIMER_TICKS + 1) * TIMER_TICKS;
    m_noise = m_bank.rom.read_u16(sound_5a7e9117::NOISE_SEED_PTR);
    m_noise_history = m_bank.rom.read_u16(sound_5a7e9117::NOISE_HISTORY_PTR);
    m_serial = 0;
    m_buffered.clear();
    m_buffering = false;
}

void VoiceService::emit(const Voice &v, VoiceEvent::Kind kind, uint64_t tick, const Onset *onset) {
    if (!m_observer) return;
    VoiceEvent e{};
    e.kind = kind;
    e.instance = v.owner.instance;
    e.tick = tick;
    e.sequence = v.owner.sequence;
    e.track = uint8_t(v.owner.track + 1);
    e.key = v.owner.key;
    e.layer = v.patch->layer_index;
    e.output_pair = v.pair;
    e.sample_start = uint32_t(v.start);
    e.sample_end = uint32_t(v.end);
    e.frequency = v.frequency;
    e.left_volume = v.left_volume;
    e.right_volume = v.right_volume;
    e.k1 = v.filter_cutoff_low;
    e.k2 = v.filter_cutoff_high;
    if (onset) {
        e.left_volume = onset->left_volume;
        e.right_volume = onset->right_volume;
        e.k1 = onset->k1;
        e.k2 = onset->k2;
    }
    e.loop = v.loop;
    e.reverse = v.reverse;
    e.slot = v.slot;
    e.sounding_key = v.effective_key;
    e.velocity = v.owner.velocity;
    e.filter_mode = v.filter.mode;
    e.direct = v.patch->is_direct;
    e.program = v.owner.program;
    e.pitch = v.pitch;
    e.glide = v.glide_target;
    e.lfo = v.lfo;
    for (unsigned i = 0; i < 3; ++i) e.env[i] = v.envelope[i].level();
    if (m_buffering) m_buffered.push_back({m_current_frame, m_current_voice, e});
    else m_observer(e);
}

void VoiceService::begin_render() {
    if (!m_observer) return;
    m_buffering = true;
    m_buffered.clear();
}

void VoiceService::finish_render() {
    if (!m_buffering) return;
    m_buffering = false;
    std::stable_sort(m_buffered.begin(), m_buffered.end(),
                     [](const BufferedEvent &a, const BufferedEvent &b) {
                         return a.frame != b.frame ? a.frame < b.frame : a.voice < b.voice;
                     });
    for (const BufferedEvent &event : m_buffered) m_observer(event.event);
    m_buffered.clear();
}

void VoiceService::parameters(Voice &v, bool initial, uint64_t tick, bool notify) {
    voice_parameters(m_bank, v, initial);
    if (!initial && notify) emit(v, VoiceEvent::Kind::Parameters, tick);
}

bool VoiceService::update_envelope(Voice &v, unsigned which, uint64_t tick) {
    Envelope &e = v.envelope[which];
    if (e.done) return false;
    if (!e.advance()) return false;
    uint16_t level = uint16_t(e.target >> 16);
    if (e.slope <= 0) level ^= 0x8000;
    e.position = uint32_t(level) << 16;
    if (v.patch->is_direct) {
        // Timer expiry dispatches on the envelope stage (c1971c -> stage routine), not on note-off: stage 0 starts
        // the release ramp (c1983c: ends the voice if the level's top byte is already 0), stage 4 ends the voice
        // (c19888). A patch whose note-off is ignored (patch[0] bit 7) still decays when its timer expires.
        if (e.stage == 4 || (e.position >> 24) == 0) {
            e.done = true;
            return true;
        }
        start_direct_release(e, v, m_bank);
        return false;
    }
    if (e.stage == 4) {
        e.done = true;
        return which == Voice::amplitude_envelope;
    }
    if (e.intermediate) {
        e.intermediate = false;
        segment(e, e.levels[1], e.rates[0], m_bank);
        return false;
    }
    ++e.stage;
    if (e.stage == 1) segment(e, e.levels[2], e.rates[1], m_bank);
    else if (e.stage == 2) segment(e, e.levels[3], e.rates[2], m_bank);
    else if (e.flags & 0x40) {
        e.stage = 4;
        segment(e, 0, e.rates[3], m_bank);
    } else if (e.flags & 0x80) {
        e.stage = 0;
        segment(e, e.levels[1], e.rates[0], m_bank);
        if (which == Voice::amplitude_envelope && v.patch->layer().sample().ordinary()) {
            v.position = v.reverse ? v.end : accumulator_start(v);
            if (v.sample_finished) {
                v.sample_finished = false;
                emit(v, VoiceEvent::Kind::Start, tick);
            }
        }
    } else {
        e.done = true;
        return which == Voice::amplitude_envelope && e.level() == 0;
    }
    return false;
}

void VoiceService::move_before_cursor(unsigned i) {
    unsigned a = m_prev[i], b = m_next[i];
    m_next[a] = uint8_t(b);
    m_prev[b] = uint8_t(a);
    if (m_cursor == i) m_cursor = uint8_t(b);
    a = m_prev[m_cursor];
    m_next[a] = uint8_t(i);
    m_prev[i] = uint8_t(a);
    m_next[i] = m_cursor;
    m_prev[m_cursor] = uint8_t(i);
}

unsigned VoiceService::allocate(const NoteOnEvent &n, const Patch &patch, unsigned priority,
                                AllocationMode mode) {
    const unsigned layer = patch.layer_index;
    if (mode == AllocationMode::Legato || mode == AllocationMode::LegatoHold) {
        Voice *found = nullptr;
        for (Voice &v : m_voices)
            if (v.active && !v.released && !v.patch->is_direct && v.owner.sequence == n.sequence &&
                v.owner.track == n.track && v.patch->layer_index == layer)
                if (!found || v.serial < found->serial) found = &v;
        if (found) return unsigned(found - m_voices.data());
    }
    if (mode == AllocationMode::Retrigger) {
        for (Voice &v : m_voices)
            if (v.active && !v.released && !v.patch->is_direct && v.owner.sequence == n.sequence &&
                v.owner.track == n.track && v.patch->layer_index == layer) {
                release(v, n.tick);
                break;
            }
    }
    for (unsigned i = 32; i--;)
        if (!m_voices[i].active) return i;
    for (unsigned klass : std::array<unsigned, 3>{0, 64, 128}) {
        if (klass == 64 && priority == 0) break;
        if (klass == 128 && priority <= 64) break;
        for (bool released : std::array<bool, 2>{true, false}) {
            Voice *old = nullptr;
            for (Voice &v : m_voices)
                if (v.released == released && (klass == 128 || v.priority == klass)) {
                    if (!old) old = &v;
                    else {
                        const uint64_t v_k = released ? v.release_serial : v.serial;
                        const uint64_t o_k = released ? old->release_serial : old->serial;
                        if (v_k < o_k) old = &v;
                    }
                }
            if (old) {
                unsigned index = unsigned(old - m_voices.data());
                stop(*old, n.tick);
                return index;
            }
        }
    }
    return 32;
}

void VoiceService::begin(const NoteOnEvent &n, const Patch &patch, Track &track, uint64_t at) {
    const bool direct = patch.is_direct;
    const unsigned layer = patch.layer_index;
    const Layer layer_patch = patch.layer();
    const DirectPatch direct_patch = patch.direct();
    const Program program = patch.program_view();
    const int kernel = track.config.parameters.transposed_key(n.kernel_key);
    const int weight = direct ? 127 : m_bank.key_weight(layer_patch, kernel);
    if (weight < 0) return;
    if (!direct) {
        int threshold = layer_patch.velocity_gate();
        int boundary = (std::abs(threshold & ~15) * 9 / 8) & 0x7c;
        if (threshold < 0 ? n.velocity >= boundary : n.velocity < boundary) return;
    }
    const AllocationMode mode = direct ? AllocationMode::Poly : layer_patch.allocation();
    const unsigned priority = direct ? 64 : layer_patch.priority();
    int prior_pitch = 0;
    bool prior_found = false;
    if (mode == AllocationMode::Retrigger)
        for (const Voice &old : m_voices)
            if (old.active && old.owner.sequence == n.sequence && old.owner.track == n.track &&
                old.patch->layer_index == layer) {
                prior_pitch = old.glide_target;
                prior_found = true;
                break;
            }
    const unsigned index = allocate(n, patch, priority, mode);
    if (index == 32) return;
    Voice &v = m_voices[index];
    const bool legato = v.active &&
                        (mode == AllocationMode::Legato || mode == AllocationMode::LegatoHold) &&
                        !direct;
    const double old_board = double((v.selector >> 1) << 20);
    const double old_start = v.start - old_board, old_end = v.end - old_board;
    const bool old_hardware = v.hardware_valid;
    if (legato) {
        uint8_t velocity = v.owner.velocity;
        v.owner = n;
        v.owner.velocity = velocity;
        v.released = false;
        int16_t destination = layer_patch.pitch_word(kernel);
        set_glide(v, destination, program.glide(), m_bank);
        parameters(v, false, at);
        return;
    }
    stop(v, at);
    v = Voice{};
    v.slot = uint8_t(index);
    v.filter_epoch = ++m_filter_epoch;
    v.owner = n;
    v.patch = &patch;
    v.track = &track;
    v.priority = uint8_t(priority);
    v.allocation = uint8_t(mode);
    v.effective_key = uint8_t(std::clamp(kernel, 21, 108));
    v.weight = uint8_t(weight);
    v.serial = ++m_serial;
    if (direct) {
        int key = direct_patch.sounding_key(kernel, track.config.parameters.signed_transpose());
        v.pitch = s16(std::clamp(key * 256, 0, 0x7f00) | (direct_patch.fine_pitch() << 4));
    } else {
        v.glide_target = layer_patch.pitch_word(kernel);
        v.pitch = static_cast<int16_t>(std::clamp(int(v.glide_target), 0, 0x7f00));
        if (program.glide() && (mode == AllocationMode::Legato || prior_found)) {
            int16_t destination = v.glide_target;
            v.glide_target = prior_found ? s16(prior_pitch)
                                         : s16(uint8_t(n.previous_key +
                                                       layer_patch.coarse_pitch()) *
                                               256);
            set_glide(v, destination, program.glide(), m_bank);
        }
        initialize_envelope(v.envelope[Voice::pitch_envelope], layer_patch.pitch(), v, m_bank);
        initialize_envelope(v.envelope[Voice::filter_envelope], layer_patch.filter(), v, m_bank);
        initialize_envelope(v.envelope[Voice::amplitude_envelope], layer_patch.amplitude(), v,
                            m_bank);
        v.lfo_target = int16_t(layer_patch.lfo_depth() << 8);
        v.lfo_phase = layer_patch.lfo_phase_reset() ? 0 : m_noise;
        v.lfo_step =
            int16_t((uint32_t(v.lfo_target) * m_bank.rom.read_u16(sound_5a7e9117::RATE_TABLE +
                                                                layer_patch.lfo_fade_offset())) >>
                    16);
        v.filter.mode = uint8_t(layer_patch.filter_mode());
        v.random_period = v.random_countdown = layer_patch.random_countdown();
        m_noise_history = uint16_t(m_noise_history + m_noise);
        m_noise = uint16_t(m_noise + m_noise_history);
        v.random = v.previous_random = s16(m_noise);
    }
    v.split = direct ? m_bank.split_for(direct_patch, layer, v.pitch, v.selector, n.program)
                     : m_bank.split_for(layer_patch, layer, v.pitch, v.selector, n.program);
    const SampleKind sample = direct ? direct_patch.sample() : layer_patch.sample();
    const unsigned id = sample.id;
    if (direct && id == 0x78)
        throw std::runtime_error("ROM direct sample78 enters infinite branch c1968e");
    uint32_t bit = uint32_t(v.selector & 1) << 28;
    unsigned board = ((v.selector >> 1) & m_bank.board_bank_mask) << 20;
    uint8_t control = (!direct && sample.probe())
                          ? 0x0b
                          : m_bank.rom.read_byte(sound_5a7e9117::SPLIT_MODE_TABLE + v.split.mode);
    v.reverse = (direct ? direct_patch.reverse() : layer_patch.reverse()) && sample.ordinary();
    if (v.reverse) control = 0x43;
    uint32_t start = v.reverse ? v.split.accumulator : v.split.start;
    uint32_t phase = v.reverse ? v.split.end : v.split.accumulator;
    uint32_t end = v.split.end;
    if (sample.phase_aligned()) phase &= 0xfffffe00;
    if (!direct && !sample.table()) phase &= 0xffffff00;
    if (!direct && sample.table()) {
        uint32_t d0 = layer_patch.start_offset();
        uint32_t d1 = std::min(uint32_t(d0 + layer_patch.start_amount_byte()), 0xfau);
        d0 *= 14;
        d1 *= 14;
        const uint32_t split_table = sound_5a7e9117::SPECIAL_SPLIT_TABLE;
        uint32_t d4 = longword(m_bank.rom.pointer(split_table + d0 + 2, 4));
        if (d4 == 0x1dcaa07f) {
            d0 += 14;
            d4 = longword(m_bank.rom.pointer(split_table + d0 + 2, 4));
            if (d0 == d1) d1 += 14;
        }
        uint32_t d5 = longword(m_bank.rom.pointer(split_table + d1 + 2, 4));
        if (d5 == 0x1dcaa07f)
            d5 = longword(
                m_bank.rom.pointer(split_table + d1 + sound_5a7e9117::SPLIT_RECORD_STRIDE, 4));
        v.reverse = layer_patch.reverse();
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
    if (!direct && sample.ordinary()) {
        int amount =
            layer_patch.start_amount() * n.velocity + (int(layer_patch.start_offset()) << 7);
        if (amount > 0) {
            amount = std::min(32767, amount * 2);
            int length = s16((v.split.end - v.split.accumulator) >> 10);
            int32_t offset = wrap_product(int64_t(length) * amount) >> 5;
            v.position += double(v.reverse ? -offset : offset) / 512;
            v.position = std::clamp(v.position, v.start, v.end);
        }
    }
    if (!direct && sample.probe()) {
        // c17e8c loads ACC from this bank's probe. c17fbe leaves START/END
        // unchanged, including when the physical slot was previously free.
        v.boundary = true;
        v.loop = true;
        v.start = double(board) + (old_hardware ? old_start : double(m_bank.reset_start) / 512);
        v.end = double(board) + (old_hardware ? old_end : double(m_bank.reset_end) / 512);
        v.position = double(board) + double(m_bank.banks[v.selector].probe & 0x1fffffff) / 512;
    }
    v.active = v.hardware_valid = true;
    // Ring admission, then the opening step: a layered voice steps once here, a direct voice waits for the ring.
    move_before_cursor(v.slot);
    if (!direct) update_lfo(v, m_bank);
    parameters(v, true, at, false);
    const Onset onset{v};
    if (!direct) {
        bool ended = false;
        for (unsigned which :
             {Voice::pitch_envelope, Voice::filter_envelope, Voice::amplitude_envelope})
            ended |= update_envelope(v, which, at);
        if (ended) {
            v.active = false;
            return;
        }
        update_lfo(v, m_bank);
        update_random(v, m_noise, m_noise_history);
        advance_glide(v);
        parameters(v, false, at, false); // c181da calls c18372 before key-on.
    }
    emit(v, VoiceEvent::Kind::Start, at, &onset);
    if (!direct) emit(v, VoiceEvent::Kind::Parameters, at);
}

void VoiceService::launch(const NoteOnEvent &n, const Patch &patch, Track &track, uint64_t start) {
    if (start == n.tick && m_queue.empty()) {
        begin(n, patch, track, start);
        return;
    }
    Pending q{n, &patch, &track, start, true, false};
    if (!m_queue.push(q, n.instance, n.tick, start))
        throw std::runtime_error("HLE driver key-on queue exhausted");
}

void VoiceService::release(Voice &v, uint64_t tick) {
    if (v.released) return;
    v.released = true;
    v.release_serial = ++m_serial;
    emit(v, VoiceEvent::Kind::Release, tick);
    if (v.patch->is_direct) {
        if (v.patch->direct().ignore_note_off()) return;
        start_direct_release(v.envelope[Voice::amplitude_envelope], v, m_bank);
    } else {
        for (Envelope &e : v.envelope) {
            if (e.flags & 64) continue;
            e.done = false;
            e.stage = 4;
            segment(e, 0, e.rates[3], m_bank);
        }
        if (!(v.envelope[Voice::amplitude_envelope].flags & 64) &&
            v.envelope[Voice::amplitude_envelope].level() == 0) {
            stop(v, tick);
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

void VoiceService::stop(Voice &voice, uint64_t tick) {
    if (voice.active && !voice.sample_finished) emit(voice, VoiceEvent::Kind::Stop, tick);
    voice.active = false;
}

void VoiceService::end_sample(Voice &voice, uint64_t tick) {
    if (voice.allocation) {
        emit(voice, VoiceEvent::Kind::Stop, tick);
        voice.sample_finished = true;
    } else stop(voice, tick);
}

void VoiceService::release_instance(uint64_t instance, uint64_t tick) {
    for (Voice &v : m_voices)
        if (v.active && v.owner.instance == instance) release(v, tick);
}

void VoiceService::note_on(const NoteOnEvent &event, const PatchList &patches, Track &track) {
    NoteOnEvent note = event;
    note.velocity &= 127; // c16faa clears the velocity flag bit.
    unsigned immediate = 0;
    for (const Patch *patch : patches)
        if (!patch->delay_ticks) ++immediate;
    const uint64_t start = immediate ? m_driver.start(note.tick, immediate) : note.tick;
    for (const Patch *patch : patches) {
        if (!patch->delay_ticks) {
            launch(note, *patch, track, start);
            continue;
        }
        auto slot = std::find_if(m_pending.begin(), m_pending.end(),
                                 [](const Pending &p) { return !p.active; });
        if (slot == m_pending.end()) throw std::runtime_error("HLE delayed-note pool exhausted");
        *slot = {note,
                 patch,
                 &track,
                 note.tick + uint64_t(patch->delay_ticks) * TIMER_TICKS,
                 true,
                 patch->starts_on_release};
    }
}

void VoiceService::note_off(uint64_t instance, uint64_t tick) {
    m_queue.note_off(instance, tick);
    for (Pending &p : m_pending)
        if (p.active && p.note.instance == instance) {
            if (p.release_trigger) {
                NoteOnEvent n = p.note;
                n.tick = tick;
                begin(n, *p.patch, *p.track, tick);
            }
            p.active = false;
        }
    release_instance(instance, tick);
}

void VoiceService::service_queue(uint64_t tick) {
    m_queue.service(
        tick,
        [&](const Pending &p, uint64_t start) {
            NoteOnEvent note = p.note;
            note.tick = start;
            begin(note, *p.patch, *p.track, start);
        },
        [&](uint64_t instance, uint64_t at) {
            release_instance(instance, at);
        });
}

void VoiceService::refresh_track(uint8_t sequence, uint8_t track, uint64_t tick) {
    for (Voice &v : m_voices)
        if (v.active && v.owner.sequence == sequence && v.owner.track == track)
            parameters(v, false, tick);
}

void VoiceService::refresh_sequence(uint8_t sequence, uint64_t tick) {
    // Track-major, as the old per-track channel sync was: all voices of track 0, then track 1, ...
    for (uint8_t track = 0; track < TRACK_COUNT; ++track)
        for (Voice &v : m_voices)
            if (v.active && v.owner.sequence == sequence && v.owner.track == track)
                parameters(v, false, tick);
}

void VoiceService::service() {
    const uint64_t tick = m_tick;
    m_tick += TIMER_TICKS;
    // Delayed layered patches whose delay elapsed; the start still pays the driver's key-on cost.
    for (Pending &p : m_pending)
        if (p.active && !p.release_trigger && p.tick <= tick) {
            NoteOnEvent n = p.note;
            n.tick = p.tick;
            p.active = false;
            launch(n, *p.patch, *p.track, m_driver.start(n.tick, 1));
        }
    unsigned count = m_bank.budgets[m_budget_slot];
    m_budget_slot = uint8_t((m_budget_slot + 1) % 12);
    while (count--) {
        const unsigned index = m_cursor;
        m_cursor = m_next[m_cursor];
        Voice &v = m_voices[index];
        if (!v.active) continue;
        bool ended = false;
        if (v.patch->is_direct) ended = update_envelope(v, Voice::amplitude_envelope, tick);
        else {
            for (unsigned which :
                 {Voice::pitch_envelope, Voice::filter_envelope, Voice::amplitude_envelope})
                ended |= update_envelope(v, which, tick);
            update_lfo(v, m_bank);
            update_random(v, m_noise, m_noise_history);
        }
        if (ended) {
            stop(v, tick);
            continue;
        }
        advance_glide(v);
        parameters(v, false, tick);
    }
}

} // namespace f3rt::hle
