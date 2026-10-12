#pragma once
#include <array>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <stdexcept>
#include "state_io.hpp"
namespace f3rt {
// 93C46, 64 x 16 organization. DO read dummy bit and sequential reads included.
class Eeprom {
public:
    std::array<uint16_t, 64> words;
    Eeprom() { words.fill(0xffff); }
    // Absolute 16 MHz main-clock ticks. Programming completion is independent
    // of serial clock edges; DO reports busy when CS is raised to poll it.
    bool output(uint64_t now) const {
        return m_selected && m_mode == Mode::Command && m_count == 0 ? now >= m_ready_at
                                                                     : m_data_out;
    }
    void reset() {
        m_selected = m_old_clock = m_writable = false;
        m_ready_at = 0;
        reset_command();
    }
    void pins(uint8_t pins, uint64_t now) {
        const bool select = pins & 0x10, clock = pins & 0x08, bit = pins & 0x04;
        if (!select) {
            m_selected = false;
            m_old_clock = clock;
            reset_command();
            return;
        }
        if (!m_selected) {
            m_selected = true;
            reset_command();
        }
        if (clock && !m_old_clock) edge(bit, now);
        m_old_clock = clock;
    }
    void load(const std::filesystem::path &path) {
        if (!std::filesystem::exists(path)) return;
        std::ifstream f(path, std::ios::binary | std::ios::ate);
        if (!f || f.tellg() != 128)
            throw std::runtime_error("Invalid EEPROM file: " + path.string());
        f.seekg(0);
        std::array<uint8_t, 128> data{};
        if (!f.read(reinterpret_cast<char *>(data.data()), data.size()))
            throw std::runtime_error("EEPROM read failed");
        for (size_t i = 0; i < 64; ++i) words[i] = uint16_t(data[2 * i] << 8 | data[2 * i + 1]);
    }
    void save(const std::filesystem::path &path) const {
        std::array<uint8_t, 128> data{};
        for (size_t i = 0; i < 64; ++i) {
            data[2 * i] = uint8_t(words[i] >> 8);
            data[2 * i + 1] = uint8_t(words[i]);
        }
        std::ofstream f(path, std::ios::binary | std::ios::trunc);
        if (!f.write(reinterpret_cast<const char *>(data.data()), data.size()))
            throw std::runtime_error("EEPROM write failed");
    }
    size_t state_size() const { return sizeof(CanonicalEeprom); }
    void save_state(StateWriter &writer) const {
        CanonicalEeprom st{};
        std::memcpy(st.words, words.data(), sizeof(st.words));
        st.mode = uint8_t(m_mode);
        st.selected = m_selected ? 1 : 0;
        st.old_clock = m_old_clock ? 1 : 0;
        st.data_out = m_data_out ? 1 : 0;
        st.writable = m_writable ? 1 : 0;
        st.shift = m_shift;
        st.count = m_count;
        st.address = m_address;
        st.read_bit = m_read_bit;
        st.ready_at = m_ready_at;
        writer.write(st);
    }
    void load_state(StateReader &reader) {
        CanonicalEeprom st;
        reader.read(st);
        std::memcpy(words.data(), st.words, sizeof(st.words));
        m_mode = Mode(st.mode);
        m_selected = st.selected != 0;
        m_old_clock = st.old_clock != 0;
        m_data_out = st.data_out != 0;
        m_writable = st.writable != 0;
        m_shift = st.shift;
        m_count = st.count;
        m_address = st.address;
        m_read_bit = st.read_bit;
        m_ready_at = st.ready_at;
    }

private:
    enum class Mode { Command, Read, Write, WriteAll, Done } m_mode = Mode::Command;
    bool m_selected = false, m_old_clock = false, m_data_out = true, m_writable = false;
    uint32_t m_shift = 0;
    unsigned m_count = 0, m_address = 0, m_read_bit = 0;
    uint64_t m_ready_at = 0;
    void reset_command() {
        m_mode = Mode::Command;
        m_shift = m_count = 0;
        m_data_out = true;
    }
    void edge(bool bit, uint64_t now) {
        if (m_mode == Mode::Read) {
            m_data_out = (words[m_address] >> (15 - m_read_bit)) & 1;
            if (++m_read_bit == 16) {
                m_read_bit = 0;
                m_address = (m_address + 1) & 63;
            }
            return;
        }
        if (m_mode == Mode::Done) return;
        if (m_mode == Mode::Command && !m_count && (!bit || now < m_ready_at)) return;
        m_shift = (m_shift << 1) | unsigned(bit);
        ++m_count;
        if (m_mode == Mode::Write || m_mode == Mode::WriteAll) {
            if (m_count == 16) {
                if (m_writable) {
                    if (m_mode == Mode::WriteAll) words.fill(uint16_t(m_shift));
                    else words[m_address] = uint16_t(m_shift);
                    m_ready_at = now + (m_mode == Mode::WriteAll ? 128000 : 28000);
                }
                m_mode = Mode::Done;
                m_data_out = true;
            }
            return;
        }
        if (m_count != 9) return;
        m_address = m_shift & 63;
        switch ((m_shift >> 6) & 3) {
        case 2:
            m_mode = Mode::Read;
            m_read_bit = 0;
            m_data_out = false;
            break;
        case 1:
            m_mode = Mode::Write;
            m_count = m_shift = 0;
            break;
        case 3:
            if (m_writable) {
                words[m_address] = 0xffff;
                m_ready_at = now + 16000;
            }
            m_mode = Mode::Done;
            break;
        case 0:
            switch (m_address >> 4) {
            case 0:
                m_writable = false;
                m_mode = Mode::Done;
                break;
            case 1:
                m_mode = Mode::WriteAll;
                m_count = m_shift = 0;
                break;
            case 2:
                if (m_writable) {
                    words.fill(0xffff);
                    m_ready_at = now + 128000;
                }
                m_mode = Mode::Done;
                break;
            case 3:
                m_writable = true;
                m_mode = Mode::Done;
                break;
            default:
                break;
            }
        default:
            break;
        }
    }
};
static_assert(Snapshotable<Eeprom>);
} // namespace f3rt
