// license:BSD-3-Clause
// copyright-holders:Mariusz Wojcieszek, R. Belmont
/*****************************************************************************
 *
 *  mc68681.hpp - Motorola MC68681 Dual Asynchronous Receiver/Transmitter
 *  Standalone implementation for f3rt / Taito F3 sound
 *
 *****************************************************************************/

#pragma once

#include <array>
#include <cstdint>
#include <functional>

namespace f3rt {

class MC68681 {
public:
    static constexpr uint8_t INT_TXRDY_A            = 0x01;
    static constexpr uint8_t INT_RXRDY_A            = 0x02;
    static constexpr uint8_t INT_COUNTER_READY      = 0x08;
    static constexpr uint8_t INT_TXRDY_B            = 0x10;
    static constexpr uint8_t INT_RXRDY_B            = 0x20;
    static constexpr uint8_t INT_INPUT_PORT_CHANGE  = 0x80;

    MC68681();
    ~MC68681();

    void reset();

    // 8-bit register read and write (offset 0x00 .. 0x0f)
    uint8_t read(uint32_t offset);
    void write(uint32_t offset, uint8_t data);

    // CPU space interrupt acknowledge returns IVR
    uint8_t get_irq_vector();

    bool irq_pending() const { return (m_isr & m_imr) != 0; }

    // Advance DUART by 4MHz clock ticks
    void advance(uint32_t duart_cycles);

    void set_irq_callback(std::function<void(bool state)> cb) {
        m_irq_cb = cb;
    }

    void set_outport_callback(std::function<void(uint8_t data)> cb) {
        m_outport_cb = cb;
    }

    uint8_t output_port() const { return m_opr ^ 0xff; }

private:
    void update_interrupts();
    uint32_t counter_divider() const;
    void start_counter();
    void tx_interrupt(unsigned channel);
    void tx_clock_select(unsigned channel);
    void tx_command(unsigned channel, uint8_t data);
    void tx_write(unsigned channel);
    void tx_bit(unsigned channel);
    void tx_advance(unsigned channel, uint32_t cycles);
    uint8_t tx_frame_bits(unsigned channel) const;
    uint8_t &tx_status(unsigned channel) { return channel ? m_srb : m_sra; }
    struct Transmitter {
        uint64_t phase = 0;
        uint32_t baud = 0;
        uint8_t remaining = 0, sent = 0;
        uint8_t counter_prescaler = 16;
        bool clock = false, running = false, enabled = false, buffered = false;
    };
    std::array<Transmitter, 2> m_tx{};

    uint8_t m_acr;
    uint8_t m_imr;
    uint8_t m_isr;
    uint8_t m_ivr;
    uint8_t m_opcr;
    uint8_t m_opr;
    uint8_t m_ipcr;
    uint8_t m_ip_last_state;

    uint16_t m_ctr_preset;
    uint32_t m_ct_remaining;
    uint8_t  m_half_period;
    bool     m_ct_running;

    // Channel A
    uint8_t m_mr1a, m_mr2a, m_mr_ptra;
    uint8_t m_sra, m_csra, m_cra;

    // Channel B
    uint8_t m_mr1b, m_mr2b, m_mr_ptrb;
    uint8_t m_srb, m_csrb, m_crb;

    std::function<void(bool state)> m_irq_cb;
    std::function<void(uint8_t data)> m_outport_cb;
};

} // namespace f3rt
