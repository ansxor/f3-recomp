// license:BSD-3-Clause
// copyright-holders:Fabio Priuli, Philip Bennett, hap
/*****************************************************************************
 *
 *  mb87078.cpp - Fujitsu MB87078 6-bit 4-channel electronic volume controller
 *  Standalone implementation for f3rt
 *
 *****************************************************************************/

#include "mb87078.hpp"
#include "state_io.hpp"

#include <cmath>
#include <cstring>

namespace f3rt {

MB87078::MB87078()
    : m_control(0)
    , m_data(0)
{
    // Output volume table: 0dB to -32dB in steps of -0.5dB
    for (int i = 0; i <= 64; i++) {
        m_gains[i] = std::pow(10.0f, (-0.5f * float(i)) / 20.0f);
    }
    m_gains[65] = 0.0f; // -infinity
    m_gains[66] = m_gains[0]; // 0 dB

    for (int i = 0; i < 4; i++) {
        m_gain_index[i] = 66;
        m_channel_latch[i] = 0x7f;
    }

    reset();
}

MB87078::~MB87078() = default;

void MB87078::reset() {
    m_control = 0;
    m_data = 0;

    for (int i = 0; i < 4; i++) {
        m_channel_latch[i] = 0x7f; // 0 dB, all enabled
    }

    gain_recalc();
}

void MB87078::gain_recalc() {
    for (int i = 0; i < 4; i++) {
        uint8_t gain_index;

        // EN = 0: -infinity dB
        if ((~m_channel_latch[i]) & 0x40) {
            gain_index = 65;
        }
        // C32 = 1: -32dB
        else if (m_channel_latch[i] & 0x100) {
            gain_index = 64;
        }
        // C0 = 1: 0dB
        else if (m_channel_latch[i] & 0x80) {
            gain_index = 0;
        } else {
            gain_index = (~m_channel_latch[i]) & 0x3f;
        }

        if (gain_index != m_gain_index[i]) {
            m_gain_index[i] = gain_index;
            if (m_gain_cb) {
                m_gain_cb(i, m_gains[gain_index]);
            }
        }
    }
}

void MB87078::write(uint32_t offset, uint8_t data) {
    if (offset & 1) {
        m_control = data & 0x1f;
    } else {
        m_data = data & 0x3f;
        m_channel_latch[m_control & 3] = ((uint16_t(m_control) << 4) & 0x1c0) | m_data;
        gain_recalc();
    }
}

uint8_t MB87078::read(uint32_t offset) const {
    return (offset & 1) ? m_control : m_data;
}

size_t MB87078::state_size() const {
    return sizeof(CanonicalMB87078);
}

void MB87078::save_state(StateWriter &writer) const {
    CanonicalMB87078 st{};
    for (int i = 0; i < 4; ++i) {
        st.gain_index[i] = m_gain_index[i];
        st.channel_latch[i] = m_channel_latch[i];
    }
    st.control = m_control;
    st.data = m_data;
    writer.write(st);
}

void MB87078::load_state(StateReader &reader) {
    CanonicalMB87078 st;
    reader.read(st);
    for (int i = 0; i < 4; ++i) {
        m_gain_index[i] = st.gain_index[i];
        m_channel_latch[i] = st.channel_latch[i];
    }
    m_control = st.control;
    m_data = st.data;
    gain_recalc();
}

} // namespace f3rt
