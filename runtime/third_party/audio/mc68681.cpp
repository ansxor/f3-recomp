// license:BSD-3-Clause
// copyright-holders:Mariusz Wojcieszek, R. Belmont
/*****************************************************************************
 *
 *  mc68681.cpp - Motorola MC68681 Dual Asynchronous Receiver/Transmitter
 *  Standalone implementation for f3rt / Taito F3 sound
 *
 *****************************************************************************/

#include "mc68681.hpp"

#include <algorithm>
#include <cstring>

namespace f3rt {

MC68681::MC68681()
    : m_acr(0)
    , m_imr(0)
    , m_isr(0)
    , m_ivr(0x0f) // default uninitialized vector
    , m_opcr(0)
    , m_opr(0)
    , m_ipcr(0)
    , m_ip_last_state(0x0f)
    , m_ctr_preset(0)
    , m_ct_remaining(0)
    , m_half_period(0)
    , m_ct_running(false)
    , m_mr1a(0), m_mr2a(0), m_mr_ptra(0)
    , m_sra(0), m_csra(0), m_cra(0)
    , m_mr1b(0), m_mr2b(0), m_mr_ptrb(0)
    , m_srb(0), m_csrb(0), m_crb(0)
{
    reset();
}

MC68681::~MC68681() = default;

void MC68681::reset() {
    m_acr = 0;
    m_imr = 0;
    m_isr = 0;
    m_ivr = 0x0f;
    m_opcr = 0;
    m_opr = 0;
    m_ipcr = 0;
    m_ip_last_state = 0x0f;

    m_ctr_preset = 0;
    // Match the reference device's pending timer event across board reset.
    // Registers reset, but the scheduled deadline and output phase survive;
    // its next expiration is interpreted using the reset ACR (counter mode).

    m_mr1a = m_mr2a = m_mr_ptra = 0;
    m_sra = 0;
    m_csra = m_cra = 0;

    m_mr1b = m_mr2b = m_mr_ptrb = 0;
    m_srb = 0;
    m_csrb = m_crb = 0;
    for (auto &tx : m_tx) {
        tx.remaining = tx.sent = 0;
        tx.enabled = tx.buffered = false;
        // An idle serial clock retains its edge state across device reset.
    }

    if (m_outport_cb) {
        m_outport_cb(m_opr ^ 0xff); // 0xff: bit 6 is 1 -> ESPHALT active
    }

    update_interrupts();
}

void MC68681::update_interrupts() {
    bool irq = irq_pending();
    if (m_irq_cb) {
        m_irq_cb(irq);
    }
}

void MC68681::tx_interrupt(unsigned channel) {
    auto &status = tx_status(channel);
    const uint8_t mode = channel ? m_mr2b : m_mr2a;
    if (mode & 0x40) status &= ~0x04; // Echo modes do not accept host TX data.
    const uint8_t bit = channel ? INT_TXRDY_B : INT_TXRDY_A;
    m_isr = uint8_t((m_isr & ~bit) | ((status & 0x04) ? bit : 0));
    update_interrupts();
}

uint8_t MC68681::tx_frame_bits(unsigned channel) const {
    const uint8_t mr1 = channel ? m_mr1b : m_mr1a;
    const uint8_t mr2 = channel ? m_mr2b : m_mr2a;
    const unsigned parity = ((mr1 >> 3) & 3) < 2 ? 1 : 0;
    // The reference serial backend represents 1.5 stop bits as two.
    const unsigned stop = (mr2 & 0x0c) < 8 ? 1 : 2;
    return uint8_t(1 + 5 + (mr1 & 3) + parity + stop);
}

void MC68681::tx_clock_select(unsigned channel) {
    static constexpr uint32_t rates[2][13] = {
        {50,110,134,200,300,600,1200,1050,2400,4800,7200,9600,38400},
        {75,110,134,150,300,600,1200,2000,2400,4800,1800,9600,19200}
    };
    const unsigned select = (channel ? m_csrb : m_csra) & 15;
    auto &tx = m_tx[channel];
    // F3 supplies IP3 at 1 MHz and IP5 at 500 kHz.
    const uint32_t external = channel ? 500000 : 1000000;
    tx.baud = select < 13 ? rates[m_acr >> 7][select] :
        select == 14 ? external / 16 : select == 15 ? external : 0;
    tx.phase = 0;
    tx.remaining = 0;
    tx.running = false;
}

void MC68681::tx_command(unsigned channel, uint8_t data) {
    auto &tx = m_tx[channel];
    auto &status = tx_status(channel);
    switch ((data >> 4) & 7) {
    case 1: (channel ? m_mr_ptrb : m_mr_ptra) = 0; break;
    case 2: status &= ~0xf3; break; // Reset disconnected receiver.
    case 3:
        status &= ~0x0c;
        tx.remaining = tx.sent = 0;
        tx.enabled = tx.buffered = false;
        break;
    case 4: status &= ~0xf0; break;
    case 5: m_isr &= ~(channel ? 0x40 : 0x04); break;
    }
    if ((data & 0x04) && !tx.enabled) {
        tx.enabled = true;
        tx.buffered = false;
        status |= 0x0c;
    }
    if (data & 0x08) {
        tx.enabled = tx.buffered = false;
        status &= ~0x0c;
    }
    (channel ? m_crb : m_cra) = data;
    tx_interrupt(channel);
}

void MC68681::tx_write(unsigned channel) {
    // F3 leaves TX pins unconnected; only holding/shift-register occupancy is observable.
    auto &tx = m_tx[channel];
    auto &status = tx_status(channel);
    if (!(status & 0x04)) return; // A full holding register rejects another byte.
    status &= ~0x0c;
    if (tx.remaining) {
        tx.buffered = true;
    } else {
        tx.remaining = tx_frame_bits(channel);
        tx.sent = 0;
        tx.phase = 0;
        tx.running = true;
    }
    tx_interrupt(channel);
}

void MC68681::tx_bit(unsigned channel) {
    auto &tx = m_tx[channel];
    if (!tx.remaining) return;
    auto &status = tx_status(channel);
    const uint8_t before = status;
    // THR becomes ready at the end of the start-bit time.
    if (++tx.sent > 1 && !tx.buffered && tx.enabled) status |= 0x04;
    if (--tx.remaining == 0) {
        if (tx.buffered) {
            tx.remaining = tx_frame_bits(channel);
            tx.sent = 0;
            tx.buffered = false;
            status |= 0x04;
        } else {
            status |= 0x08;
        }
    }
    if (status != before) tx_interrupt(channel);
}

void MC68681::tx_advance(unsigned channel, uint32_t cycles) {
    auto &tx = m_tx[channel];
    if (!tx.running || !tx.baud) return;
    tx.phase += uint64_t(cycles) * tx.baud * 2;
    while (tx.running && tx.phase >= 4000000) {
        tx.phase -= 4000000;
        tx.clock = !tx.clock;
        if (tx.clock) {
            tx_bit(channel);
            if (!tx.remaining) tx.running = false;
        }
    }
}

uint8_t MC68681::get_irq_vector() {
    return m_ivr;
}

uint8_t MC68681::read(uint32_t offset) {
    offset &= 0x0f;
    switch (offset) {
    case 0x00: { // MR1A / MR2A
        uint8_t val = (m_mr_ptra == 0) ? m_mr1a : m_mr2a;
        m_mr_ptra = 1;
        return val;
    }
    case 0x01: // SRA
        return m_sra;
    case 0x03: // RHRA
        return 0;

    case 0x04: { // IPCR
        uint8_t val = m_ipcr;
        m_ipcr &= 0x0f;
        m_isr &= ~INT_INPUT_PORT_CHANGE;
        update_interrupts();
        return val;
    }
    case 0x05: // ISR
        return m_isr;
    case 0x06: // CTUR
        return uint8_t((m_ct_remaining / counter_divider()) >> 8);
    case 0x07: // CTLR
        return uint8_t(m_ct_remaining / counter_divider());

    case 0x08: { // MR1B / MR2B
        uint8_t val = (m_mr_ptrb == 0) ? m_mr1b : m_mr2b;
        m_mr_ptrb = 1;
        return val;
    }
    case 0x09: // SRB
        return m_srb;
    case 0x0a: // 1X/16X Test
        return 0x61;
    case 0x0b: // RHRB
        return 0;

    case 0x0c: // IVR
        return m_ivr;

    case 0x0d: // IP
        return 0xbf; // Bit 7=1, Bit 6=0, other IP pins high

    case 0x0e: // Start counter command
        if (m_acr & 0x40) {
            m_half_period = 0;
        }
        start_counter();
        return 0;

    case 0x0f: // Stop counter command
        if (!(m_acr & 0x40)) {
            m_ct_running = false;
        }
        m_isr &= ~INT_COUNTER_READY;
        update_interrupts();
        return 0;

    default:
        return 0xff;
    }
}

void MC68681::write(uint32_t offset, uint8_t data) {
    offset &= 0x0f;
    switch (offset) {
    case 0x00: // MR1A / MR2A
        if (m_mr_ptra == 0) {
            m_mr1a = data;
            m_mr_ptra = 1;
        } else {
            m_mr2a = data;
        }
        tx_interrupt(0);
        break;
    case 0x01: // CSRA
        m_csra = data;
        tx_clock_select(0);
        break;
    case 0x02: // CRA
        tx_command(0, data);
        break;
    case 0x03: // THRA
        tx_write(0);
        break;

    case 0x04: { // ACR
        uint8_t old_acr = m_acr;
        m_acr = data;
        if ((old_acr ^ data) & 0x40) {
            if (data & 0x40) {
                // Entering timer mode
                m_half_period = 0;
                start_counter();
            } else {
                m_ct_running = false;
            }
        }
        if ((old_acr ^ data) & 0x80) {
            tx_clock_select(0);
            tx_clock_select(1);
        }
        break;
    }
    case 0x05: // IMR
        m_imr = data;
        update_interrupts();
        break;

    case 0x06: // CTUR
        m_ctr_preset = (m_ctr_preset & 0x00ff) | (uint16_t(data) << 8);
        break;
    case 0x07: // CTLR
        m_ctr_preset = (m_ctr_preset & 0xff00) | data;
        break;

    case 0x08: // MR1B / MR2B
        if (m_mr_ptrb == 0) {
            m_mr1b = data;
            m_mr_ptrb = 1;
        } else {
            m_mr2b = data;
        }
        tx_interrupt(1);
        break;
    case 0x09: // CSRB
        m_csrb = data;
        tx_clock_select(1);
        break;
    case 0x0a: // CRB
        tx_command(1, data);
        break;
    case 0x0b: // THRB
        tx_write(1);
        break;

    case 0x0c: // IVR
        m_ivr = data;
        break;
    case 0x0d: // OPCR
        m_opcr = data;
        break;

    case 0x0e: // Set output port bits
        m_opr |= data;
        if (m_outport_cb) {
            m_outport_cb(m_opr ^ 0xff);
        }
        break;

    case 0x0f: // Reset output port bits
        m_opr &= ~data;
        if (m_outport_cb) {
            m_outport_cb(m_opr ^ 0xff);
        }
        break;
    }
}

uint32_t MC68681::counter_divider() const {
    if (m_acr & 0x40) {
        switch ((m_acr >> 4) & 3) {
        case 0: return 4;  // IP2 (1 MHz)
        case 1: return 64; // IP2 / 16
        case 2: return 1;  // X1/CLK (4 MHz)
        default: return 16; // X1/CLK / 16
        }
    }
    return (m_acr & 0x30) == 0x30 ? 16 : 4;
}

void MC68681::start_counter() {
    m_ct_remaining = std::max<uint16_t>(m_ctr_preset, 1) * counter_divider();
    m_ct_running = true;
}

void MC68681::advance(uint32_t duart_cycles) {
    tx_advance(0, duart_cycles);
    tx_advance(1, duart_cycles);
    if (!m_ct_running) return;

    // Store the scheduled duration in crystal clocks, not live ACR units.
    // Preset/source writes affect the next reload, not an already armed event.
    while (duart_cycles >= m_ct_remaining) {
        duart_cycles -= m_ct_remaining;
        if (m_acr & 0x40) {
            m_half_period = !m_half_period;
            for (unsigned channel = 0; channel < 2; ++channel) {
                if (((channel ? m_csrb : m_csra) & 15) == 13 && m_half_period) {
                    auto &tx = m_tx[channel];
                    if (--tx.counter_prescaler == 8) {
                        const bool rising = !tx.clock;
                        tx.clock = true;
                        if (rising) tx_bit(channel);
                    } else if (tx.counter_prescaler == 0) {
                        tx.counter_prescaler = 16;
                        tx.clock = false;
                    }
                }
            }
            if (m_half_period == 0) {
                m_isr |= INT_COUNTER_READY;
                update_interrupts();
            }
            start_counter();
        } else {
            m_isr |= INT_COUNTER_READY;
            update_interrupts();
            m_ct_remaining = 0xffff * counter_divider();
        }
    }
    m_ct_remaining -= duart_cycles;
}

} // namespace f3rt
