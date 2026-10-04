// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/**********************************************************************************************
 *
 *   es5505.cpp - Ensoniq ES5505 OTIS sound generator
 *   Standalone implementation for f3rt
 *
 **********************************************************************************************/

#include "es5505.hpp"

#include <algorithm>
#include <cstring>

namespace f3rt {

namespace {

constexpr uint32_t FINE_FILTER_BIT = 16;
constexpr uint32_t FILTER_BIT      = 12;
constexpr uint32_t FILTER_SHIFT    = FINE_FILTER_BIT - FILTER_BIT;

constexpr uint32_t ULAW_MAXBITS    = 8;

enum : uint16_t {
    CONTROL_BS1         = 0x8000,
    CONTROL_BS0         = 0x4000,
    CONTROL_CMPD        = 0x2000,
    CONTROL_CA1         = 0x0200,
    CONTROL_CA0         = 0x0100,
    CONTROL_LP4         = 0x0800,
    CONTROL_LP3         = 0x0400,
    CONTROL_IRQ         = 0x0080,
    CONTROL_DIR         = 0x0040,
    CONTROL_IRQE        = 0x0020,
    CONTROL_BLE         = 0x0010,
    CONTROL_LPE         = 0x0008,
    CONTROL_LEI         = 0x0004,
    CONTROL_STOP1       = 0x0002,
    CONTROL_STOP0       = 0x0001,

    CONTROL_LOOPMASK    = (CONTROL_BLE | CONTROL_LPE),
    CONTROL_STOPMASK    = (CONTROL_STOP1 | CONTROL_STOP0)
};

constexpr int ADDRESS_FRAC_BIT = 11;
constexpr uint64_t ADDRESS_ACC_MASK = 0x7fffffffULL; // 20 integer + 9 fraction + 2 internal precision bits

inline uint64_t get_address_acc_shifted_val(uint64_t val, int bias = 0) {
    int shift = 2 - bias;
    return (shift >= 0) ? (val << shift) : (val >> (-shift));
}

inline uint64_t get_address_acc_res(uint64_t val, int bias = 0) {
    int shift = 2 - bias;
    return (shift >= 0) ? (val >> shift) : (val << (-shift));
}

inline uint64_t get_integer_addr(uint64_t accum, int32_t bias = 0) {
    return ((accum + (uint64_t(bias) << ADDRESS_FRAC_BIT)) & ADDRESS_ACC_MASK) >> ADDRESS_FRAC_BIT;
}

inline int32_t apply_lowpass(int32_t out, int32_t cutoff, int32_t in) {
    return ((int32_t)(cutoff >> FILTER_SHIFT) * (out - in) / (1 << FILTER_BIT)) + in;
}

inline int32_t apply_highpass(int32_t out, int32_t cutoff, int32_t in, int32_t prev) {
    return out - prev + ((int32_t)(cutoff >> FILTER_SHIFT) * in) / (1 << (FILTER_BIT + 1)) + in / 2;
}

inline void update_pole(int32_t &pole, int32_t sample) {
    pole = sample;
}

inline void update_2_pole(int32_t &prev, int32_t &pole, int32_t sample) {
    prev = pole;
    pole = sample;
}

} // namespace

ES5505::ES5505(uint32_t master_clock)
    : m_master_clock(master_clock)
    , m_sample_rate(0)
    , m_active_voices(31)
    , m_current_page(0)
    , m_irqv(0x80)
    , m_mode(0x7f8)
    , m_voice_index(0)
    , m_rom(nullptr)
    , m_rom_words(0)
{
    std::memset(m_voice_bank, 0, sizeof(m_voice_bank));
    compute_tables();
    reset();
}

ES5505::~ES5505() = default;

void ES5505::reset() {
    m_active_voices = 31;
    m_current_page = 0;
    m_irqv = 0x80;
    m_mode = 0x7f8;
    m_voice_index = 0;

    m_sample_rate = m_master_clock / (16 * (m_active_voices + 1));
    if (m_sample_rate_cb) {
        m_sample_rate_cb(m_sample_rate);
    }

    for (int j = 0; j < MAX_VOICES; j++) {
        m_voices[j].index = j;
        m_voices[j].control = CONTROL_STOPMASK;
        m_voices[j].freqcount = 0;
        m_voices[j].start = 0;
        m_voices[j].end = 0;
        m_voices[j].accum = 0;
        m_voices[j].lvol = 0x80;
        m_voices[j].rvol = 0x80;
        m_voices[j].k1 = 0;
        m_voices[j].k2 = 0;
        m_voices[j].o1n1 = 0;
        m_voices[j].o2n1 = 0;
        m_voices[j].o2n2 = 0;
        m_voices[j].o3n1 = 0;
        m_voices[j].o3n2 = 0;
        m_voices[j].o4n1 = 0;
        m_voices[j].filtcount = 0;
        m_voice_bank[j] = 0;
    }
}

void ES5505::set_sample_rom(const uint16_t *rom_words, size_t word_count) {
    m_rom = rom_words;
    m_rom_words = word_count;
}

void ES5505::set_voice_bank(int voice, uint32_t bank_words) {
    if (voice >= 0 && voice < MAX_VOICES) {
        m_voice_bank[voice] = bank_words;
    }
}

void ES5505::compute_tables() {
    m_ulaw_lookup.resize(1 << ULAW_MAXBITS);
    for (int i = 0; i < (1 << ULAW_MAXBITS); i++) {
        const uint16_t rawval = (i << (16 - ULAW_MAXBITS)) | (1 << (15 - ULAW_MAXBITS));
        const uint8_t exponent = rawval >> 13;
        uint32_t mantissa = (rawval << 3) & 0xffff;
        if (exponent == 0) {
            m_ulaw_lookup[i] = int16_t(mantissa) >> 7;
        } else {
            mantissa = (mantissa >> 1) | (~mantissa & 0x8000);
            m_ulaw_lookup[i] = int16_t(mantissa) >> (7 - exponent);
        }
    }

    constexpr uint32_t volume_len = 256;
    m_volume_lookup.resize(volume_len);
    for (int i = 0; i < 256; i++) {
        const uint32_t exponent = (i >> 4) & 15;
        const uint32_t mantissa = (i & 15) | 16;
        m_volume_lookup[i] = (mantissa << 11) >> (16 - exponent);
    }
}

uint16_t ES5505::read_sample(Voice *voice, uint32_t word_addr) {
    m_voice_index = voice->index;
    if (!m_rom || m_rom_words == 0) return 0;
    uint32_t full_addr = (m_voice_bank[voice->index] + word_addr) & (m_rom_words - 1);
    return m_rom[full_addr];
}

int32_t ES5505::interpolate(int32_t s1, int32_t s2, uint64_t accum) {
    constexpr uint32_t shifted = 1 << ADDRESS_FRAC_BIT;
    constexpr uint32_t mask = shifted - 1;
    accum &= mask & ADDRESS_ACC_MASK;
    return (s1 * int32_t(shifted - accum) + s2 * int32_t(accum)) >> ADDRESS_FRAC_BIT;
}

void ES5505::apply_filters(Voice *voice, int32_t &sample) {
    // Pole 1: lowpass using K1
    sample = apply_lowpass(sample, voice->k1, voice->o1n1);
    update_pole(voice->o1n1, sample);

    // Pole 2: lowpass using K1
    sample = apply_lowpass(sample, voice->k1, voice->o2n1);
    update_2_pole(voice->o2n2, voice->o2n1, sample);

    uint32_t lp = (voice->control >> 10) & 3;
    switch (lp) {
    case 0:
        sample = apply_highpass(sample, voice->k2, voice->o3n1, voice->o2n2);
        update_2_pole(voice->o3n2, voice->o3n1, sample);
        sample = apply_highpass(sample, voice->k2, voice->o4n1, voice->o3n2);
        update_pole(voice->o4n1, sample);
        break;
    case 1:
        sample = apply_lowpass(sample, voice->k1, voice->o3n1);
        update_2_pole(voice->o3n2, voice->o3n1, sample);
        sample = apply_highpass(sample, voice->k2, voice->o4n1, voice->o3n2);
        update_pole(voice->o4n1, sample);
        break;
    case 2:
        sample = apply_lowpass(sample, voice->k2, voice->o3n1);
        update_2_pole(voice->o3n2, voice->o3n1, sample);
        sample = apply_lowpass(sample, voice->k2, voice->o4n1);
        update_pole(voice->o4n1, sample);
        break;
    case 3:
        sample = apply_lowpass(sample, voice->k1, voice->o3n1);
        update_2_pole(voice->o3n2, voice->o3n1, sample);
        sample = apply_lowpass(sample, voice->k2, voice->o4n1);
        update_pole(voice->o4n1, sample);
        break;
    }
}

void ES5505::check_for_end_forward(Voice *voice, uint64_t &accum) {
    if (accum > voice->end) {
        if (voice->control & CONTROL_IRQE) {
            voice->control |= CONTROL_IRQ;
        }
        switch (voice->control & CONTROL_LOOPMASK) {
        case 0:
        case CONTROL_BLE:
            voice->control |= CONTROL_STOP0;
            break;
        case CONTROL_LPE:
            accum = (voice->start + (accum - voice->end)) & ADDRESS_ACC_MASK;
            break;
        case CONTROL_LPE | CONTROL_BLE:
            accum = (voice->end - (accum - voice->end)) & ADDRESS_ACC_MASK;
            voice->control ^= CONTROL_DIR;
            break;
        }
    }
}

void ES5505::check_for_end_reverse(Voice *voice, uint64_t &accum) {
    if (accum < voice->start) {
        if (voice->control & CONTROL_IRQE) {
            voice->control |= CONTROL_IRQ;
        }
        switch (voice->control & CONTROL_LOOPMASK) {
        case 0:
        case CONTROL_BLE:
            voice->control |= CONTROL_STOP0;
            break;
        case CONTROL_LPE:
            accum = (voice->end - (voice->start - accum)) & ADDRESS_ACC_MASK;
            break;
        case CONTROL_LPE | CONTROL_BLE:
            accum = (voice->start + (voice->start - accum)) & ADDRESS_ACC_MASK;
            voice->control ^= CONTROL_DIR;
            break;
        }
    }
}

void ES5505::generate_pcm(Voice *voice, int32_t *dest) {
    const uint32_t freqcount = uint32_t(voice->freqcount);
    uint64_t accum = voice->accum & ADDRESS_ACC_MASK;

    if (!(voice->control & CONTROL_STOPMASK)) {
        if (!(voice->control & CONTROL_DIR)) {
            // Forward
            int32_t val1 = int16_t(read_sample(voice, get_integer_addr(accum)));
            int32_t val2 = int16_t(read_sample(voice, get_integer_addr(accum, 1)));

            val1 = interpolate(val1, val2, accum);
            accum = (accum + freqcount) & ADDRESS_ACC_MASK;

            apply_filters(voice, val1);

            dest[0] += (int64_t(val1) * m_volume_lookup[voice->lvol & 0xff]) >> 11;
            dest[1] += (int64_t(val1) * m_volume_lookup[voice->rvol & 0xff]) >> 11;

            check_for_end_forward(voice, accum);
        } else {
            // Backward
            int32_t val1 = int16_t(read_sample(voice, get_integer_addr(accum)));
            int32_t val2 = int16_t(read_sample(voice, get_integer_addr(accum, 1)));

            val1 = interpolate(val1, val2, accum);
            accum = (accum - freqcount) & ADDRESS_ACC_MASK;

            apply_filters(voice, val1);

            dest[0] += (int64_t(val1) * m_volume_lookup[voice->lvol & 0xff]) >> 11;
            dest[1] += (int64_t(val1) * m_volume_lookup[voice->rvol & 0xff]) >> 11;

            check_for_end_reverse(voice, accum);
        }
    }

    voice->accum = accum;
}

void ES5505::generate_irq(Voice *voice, int v) {
    if (voice->control & CONTROL_IRQ) {
        if (m_irqv & 0x80) {
            m_irqv = v & 0x1f;
            voice->control &= ~CONTROL_IRQ;
            if (m_irq_cb) {
                m_irq_cb(true);
            }
        }
    }
}

void ES5505::update_internal_irq_state() {
    m_irqv = 0x80;
    if (m_irq_cb) {
        m_irq_cb(false);
    }
}

void ES5505::generate_one_sample(int32_t output8[NUM_CHANNELS]) {
    std::memset(output8, 0, sizeof(int32_t) * NUM_CHANNELS);

    for (int v = 0; v <= m_active_voices; v++) {
        Voice *voice = &m_voices[v];

        const int voice_channel = (voice->control >> 8) & 3;
        const int left_idx = voice_channel * 2;

        generate_pcm(voice, &output8[left_idx]);
        generate_irq(voice, v);
    }
}

uint16_t ES5505::reg_read_low(Voice *voice, uint32_t offset) {
    switch (offset) {
    case 0x00: return voice->control | 0xf000;
    case 0x01: return uint16_t(get_address_acc_res(voice->freqcount, 1));
    case 0x02: return uint16_t(get_address_acc_res(voice->start) >> 16);
    case 0x03: return uint16_t(get_address_acc_res(voice->start));
    case 0x04: return uint16_t(get_address_acc_res(voice->end) >> 16);
    case 0x05: return uint16_t(get_address_acc_res(voice->end));
    case 0x06: return uint16_t(voice->k2);
    case 0x07: return uint16_t(voice->k1);
    case 0x08: return uint16_t(voice->lvol << 8);
    case 0x09: return uint16_t(voice->rvol << 8);
    case 0x0a: return uint16_t(get_address_acc_res(voice->accum) >> 16);
    case 0x0b: return uint16_t(get_address_acc_res(voice->accum));
    case 0x0d: return m_active_voices;
    case 0x0e: {
        uint16_t res = m_irqv;
        update_internal_irq_state();
        return res;
    }
    case 0x0f: return m_current_page;
    default:   return 0;
    }
}

uint16_t ES5505::reg_read_high(Voice *voice, uint32_t offset) {
    switch (offset) {
    case 0x00: return voice->control | 0xf000;
    case 0x01: return uint16_t(voice->o4n1 & 0xffff);
    case 0x02: return uint16_t(voice->o3n1 & 0xffff);
    case 0x03: return uint16_t(voice->o3n2 & 0xffff);
    case 0x04: return uint16_t(voice->o2n1 & 0xffff);
    case 0x05: return uint16_t(voice->o2n2 & 0xffff);
    case 0x06:
        // Special case for Taito F3 games: reading O1(n-1) on stopped voice returns raw sample ROM data
        if (voice->control & CONTROL_STOPMASK) {
            voice->o1n1 = read_sample(voice, get_integer_addr(voice->accum));
        }
        return uint16_t(voice->o1n1 & 0xffff);
    case 0x0d: return m_active_voices;
    case 0x0e: {
        uint16_t res = m_irqv;
        update_internal_irq_state();
        return res;
    }
    case 0x0f: return m_current_page;
    default:   return 0;
    }
}

uint16_t ES5505::reg_read_test(Voice *voice, uint32_t offset) {
    switch (offset) {
    case 0x08: return m_mode | 0x7f8;
    case 0x0d: return m_active_voices;
    case 0x0e: {
        uint16_t res = m_irqv;
        update_internal_irq_state();
        return res;
    }
    case 0x0f: return m_current_page;
    default:   return 0;
    }
}

void ES5505::reg_write_low(Voice *voice, uint32_t offset, uint16_t data, uint16_t mem_mask) {
    switch (offset) {
    case 0x00: // CR
        voice->control |= 0xf000;
        if (mem_mask & 0x00ff) {
            voice->control = (voice->control & ~0x00ff) | (data & 0x00ff);
        }
        if (mem_mask & 0xff00) {
            voice->control = (voice->control & ~0x0f00) | (data & 0x0f00);
        }
        break;
    case 0x01: // FC
        if (mem_mask & 0x00ff) {
            voice->freqcount = (voice->freqcount & ~get_address_acc_shifted_val(0x00fe, 1)) | (get_address_acc_shifted_val(data & 0x00fe, 1));
        }
        if (mem_mask & 0xff00) {
            voice->freqcount = (voice->freqcount & ~get_address_acc_shifted_val(0xff00, 1)) | (get_address_acc_shifted_val(data & 0xff00, 1));
        }
        break;
    case 0x02: // STRT (hi)
        if (mem_mask & 0x00ff) {
            voice->start = (voice->start & ~get_address_acc_shifted_val(0x00ff0000)) | (get_address_acc_shifted_val((data & 0x00ff) << 16));
        }
        if (mem_mask & 0xff00) {
            voice->start = (voice->start & ~get_address_acc_shifted_val(0x1f000000)) | (get_address_acc_shifted_val((data & 0x1f00) << 16));
        }
        break;
    case 0x03: // STRT (lo)
        if (mem_mask & 0x00ff) {
            voice->start = (voice->start & ~get_address_acc_shifted_val(0x000000e0)) | (get_address_acc_shifted_val(data & 0x00e0));
        }
        if (mem_mask & 0xff00) {
            voice->start = (voice->start & ~get_address_acc_shifted_val(0x0000ff00)) | (get_address_acc_shifted_val(data & 0xff00));
        }
        break;
    case 0x04: // END (hi)
        if (mem_mask & 0x00ff) {
            voice->end = (voice->end & ~get_address_acc_shifted_val(0x00ff0000)) | (get_address_acc_shifted_val((data & 0x00ff) << 16));
        }
        if (mem_mask & 0xff00) {
            voice->end = (voice->end & ~get_address_acc_shifted_val(0x1f000000)) | (get_address_acc_shifted_val((data & 0x1f00) << 16));
        }
        break;
    case 0x05: // END (lo)
        if (mem_mask & 0x00ff) {
            voice->end = (voice->end & ~get_address_acc_shifted_val(0x000000e0)) | (get_address_acc_shifted_val(data & 0x00e0));
        }
        if (mem_mask & 0xff00) {
            voice->end = (voice->end & ~get_address_acc_shifted_val(0x0000ff00)) | (get_address_acc_shifted_val(data & 0xff00));
        }
        break;
    case 0x06: // K2
        if (mem_mask & 0x00ff) {
            voice->k2 = (voice->k2 & ~0x00f0) | (data & 0x00f0);
        }
        if (mem_mask & 0xff00) {
            voice->k2 = (voice->k2 & ~0xff00) | (data & 0xff00);
        }
        break;
    case 0x07: // K1
        if (mem_mask & 0x00ff) {
            voice->k1 = (voice->k1 & ~0x00f0) | (data & 0x00f0);
        }
        if (mem_mask & 0xff00) {
            voice->k1 = (voice->k1 & ~0xff00) | (data & 0xff00);
        }
        break;
    case 0x08: // LVOL
        if (mem_mask & 0xff00) {
            voice->lvol = (data >> 8) & 0xff;
        }
        break;
    case 0x09: // RVOL
        if (mem_mask & 0xff00) {
            voice->rvol = (data >> 8) & 0xff;
        }
        break;
    case 0x0a: // ACC (hi)
        if (mem_mask & 0x00ff) {
            voice->accum = (voice->accum & ~get_address_acc_shifted_val(0x00ff0000)) | (get_address_acc_shifted_val((data & 0x00ff) << 16));
        }
        if (mem_mask & 0xff00) {
            voice->accum = (voice->accum & ~get_address_acc_shifted_val(0x1f000000)) | (get_address_acc_shifted_val((data & 0x1f00) << 16));
        }
        break;
    case 0x0b: // ACC (lo)
        if (mem_mask & 0x00ff) {
            voice->accum = (voice->accum & ~get_address_acc_shifted_val(0x000000ff)) | (get_address_acc_shifted_val(data & 0x00ff));
        }
        if (mem_mask & 0xff00) {
            voice->accum = (voice->accum & ~get_address_acc_shifted_val(0x0000ff00)) | (get_address_acc_shifted_val(data & 0xff00));
        }
        break;
    case 0x0d: // ACT
        if (mem_mask & 0x00ff) {
            m_active_voices = data & 0x1f;
            m_sample_rate = m_master_clock / (16 * (m_active_voices + 1));
            if (m_sample_rate_cb) {
                m_sample_rate_cb(m_sample_rate);
            }
        }
        break;
    case 0x0f: // PAGE
        if (mem_mask & 0x00ff) {
            m_current_page = data & 0x7f;
        }
        break;
    default:
        break;
    }
}

void ES5505::reg_write_high(Voice *voice, uint32_t offset, uint16_t data, uint16_t mem_mask) {
    switch (offset) {
    case 0x00: // CR
        voice->control |= 0xf000;
        if (mem_mask & 0x00ff) {
            voice->control = (voice->control & ~0x00ff) | (data & 0x00ff);
        }
        if (mem_mask & 0xff00) {
            voice->control = (voice->control & ~0x0f00) | (data & 0x0f00);
        }
        break;
    case 0x01: // O4(n-1)
        if (mem_mask & 0x00ff) voice->o4n1 = (voice->o4n1 & ~0x00ff) | (data & 0x00ff);
        if (mem_mask & 0xff00) voice->o4n1 = (int16_t)((voice->o4n1 & ~0xff00) | (data & 0xff00));
        break;
    case 0x02: // O3(n-1)
        if (mem_mask & 0x00ff) voice->o3n1 = (voice->o3n1 & ~0x00ff) | (data & 0x00ff);
        if (mem_mask & 0xff00) voice->o3n1 = (int16_t)((voice->o3n1 & ~0xff00) | (data & 0xff00));
        break;
    case 0x03: // O3(n-2)
        if (mem_mask & 0x00ff) voice->o3n2 = (voice->o3n2 & ~0x00ff) | (data & 0x00ff);
        if (mem_mask & 0xff00) voice->o3n2 = (int16_t)((voice->o3n2 & ~0xff00) | (data & 0xff00));
        break;
    case 0x04: // O2(n-1)
        if (mem_mask & 0x00ff) voice->o2n1 = (voice->o2n1 & ~0x00ff) | (data & 0x00ff);
        if (mem_mask & 0xff00) voice->o2n1 = (int16_t)((voice->o2n1 & ~0xff00) | (data & 0xff00));
        break;
    case 0x05: // O2(n-2)
        if (mem_mask & 0x00ff) voice->o2n2 = (voice->o2n2 & ~0x00ff) | (data & 0x00ff);
        if (mem_mask & 0xff00) voice->o2n2 = (int16_t)((voice->o2n2 & ~0xff00) | (data & 0xff00));
        break;
    case 0x06: // O1(n-1)
        if (mem_mask & 0x00ff) voice->o1n1 = (voice->o1n1 & ~0x00ff) | (data & 0x00ff);
        if (mem_mask & 0xff00) voice->o1n1 = (int16_t)((voice->o1n1 & ~0xff00) | (data & 0xff00));
        break;
    case 0x0d: // ACT
        if (mem_mask & 0x00ff) {
            m_active_voices = data & 0x1f;
            m_sample_rate = m_master_clock / (16 * (m_active_voices + 1));
            if (m_sample_rate_cb) {
                m_sample_rate_cb(m_sample_rate);
            }
        }
        break;
    case 0x0f: // PAGE
        if (mem_mask & 0x00ff) {
            m_current_page = data & 0x7f;
        }
        break;
    default:
        break;
    }
}

void ES5505::reg_write_test(Voice *voice, uint32_t offset, uint16_t data, uint16_t mem_mask) {
    switch (offset) {
    case 0x08: // SERMODE
        m_mode |= 0x7f8;
        if (mem_mask & 0xff00) m_mode = (m_mode & ~0xf800) | (data & 0xf800);
        if (mem_mask & 0x00ff) m_mode = (m_mode & ~0x0007) | (data & 0x0007);
        break;
    case 0x0d: // ACT
        if (mem_mask & 0x00ff) {
            m_active_voices = data & 0x1f;
            m_sample_rate = m_master_clock / (16 * (m_active_voices + 1));
            if (m_sample_rate_cb) {
                m_sample_rate_cb(m_sample_rate);
            }
        }
        break;
    case 0x0f: // PAGE
        if (mem_mask & 0x00ff) {
            m_current_page = data & 0x7f;
        }
        break;
    default:
        break;
    }
}

uint16_t ES5505::read(uint32_t offset) {
    Voice *voice = &m_voices[m_current_page & 0x1f];
    offset &= 0x0f;
    if (m_current_page < 0x20) {
        return reg_read_low(voice, offset);
    } else if (m_current_page < 0x40) {
        return reg_read_high(voice, offset);
    } else {
        return reg_read_test(voice, offset);
    }
}

void ES5505::write(uint32_t offset, uint16_t data, uint16_t mem_mask) {
    Voice *voice = &m_voices[m_current_page & 0x1f];
    offset &= 0x0f;
    if (m_current_page < 0x20) {
        reg_write_low(voice, offset, data, mem_mask);
    } else if (m_current_page < 0x40) {
        reg_write_high(voice, offset, data, mem_mask);
    } else {
        reg_write_test(voice, offset, data, mem_mask);
    }
}

} // namespace f3rt
