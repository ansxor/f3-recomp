// license:BSD-3-Clause
// copyright-holders:Fabio Priuli, Philip Bennett, hap
/*****************************************************************************
 *
 *  mb87078.hpp - Fujitsu MB87078 6-bit 4-channel electronic volume controller
 *  Standalone implementation for f3rt
 *
 *****************************************************************************/

#pragma once

#include <cstdint>
#include <functional>

namespace f3rt {
class StateWriter;
class StateReader;

class MB87078 {
public:
    MB87078();
    ~MB87078();

    void reset();

    // Data write (offset 0 = control register, offset 1 = data register)
    void write(uint32_t offset, uint8_t data);
    uint8_t read(uint32_t offset) const;

    float gain(int channel) const { return m_gains[m_gain_index[channel & 3]]; }

    void set_gain_callback(std::function<void(int channel, float gain)> cb) {
        m_gain_cb = cb;
    }
    size_t state_size() const;
    void save_state(StateWriter &writer) const;
    void load_state(StateReader &reader);

private:
    void gain_recalc();

    float m_gains[64 + 3];
    uint8_t m_gain_index[4];
    uint16_t m_channel_latch[4];
    uint8_t m_control;
    uint8_t m_data;

    std::function<void(int channel, float gain)> m_gain_cb;
};

} // namespace f3rt
