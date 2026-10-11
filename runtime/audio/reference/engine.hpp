#pragma once

#include "third_party/audio/es5505.hpp"
#include "third_party/audio/es5510.hpp"
#include "state_io.hpp"

#include <array>
#include <cstdint>
#include <functional>

namespace f3rt::reference {

// Low-level cycle-accurate reference audio synthesis engine (ES5505 OTIS + ES5510 ESP DSP).
class Engine {
public:
    Engine();

    void reset();
    void set_sample_rom(const uint16_t *rom_words, size_t word_count);
    void set_bank_mask(uint32_t mask) { m_bank_mask = mask; }
    uint32_t bank_mask() const { return m_bank_mask; }
    void set_bank_table(int voice, uint16_t value) {
        if (voice >= 0 && voice < ES5505::MAX_VOICES) m_bank_table[voice] = value;
    }
    uint16_t bank_table(int voice) const {
        return (voice >= 0 && voice < ES5505::MAX_VOICES) ? m_bank_table[voice] : 0;
    }

    void set_esp_halted(bool halted) { m_esp_halted = halted; }
    bool esp_halted() const { return m_esp_halted; }
    uint32_t sample_rate() const { return m_es5505.sample_rate(); }

    bool is_mapped(uint32_t address) const;
    uint8_t read8(uint32_t address);
    uint16_t read16(uint32_t address);
    uint32_t read32(uint32_t address);
    void write8(uint32_t address, uint8_t value);
    void write16(uint32_t address, uint16_t value);
    void write32(uint32_t address, uint32_t value);

    // Generate one PCM frame: ES5505 voice render -> serial DSP -> board gains -> push_frame
    void generate_one_frame(const std::array<float, 2> &otis_gain,
                            const std::array<float, 2> &output_gain,
                            const std::function<void(float, float)> &push_frame);

    // State snapshot serialization
    void save_state(StateWriter &writer) const;
    void load_state(StateReader &reader);

    ES5505 &es5505() { return m_es5505; }
    const ES5505 &es5505() const { return m_es5505; }
    ES5510 &es5510() { return m_es5510; }
    const ES5510 &es5510() const { return m_es5510; }

private:
    ES5505 m_es5505;
    ES5510 m_es5510;
    uint16_t m_bank_table[ES5505::MAX_VOICES];
    uint32_t m_bank_mask = 7;
    bool m_esp_halted = true;
};

} // namespace f3rt::reference
