#include "audio/reference/engine.hpp"

#include <algorithm>
#include <cstring>

namespace f3rt::reference {

Engine::Engine() {
    std::memset(m_bank_table, 0, sizeof(m_bank_table));
}

void Engine::reset() {
    m_es5510.reset();
    std::memset(m_bank_table, 0, sizeof(m_bank_table));
}

void Engine::set_sample_rom(const uint16_t *rom_words, size_t word_count) {
    m_es5505.set_sample_rom(rom_words, word_count);
}

bool Engine::is_mapped(uint32_t address) const {
    address &= 0x00ffffff;
    return (address >= 0x200000 && address < 0x200020) ||
           (address >= 0x260000 && address < 0x260200) ||
           (address >= 0x300000 && address < 0x300040);
}

uint8_t Engine::read8(uint32_t address) {
    address &= 0x00ffffff;
    if (address >= 0x200000 && address < 0x200020) {
        uint16_t w = m_es5505.read((address >> 1) & 0x0f);
        return (address & 1) ? uint8_t(w & 0xff) : uint8_t(w >> 8);
    }
    if (address >= 0x260000 && address < 0x260200) {
        if (address & 1) {
            return m_es5510.host_r((address >> 1) & 0xff);
        }
        return 0x00;
    }
    if (address >= 0x300000 && address < 0x300040) {
        uint32_t voice = (address >> 1) & 0x1f;
        uint16_t val = (voice < ES5505::MAX_VOICES) ? m_bank_table[voice] : 0;
        return (address & 1) ? uint8_t(val & 0xff) : uint8_t(val >> 8);
    }
    return 0xff;
}

uint16_t Engine::read16(uint32_t address) {
    address &= 0x00ffffff;
    if (address >= 0x200000 && address < 0x200020) {
        return m_es5505.read((address >> 1) & 0x0f);
    }
    if (address >= 0x260000 && address < 0x260200) {
        return uint16_t(m_es5510.host_r((address >> 1) & 0xff));
    }
    if (address >= 0x300000 && address < 0x300040) {
        uint32_t voice = (address >> 1) & 0x1f;
        return (voice < ES5505::MAX_VOICES) ? m_bank_table[voice] : 0;
    }
    return 0xffff;
}

uint32_t Engine::read32(uint32_t address) {
    return (uint32_t(read16(address)) << 16) | read16(address + 2);
}

void Engine::write8(uint32_t address, uint8_t value) {
    address &= 0x00ffffff;
    if (address >= 0x200000 && address < 0x200020) {
        uint16_t mem_mask = (address & 1) ? 0x00ff : 0xff00;
        uint16_t data = (address & 1) ? value : uint16_t(uint16_t(value) << 8);
        m_es5505.write((address >> 1) & 0x0f, data, mem_mask);
        return;
    }
    if (address >= 0x260000 && address < 0x260200) {
        if (address & 1) {
            m_es5510.host_w((address >> 1) & 0xff, value);
        }
        return;
    }
    if (address >= 0x300000 && address < 0x300040) {
        uint32_t voice = (address >> 1) & 0x1f;
        if (voice < ES5505::MAX_VOICES) {
            if (address & 1) {
                m_bank_table[voice] = (m_bank_table[voice] & 0xff00) | value;
            } else {
                m_bank_table[voice] = (m_bank_table[voice] & 0x00ff) | (uint16_t(value) << 8);
            }
            m_es5505.set_voice_bank(voice, (m_bank_table[voice] & m_bank_mask) << 20);
        }
    }
}

void Engine::write16(uint32_t address, uint16_t value) {
    address &= 0x00ffffff;
    if (address >= 0x200000 && address < 0x200020) {
        m_es5505.write((address >> 1) & 0x0f, value, 0xffff);
        return;
    }
    if (address >= 0x260000 && address < 0x260200) {
        m_es5510.host_w((address >> 1) & 0xff, uint8_t(value & 0xff));
        return;
    }
    if (address >= 0x300000 && address < 0x300040) {
        uint32_t voice = (address >> 1) & 0x1f;
        if (voice < ES5505::MAX_VOICES) {
            m_bank_table[voice] = value;
            m_es5505.set_voice_bank(voice, (value & m_bank_mask) << 20);
        }
    }
}

void Engine::write32(uint32_t address, uint32_t value) {
    write16(address, uint16_t(value >> 16));
    write16(address + 2, uint16_t(value & 0xffff));
}

void Engine::generate_one_frame(const std::array<float, 2> &otis_gain,
                                const std::array<float, 2> &output_gain,
                                const std::function<void(float, float)> &push_frame) {
    int32_t cursample[ES5505::NUM_CHANNELS];
    m_es5505.generate_one_sample(cursample);

    float channels[ES5505::NUM_CHANNELS];
    for (unsigned i = 0; i < ES5505::NUM_CHANNELS; ++i)
        channels[i] = std::clamp(float(cursample[i]) / 524288.0f, -1.0f, 1.0f) * otis_gain[i & 1];
    for (unsigned i = 0; i < 6; ++i)
        m_es5510.ser_w(i, int16_t(int32_t(channels[i + 2] * 32768.0f)));

    if (!m_esp_halted) {
        m_es5510.run_once();
    }

    int16_t main_l = m_es5510.ser_r(6);
    int16_t main_r = m_es5510.ser_r(7);

    const float out_l = (float(main_l) / 32768.0f + channels[0]) * 0.5f * output_gain[0];
    const float out_r = (float(main_r) / 32768.0f + channels[1]) * 0.5f * output_gain[1];

    push_frame(out_l, out_r);
}

void Engine::save_state(StateWriter &writer) const {
    m_es5505.save_state(writer);
    m_es5510.save_state(writer);
}

void Engine::load_state(StateReader &reader) {
    m_es5505.load_state(reader);
    m_es5510.load_state(reader);
}

} // namespace f3rt::reference
