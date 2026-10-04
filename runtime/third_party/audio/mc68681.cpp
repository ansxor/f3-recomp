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
    , m_ct_counter(0)
    , m_ct_accum(0)
    , m_half_period(0)
    , m_ct_running(false)
    , m_mr1a(0), m_mr2a(0), m_mr_ptra(0)
    , m_sra(0x0c), m_csra(0), m_cra(0)
    , m_mr1b(0), m_mr2b(0), m_mr_ptrb(0)
    , m_srb(0x0c), m_csrb(0), m_crb(0)
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
    m_ct_counter = 0;
    m_ct_accum = 0;
    m_half_period = 0;
    m_ct_running = false;

    m_mr1a = m_mr2a = m_mr_ptra = 0;
    m_sra = 0x0c; // Tx ready + Tx empty
    m_csra = m_cra = 0;

    m_mr1b = m_mr2b = m_mr_ptrb = 0;
    m_srb = 0x0c; // Tx ready + Tx empty
    m_csrb = m_crb = 0;

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
        return uint8_t(m_ct_counter >> 8);
    case 0x07: // CTLR
        return uint8_t(m_ct_counter & 0xff);

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
        m_ct_counter = std::max<uint16_t>(m_ctr_preset, 1);
        m_ct_running = true;
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
        break;
    case 0x01: // CSRA
        m_csra = data;
        break;
    case 0x02: // CRA
        m_cra = data;
        if (data & 0x10) m_mr_ptra = 0; // reset MR pointer
        break;
    case 0x03: // THRA
        break;

    case 0x04: { // ACR
        uint8_t old_acr = m_acr;
        m_acr = data;
        if ((old_acr ^ data) & 0x40) {
            if (data & 0x40) {
                // Entering timer mode
                m_half_period = 0;
                m_ct_counter = std::max<uint16_t>(m_ctr_preset, 1);
                m_ct_running = true;
            } else {
                m_ct_running = false;
            }
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
        break;
    case 0x09: // CSRB
        m_csrb = data;
        break;
    case 0x0a: // CRB
        m_crb = data;
        if (data & 0x10) m_mr_ptrb = 0; // reset MR pointer
        break;
    case 0x0b: // THRB
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

void MC68681::advance(uint32_t duart_cycles) {
    if (!m_ct_running) return;

    // Determine divider from 4MHz DUART clock based on ACR[6:4]
    uint32_t divider = 1;
    if (m_acr & 0x40) { // Timer mode
        switch ((m_acr >> 4) & 3) {
        case 0: divider = 4; break;  // IP2 (1MHz): 4MHz / 4
        case 1: divider = 64; break; // IP2/16 (62.5kHz): 4MHz / 64
        case 2: divider = 1; break;  // X1/CLK (4MHz)
        case 3: divider = 16; break; // X1/CLK/16 (250kHz)
        }
    } else { // Counter mode
        switch ((m_acr >> 4) & 3) {
        case 0: divider = 4; break;  // IP2
        case 3: divider = 16; break; // X1/16
        default: divider = 4; break;
        }
    }

    m_ct_accum += duart_cycles;
    while (m_ct_accum >= divider) {
        m_ct_accum -= divider;
        --m_ct_counter;
        if (m_ct_counter <= 0) {
            if (m_acr & 0x40) { // Timer mode: square wave toggle
                m_ct_counter = std::max<uint16_t>(m_ctr_preset, 1);
                m_half_period = !m_half_period;
                if (m_half_period == 0) {
                    m_isr |= INT_COUNTER_READY;
                    update_interrupts();
                }
            } else { // Counter mode
                m_ct_counter = 0x10000;
                m_isr |= INT_COUNTER_READY;
                update_interrupts();
            }
        }
    }
}

} // namespace f3rt
