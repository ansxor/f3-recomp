#include "audio/hle/patch.hpp"
#include "audio/hle/sound_bank.hpp"
#include "audio/hle/track_service.hpp"
#include "audio/hle/voice.hpp"
#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace f3rt::hle {
using f3rt::s16;
using f3rt::s8;

namespace {

int16_t modulation(const Voice &v, ModSource selector) {
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

void update_lfo(Voice &v, const Layer &layer, const SoundBank &bank) {
    v.lfo_depth = s16(v.lfo_depth + v.lfo_step);
    if (uint16_t(v.lfo_depth) > uint16_t(v.lfo_target)) {
        v.lfo_step = 0;
        v.lfo_depth = v.lfo_target;
    }
    v.lfo_phase = uint16_t(v.lfo_phase + bank.phase_rates[layer.lfo_rate()]);
    unsigned wave = (unsigned(layer.lfo_wave()) << 8) | (v.lfo_phase >> 8);
    int depth = std::clamp(int(v.lfo_depth) + modulation(v, layer.lfo_source()), 0, 32767);
    v.lfo = s16((depth * int(s8(bank.rom.read_byte(sound_5a7e9117::LFO_WAVE_TABLE + wave)))) >> 7);
}

// The target may name the sign bit as a level; the position keeps the resolved level.
void settle_level(Envelope &e) {
    uint16_t level = uint16_t(e.target >> 16);
    if (e.slope <= 0) level ^= 0x8000;
    e.position = uint32_t(level) << 16;
}

// c1983c: ramp the level to 0 at the direct region's release rate.
void start_direct_release(Envelope &e, unsigned release_rate, const SoundBank &bank) {
    e.done = false;
    e.position &= 0xffff0000;
    e.slope =
        wrap_product(-int64_t(0x7f00) *
                     s16(bank.rom.read_u16(sound_5a7e9117::RATE_TABLE + 2 * release_rate)) * 2);
    e.target = e.slope <= 0 ? 0x80000000 : 0;
    e.stage = 4;
}

// Split addresses are word offsets inside the sample bank the selector named.
void apply_window(Voice &v, uint32_t bit, unsigned board, uint32_t start, uint32_t phase,
                  uint32_t end) {
    v.start = double(board) + double((start | bit) & 0x1fffffe0) / 512;
    v.end = double(board) + double((end | bit) & 0x1fffffe0) / 512;
    v.position = double(board) + double((phase | bit) & 0x1fffffff) / 512;
}

unsigned pair_choice(unsigned selected, unsigned patch) {
    if (selected < 3) return selected;
    return selected == 5 ? 3 : patch;
}

// Volume ramp, pair and filter targets shared by both kinds. The kind has already written the target
// volumes, the cutoff registers and the frequency.
void finish_parameters(Voice &v, bool initial, unsigned pair) {
    v.increment = double(v.frequency) * NATIVE_RATE / (1024.0 * SAMPLE_RATE);
    v.pair = uint8_t((pair + 1) & 3);
    v.target_left = v.left_volume;
    v.target_right = v.right_volume;
    if (initial) {
        v.left = v.target_left;
        v.right = v.target_right;
        v.gain_frames = 0;
    } else v.gain_frames = (v.left == v.target_left && v.right == v.target_right) ? 0 : VOLUME_RAMP;
    if (v.gain_frames) {
        v.step_left = (v.target_left - v.left) / v.gain_frames;
        v.step_right = (v.target_right - v.right) / v.gain_frames;
    }
    v.left_segment.frames = v.right_segment.frames = 0;
    v.gain_dirty = true;
    v.filter.targets(v.filter_cutoff_low, v.filter_cutoff_high, initial);
}

} // namespace

int LayerPatch::weight(const NoteOnEvent &note, int kernel) const {
    const SoundBank &bank = this->bank();
    const int key_weight = bank.key_weight(m_view, kernel);
    if (key_weight < 0) return -1;
    const int threshold = m_view.velocity_gate();
    const int boundary = (std::abs(threshold & ~15) * 9 / 8) & 0x7c;
    if (threshold < 0 ? note.velocity >= boundary : note.velocity < boundary) return -1;
    return key_weight;
}

bool LayerPatch::reuses(const Voice &voice) const noexcept {
    return voice.patch->layered() && voice.patch->index() == index();
}

void LayerPatch::initialize(Voice &v, const NoteStart &start, NoiseState &noise) const {
    const SoundBank &bank = this->bank();
    v.glide_target = m_view.pitch_word(start.kernel);
    v.pitch = static_cast<int16_t>(std::clamp(int(v.glide_target), 0, 0x7f00));
    if (start.program.glide() && (start.mode == AllocationMode::Legato || start.prior_found)) {
        int16_t destination = v.glide_target;
        v.glide_target = start.prior_found
                             ? s16(start.prior_pitch)
                             : s16(uint8_t(start.event.previous_key + m_view.coarse_pitch()) * 256);
        set_glide(v, destination, start.program.glide(), bank);
    }
    initialize_envelope(v.envelope[Voice::pitch_envelope], m_view.pitch(), v, bank);
    initialize_envelope(v.envelope[Voice::filter_envelope], m_view.filter(), v, bank);
    initialize_envelope(v.envelope[Voice::amplitude_envelope], m_view.amplitude(), v, bank);
    v.lfo_target = int16_t(m_view.lfo_depth() << 8);
    v.lfo_phase = m_view.lfo_phase_reset() ? 0 : noise.value;
    v.lfo_step = int16_t((uint32_t(v.lfo_target) * bank.rom.read_u16(sound_5a7e9117::RATE_TABLE +
                                                                     m_view.lfo_fade_offset())) >>
                         16);
    v.filter.mode = uint8_t(m_view.filter_mode());
    v.random_period = v.random_countdown = m_view.random_countdown();
    noise.history = uint16_t(noise.history + noise.value);
    noise.value = uint16_t(noise.value + noise.history);
    v.random = v.previous_random = s16(noise.value);
    // The opening LFO step precedes the initial parameters, which read v.lfo.
    update_lfo(v, m_view, bank);
}

void LayerPatch::address(Voice &v, const PriorWindow &prior) const {
    const SoundBank &bank = this->bank();
    v.split = bank.split_for(m_view, index(), v.pitch, v.selector, v.owner.program);
    const SampleKind sample = m_view.sample();
    uint32_t bit = uint32_t(v.selector & 1) << 28;
    unsigned board = ((v.selector >> 1) & bank.board_bank_mask) << 20;
    uint8_t control =
        sample.probe() ? 0x0b : bank.rom.read_byte(sound_5a7e9117::SPLIT_MODE_TABLE + v.split.mode);
    v.reverse = m_view.reverse() && sample.ordinary();
    if (v.reverse) control = 0x43;
    uint32_t start = v.reverse ? v.split.accumulator : v.split.start;
    uint32_t phase = v.reverse ? v.split.end : v.split.accumulator;
    uint32_t end = v.split.end;
    if (sample.phase_aligned()) phase &= 0xfffffe00;
    if (!sample.table()) phase &= 0xffffff00;
    else {
        uint32_t d0 = m_view.start_offset();
        uint32_t d1 = std::min(uint32_t(d0 + m_view.start_amount_byte()), 0xfau);
        d0 *= 14;
        d1 *= 14;
        const uint32_t split_table = sound_5a7e9117::SPECIAL_SPLIT_TABLE;
        uint32_t d4 = read_be32(bank.rom.pointer(split_table + d0 + 2, 4));
        if (d4 == 0x1dcaa07f) {
            d0 += 14;
            d4 = read_be32(bank.rom.pointer(split_table + d0 + 2, 4));
            if (d0 == d1) d1 += 14;
        }
        uint32_t d5 = read_be32(bank.rom.pointer(split_table + d1 + 2, 4));
        if (d5 == 0x1dcaa07f)
            d5 = read_be32(
                bank.rom.pointer(split_table + d1 + sound_5a7e9117::SPLIT_RECORD_STRIDE, 4));
        v.reverse = m_view.reverse();
        control = v.reverse ? 0x4b : 0x0b;
        start = d4;
        end = d5;
        phase = v.reverse ? d5 : d4;
        if (v.reverse) start += sound_5a7e9117::REVERSE_START_OFFSET;
    }
    v.loop = (control & 8) != 0;
    v.bidirectional = (control & 16) != 0;
    apply_window(v, bit, board, start, phase, end);
    if (sample.ordinary()) {
        int amount = m_view.start_amount() * v.owner.velocity + (int(m_view.start_offset()) << 7);
        if (amount > 0) {
            amount = std::min(32767, amount * 2);
            int length = s16((v.split.end - v.split.accumulator) >> 10);
            int32_t offset = wrap_product(int64_t(length) * amount) >> 5;
            v.position += double(v.reverse ? -offset : offset) / 512;
            v.position = std::clamp(v.position, v.start, v.end);
        }
    }
    if (sample.probe()) {
        // c17e8c loads ACC from this bank's probe. c17fbe leaves START/END
        // unchanged, including when the physical slot was previously free.
        v.boundary = true;
        v.loop = true;
        v.start = double(board) + (prior.valid ? prior.start : double(bank.reset_start) / 512);
        v.end = double(board) + (prior.valid ? prior.end : double(bank.reset_end) / 512);
        v.position = double(board) + double(bank.banks[v.selector].probe & 0x1fffffff) / 512;
    }
}

void LayerPatch::retarget(Voice &v, int kernel, const Program &program) const {
    const SoundBank &bank = this->bank();
    set_glide(v, m_view.pitch_word(kernel), program.glide(), bank);
}

EnvelopeTick LayerPatch::advance_envelopes(Voice &v) const {
    EnvelopeTick result;
    for (unsigned which :
         {Voice::pitch_envelope, Voice::filter_envelope, Voice::amplitude_envelope}) {
        const EnvelopeTick tick = advance(v, which);
        result.ended |= tick.ended;
        result.restarted |= tick.restarted;
    }
    return result;
}

EnvelopeTick LayerPatch::advance(Voice &v, unsigned which) const {
    const SoundBank &bank = this->bank();
    Envelope &e = v.envelope[which];
    if (e.done) return {};
    if (!e.advance()) return {};
    settle_level(e);
    if (e.stage == 4) {
        e.done = true;
        return {which == Voice::amplitude_envelope, false};
    }
    if (e.intermediate) {
        e.intermediate = false;
        segment(e, e.levels[1], e.rates[0], bank);
        return {};
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
        if (which == Voice::amplitude_envelope && m_view.sample().ordinary()) {
            v.position = v.reverse ? v.end : accumulator_start(v);
            if (v.sample_finished) {
                v.sample_finished = false;
                return {false, true};
            }
        }
    } else {
        e.done = true;
        return {which == Voice::amplitude_envelope && e.level() == 0, false};
    }
    return {};
}

EnvelopeTick LayerPatch::step(Voice &v, NoiseState &noise) const {
    const SoundBank &bank = this->bank();
    const EnvelopeTick result = advance_envelopes(v);
    update_lfo(v, m_view, bank);
    update_random(v, noise);
    return result;
}

OnsetStep LayerPatch::onset(Voice &v, NoiseState &noise) const {
    const SoundBank &bank = this->bank();
    const EnvelopeTick envelopes = advance_envelopes(v);
    if (envelopes.ended) return {true, envelopes.restarted, false};
    update_lfo(v, m_view, bank);
    update_random(v, noise);
    advance_glide(v);
    v.apply_parameters(false); // c181da calls c18372 before key-on.
    return {false, envelopes.restarted, true};
}

void LayerPatch::apply_parameters(Voice &v, bool initial) const {
    const SoundBank &bank = this->bank();
    const TrackParameters &c = v.track->config.parameters;
    v.owner.program =
        v.track->config.program; // program in force, as the note's channel snapshot held it
    int common =
        bank.attenuation[c.volume_index()] + bank.attenuation[v.track->sequence_volume & 127];
    common += c.expression_offset();
    const ModSource amp = m_view.amplitude_source();
    int base;
    if (amp == ModSource::Off) base = 254;
    else {
        const int amp_source = modulation(v, amp);
        const int x = amp == ModSource::Lfo ? (amp_source >> 1) + 0x4000 : std::max(0, amp_source);
        const int factor = m_view.amplitude_source_depth();
        int value = (x * factor) >> 15;
        if (factor >= 0) value -= factor;
        base = (value + 127) * 2;
    }
    base += bank.attenuation[m_view.level()] + (m_view.level_plus_43() ? 43 : 0) + common +
            bank.attenuation[v.weight & 127];
    int pan_value = c.signed_pan();
    if (pan_value <= -127) pan_value = m_view.default_pan();
    const int pan_position = (pan_value >> 1) + 64;
    const int env = v.envelope[Voice::amplitude_envelope].level() >> 5;
    v.left_volume =
        volume_target((base + bank.pan[unsigned(127 - pan_position)]) * 4 + env, 0x1b64);
    v.right_volume = volume_target((base + bank.pan[unsigned(pan_position)]) * 4 + env, 0x1b64);
    const int filter_env = v.envelope[Voice::filter_envelope].level();
    const int key = int(v.effective_key) - 60;
    int first = (filter_env * m_view.k1_envelope() +
                 modulation(v, m_view.filter_source()) * m_view.filter_source_depth()) >>
                12;
    first += (key * m_view.k1_key()) >> 3;
    first += m_view.k1_base() * 8;
    v.filter_cutoff_low = bank.coefficient(first);
    int second = (filter_env * m_view.k2_envelope() +
                  (m_view.k2_apply_source() ? modulation(v, m_view.filter_source()) : 0) *
                      m_view.filter_source_depth()) >>
                 12;
    second += (key * m_view.k2_key()) >> 3;
    second += m_view.k2_base() * 8;
    v.filter_cutoff_high = bank.coefficient(second, m_view.k2_table());
    const int pitch_env =
        s16(v.envelope[Voice::pitch_envelope].level() - (int(m_view.pitch().pitch_zero()) << 8));
    int64_t pitch_mod = int64_t(pitch_env) * m_view.pitch_envelope_depth() +
                        int64_t(v.lfo) * m_view.lfo_pitch_depth();
    const int depth = m_view.pitch_source_depth();
    int multiplier =
        bank.rom.read_u16(sound_5a7e9117::PITCH_MOD_TABLE + unsigned(std::abs(depth)) * 2);
    if (depth < 0) multiplier = -s16(multiplier);
    pitch_mod += (int64_t(modulation(v, m_view.pitch_source())) * multiplier) >> 4;
    const int bend = s16(c.bend() - TrackParameters::bend_center);
    pitch_mod >>= 4;
    pitch_mod += int64_t(bend) * bank.bend_range;
    pitch_mod >>= 6;
    v.pitch = s16(v.glide_target + v.split.tuning + int(pitch_mod));
    if (v.split.mode & 128) {
        int amount = (int(m_view.start_amount()) * modulation(v, m_view.start_source())) >> 7;
        amount = std::clamp(amount + (int(m_view.start_offset()) << 8), 0, 32767);
        const int delta = s16((int64_t(amount) * s16(v.split.accumulator >> 16)) >> 16);
        const int span = int16_t(uint16_t((v.split.end - v.split.start) >> 4));
        const uint32_t offset = uint32_t(uint32_t(uint16_t(span)) * bank.frequency(delta)) >> 6;
        const uint32_t start = (v.split.end - offset) | uint32_t((v.selector & 1) << 28);
        v.start = double((v.selector >> 1) << 20) + double(start & 0x1fffffe0) / 512;
        v.pitch = s16(v.pitch + delta);
    }
    v.frequency = bank.frequency(v.pitch);
    finish_parameters(v, initial, pair_choice(c.pair_select(), m_view.pair_choice()));
}

bool LayerPatch::release(Voice &v) const {
    const SoundBank &bank = this->bank();
    for (Envelope &e : v.envelope) {
        if (e.flags & 64) continue;
        e.done = false;
        e.stage = 4;
        segment(e, 0, e.rates[3], bank);
    }
    if (!(v.envelope[Voice::amplitude_envelope].flags & 64) &&
        v.envelope[Voice::amplitude_envelope].level() == 0)
        return true;
    if (v.boundary) {
        const uint32_t value = v.split.end;
        const unsigned board = (v.selector >> 1) << 20;
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
    return false;
}

int DirectPatch::weight(const NoteOnEvent &, int) const { return 127; }

void DirectPatch::initialize(Voice &v, const NoteStart &start, NoiseState &) const {
    const int key = m_view.sounding_key(start.kernel, start.transpose);
    v.pitch = s16(std::clamp(key * 256, 0, 0x7f00) | (m_view.fine_pitch() << 4));
}

void DirectPatch::address(Voice &v, const PriorWindow &) const {
    const SoundBank &bank = this->bank();
    v.split = bank.split_for(m_view, index(), v.pitch, v.selector, v.owner.program);
    const SampleKind sample = m_view.sample();
    if (sample.id == 0x78)
        throw std::runtime_error("ROM direct sample78 enters infinite branch c1968e");
    const uint32_t bit = uint32_t(v.selector & 1) << 28;
    const unsigned board = ((v.selector >> 1) & bank.board_bank_mask) << 20;
    const uint8_t control = bank.rom.read_byte(sound_5a7e9117::SPLIT_MODE_TABLE + v.split.mode);
    v.reverse = m_view.reverse() && sample.ordinary();
    const uint8_t final_control = v.reverse ? 0x43 : control;
    const uint32_t start = v.reverse ? v.split.accumulator : v.split.start;
    uint32_t phase = v.reverse ? v.split.end : v.split.accumulator;
    const uint32_t end = v.split.end;
    if (sample.phase_aligned()) phase &= 0xfffffe00;
    v.loop = (final_control & 8) != 0;
    v.bidirectional = (final_control & 16) != 0;
    apply_window(v, bit, board, start, phase, end);
}

EnvelopeTick DirectPatch::step(Voice &v, NoiseState &) const {
    const SoundBank &bank = this->bank();
    Envelope &e = v.envelope[Voice::amplitude_envelope];
    if (e.done) return {};
    if (!e.advance()) return {};
    settle_level(e);
    // Timer expiry dispatches on the envelope stage (c1971c -> stage routine), not on note-off: stage 0 starts
    // the release ramp (c1983c: ends the voice if the level's top byte is already 0), stage 4 ends the voice
    // (c19888). A patch whose note-off is ignored (patch[0] bit 7) still decays when its timer expires.
    if (e.stage == 4 || (e.position >> 24) == 0) {
        e.done = true;
        return {true, false};
    }
    start_direct_release(e, m_view.release_rate(), bank);
    return {};
}

OnsetStep DirectPatch::onset(Voice &, NoiseState &) const {
    // A direct voice writes its parameters once and waits for the ring.
    return {};
}

void DirectPatch::apply_parameters(Voice &v, bool initial) const {
    const SoundBank &bank = this->bank();
    const TrackParameters &c = v.track->config.parameters;
    v.owner.program = v.track->config.program;
    const int common = bank.attenuation[c.volume_index()] +
                       bank.attenuation[v.track->sequence_volume & 127] + c.expression_offset();
    const int curve = int(velocity_curve_index(m_view.velocity_curve(), v.owner.velocity));
    const int sensitivity = velocity_level_scale(m_view.velocity_level());
    const int scale =
        s16(0x4000 - (128 - bank.velocity_curves[unsigned(curve)]) * sensitivity) >> 6;
    if (initial) {
        Envelope &e = v.envelope[Voice::amplitude_envelope];
        e.position = (uint32_t(uint16_t(m_view.level() * scale)) << 16) | 0x7fff;
        e.slope = -s16(bank.rom.read_u16(sound_5a7e9117::RATE_TABLE + 2 * m_view.decay_rate()));
        e.target = (uint32_t(uint16_t((e.position >> 16) ^ 0x8000))) << 16;
    }
    const int pan_position = c.pan() == 128 ? m_view.centered_pan() : (c.signed_pan() >> 1) + 64;
    const int level =
        (v.envelope[Voice::amplitude_envelope].level() >> 5) + (m_view.level_plus_168() ? 0xa8 : 0);
    v.left_volume =
        volume_target((common + bank.pan[unsigned(127 - pan_position)]) * 4 + level, 0xfa0);
    v.right_volume = volume_target((common + bank.pan[unsigned(pan_position)]) * 4 + level, 0xfa0);
    const int filter_index = direct_filter_index(m_view.filter_index(), v.owner.velocity);
    v.filter_cutoff_low = v.filter_cutoff_high = bank.coefficient(filter_index);
    v.frequency = bank.frequency(v.pitch + v.split.tuning);
    finish_parameters(v, initial, pair_choice(c.pair_select(), m_view.pair_choice()));
}

bool DirectPatch::release(Voice &v) const {
    const SoundBank &bank = this->bank();
    if (m_view.ignore_note_off()) return false;
    start_direct_release(v.envelope[Voice::amplitude_envelope], m_view.release_rate(), bank);
    return false;
}

} // namespace f3rt::hle
