// license:BSD-3-Clause
// copyright-holders:Aaron Giles
/**********************************************************************************************
 *
 *   es5505.hpp - Ensoniq ES5505 OTIS sound generator
 *   Standalone implementation for f3rt
 *
 **********************************************************************************************/

#pragma once

#include <cstdint>
#include <cstddef>
#include <functional>
#include <vector>
#include <span>

namespace f3rt {
class StateWriter;
class StateReader;

class ES5505 {
public:
    static constexpr int MAX_VOICES = 32;
    static constexpr int NUM_CHANNELS = 8; // 4 stereo pairs (0/1, 2/3, 4/5, 6/7)

    struct Voice {
        uint32_t control = 0x0001; // stopped by default (CONTROL_STOP0)
        uint64_t freqcount = 0;
        uint64_t start = 0;
        uint32_t lvol = 0x80;
        uint64_t end = 0;
        uint32_t lvramp = 0;
        uint64_t accum = 0;
        uint32_t rvol = 0x80;
        uint32_t rvramp = 0;
        uint32_t ecount = 0;
        uint32_t k2 = 0;
        uint32_t k2ramp = 0;
        uint32_t k1 = 0;
        uint32_t k1ramp = 0;
        int32_t  o4n1 = 0;
        int32_t  o3n1 = 0;
        int32_t  o3n2 = 0;
        int32_t  o2n1 = 0;
        int32_t  o2n2 = 0;
        int32_t  o1n1 = 0;
        uint8_t  index = 0;
        uint8_t  filtcount = 0;
    };

    ES5505(uint32_t master_clock = 15238090);
    ~ES5505();

    void reset();

    // 16-bit register access from 68000
    uint16_t read(uint32_t offset);
    void write(uint32_t offset, uint16_t data, uint16_t mem_mask = 0xffff);

    // Sample ROM attachment (big-endian 16-bit words)
    void set_sample_rom(const uint16_t *rom_words, size_t word_count);

    // Bank offset per voice in 16-bit words (set by 0x300000 bank register in Taito EN)
    void set_voice_bank(int voice, uint32_t bank_words);

    // IRQ callback (invoked when voice IRQ is raised or cleared)
    void set_irq_callback(std::function<void(bool state)> cb) { m_irq_cb = cb; }

    // Sample rate callback (invoked when active voices change)
    void set_sample_rate_callback(std::function<void(uint32_t rate)> cb) { m_sample_rate_cb = cb; }

    // Generate one audio frame (accumulates 1 sample per active voice into output8[0..7])
    void generate_one_sample(int32_t output8[NUM_CHANNELS]);

    uint32_t sample_rate() const { return m_sample_rate; }
    int active_voices() const { return m_active_voices; }
    uint8_t irqv() const { return m_irqv; }

    int get_voice_index() const { return m_voice_index; }
    size_t state_size() const;
    void save_state(StateWriter &writer) const;
    void load_state(StateReader &reader);

private:
    void compute_tables();
    uint16_t read_sample(Voice *voice, uint32_t word_addr);
    int32_t interpolate(int32_t s1, int32_t s2, uint64_t accum);
    void apply_filters(Voice *voice, int32_t &sample);
    void check_for_end_forward(Voice *voice, uint64_t &accum);
    void check_for_end_reverse(Voice *voice, uint64_t &accum);
    void generate_pcm(Voice *voice, int32_t *dest);
    void generate_irq(Voice *voice, int v);
    void update_internal_irq_state();

    uint16_t reg_read_low(Voice *voice, uint32_t offset);
    uint16_t reg_read_high(Voice *voice, uint32_t offset);
    uint16_t reg_read_test(Voice *voice, uint32_t offset);

    void reg_write_low(Voice *voice, uint32_t offset, uint16_t data, uint16_t mem_mask);
    void reg_write_high(Voice *voice, uint32_t offset, uint16_t data, uint16_t mem_mask);
    void reg_write_test(Voice *voice, uint32_t offset, uint16_t data, uint16_t mem_mask);

    uint32_t m_master_clock;
    uint32_t m_sample_rate;
    uint8_t  m_active_voices;
    uint8_t  m_current_page;
    uint8_t  m_irqv;
    uint16_t m_mode;
    int      m_voice_index;

    Voice m_voices[MAX_VOICES];
    uint32_t m_voice_bank[MAX_VOICES];

    const uint16_t *m_rom;
    size_t m_rom_words;

    std::vector<int16_t> m_ulaw_lookup;
    std::vector<uint32_t> m_volume_lookup;

    std::function<void(bool state)> m_irq_cb;
    std::function<void(uint32_t rate)> m_sample_rate_cb;
};

} // namespace f3rt
