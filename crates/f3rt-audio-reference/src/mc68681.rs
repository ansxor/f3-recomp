//! `mc68681.cpp` - Motorola MC68681 Dual Asynchronous Receiver/Transmitter.

use crate::state_io::{StateReader, StateWriter, require_state};

#[derive(Clone, Copy)]
struct Transmitter {
    phase: u64,
    baud: u32,
    remaining: u8,
    sent: u8,
    counter_prescaler: u8,
    clock: bool,
    running: bool,
    enabled: bool,
    buffered: bool,
}

impl Default for Transmitter {
    fn default() -> Self {
        Transmitter {
            phase: 0,
            baud: 0,
            remaining: 0,
            sent: 0,
            counter_prescaler: 16,
            clock: false,
            running: false,
            enabled: false,
            buffered: false,
        }
    }
}

pub struct MC68681 {
    m_tx: [Transmitter; 2],

    m_acr: u8,
    m_imr: u8,
    m_isr: u8,
    m_ivr: u8,
    m_opcr: u8,
    m_opr: u8,
    m_ipcr: u8,
    m_ip_last_state: u8,

    m_ctr_preset: u16,
    m_ct_remaining: u32,
    m_half_period: u8,
    m_ct_running: bool,

    m_mr1a: u8,
    m_mr2a: u8,
    m_mr_ptra: u8,
    m_sra: u8,
    m_csra: u8,
    m_cra: u8,

    m_mr1b: u8,
    m_mr2b: u8,
    m_mr_ptrb: u8,
    m_srb: u8,
    m_csrb: u8,
    m_crb: u8,

    m_irq_cb: Option<Box<dyn FnMut(bool)>>,
    m_outport_cb: Option<Box<dyn FnMut(u8)>>,
}

impl Default for MC68681 {
    fn default() -> Self {
        Self::new()
    }
}

impl MC68681 {
    pub const INT_TXRDY_A: u8 = 0x01;
    pub const INT_RXRDY_A: u8 = 0x02;
    pub const INT_COUNTER_READY: u8 = 0x08;
    pub const INT_TXRDY_B: u8 = 0x10;
    pub const INT_RXRDY_B: u8 = 0x20;
    pub const INT_INPUT_PORT_CHANGE: u8 = 0x80;

    pub fn new() -> Self {
        let mut this = MC68681 {
            m_tx: [Transmitter::default(); 2],
            m_acr: 0,
            m_imr: 0,
            m_isr: 0,
            m_ivr: 0x0f,
            m_opcr: 0,
            m_opr: 0,
            m_ipcr: 0,
            m_ip_last_state: 0x0f,
            m_ctr_preset: 0,
            m_ct_remaining: 0,
            m_half_period: 0,
            m_ct_running: false,
            m_mr1a: 0,
            m_mr2a: 0,
            m_mr_ptra: 0,
            m_sra: 0,
            m_csra: 0,
            m_cra: 0,
            m_mr1b: 0,
            m_mr2b: 0,
            m_mr_ptrb: 0,
            m_srb: 0,
            m_csrb: 0,
            m_crb: 0,
            m_irq_cb: None,
            m_outport_cb: None,
        };
        this.reset();
        this
    }

    pub fn reset(&mut self) {
        self.m_acr = 0;
        self.m_imr = 0;
        self.m_isr = 0;
        self.m_ivr = 0x0f;
        self.m_opcr = 0;
        self.m_opr = 0;
        self.m_ipcr = 0;
        self.m_ip_last_state = 0x0f;

        self.m_ctr_preset = 0;

        self.m_mr1a = 0;
        self.m_mr2a = 0;
        self.m_mr_ptra = 0;
        self.m_sra = 0;
        self.m_csra = 0;
        self.m_cra = 0;

        self.m_mr1b = 0;
        self.m_mr2b = 0;
        self.m_mr_ptrb = 0;
        self.m_srb = 0;
        self.m_csrb = 0;
        self.m_crb = 0;
        for tx in &mut self.m_tx {
            tx.remaining = 0;
            tx.sent = 0;
            tx.enabled = false;
            tx.buffered = false;
        }

        let out = self.m_opr ^ 0xff;
        if let Some(cb) = &mut self.m_outport_cb {
            cb(out);
        }

        self.update_interrupts();
    }

    pub fn get_irq_vector(&mut self) -> u8 {
        self.m_ivr
    }

    pub fn irq_pending(&self) -> bool {
        (self.m_isr & self.m_imr) != 0
    }

    pub fn set_irq_callback(&mut self, cb: Box<dyn FnMut(bool)>) {
        self.m_irq_cb = Some(cb);
    }

    pub fn set_outport_callback(&mut self, cb: Box<dyn FnMut(u8)>) {
        self.m_outport_cb = Some(cb);
    }

    pub fn output_port(&self) -> u8 {
        self.m_opr ^ 0xff
    }

    fn update_interrupts(&mut self) {
        let irq = self.irq_pending();
        if let Some(cb) = &mut self.m_irq_cb {
            cb(irq);
        }
    }

    fn outport_notify(&mut self) {
        let out = self.m_opr ^ 0xff;
        if let Some(cb) = &mut self.m_outport_cb {
            cb(out);
        }
    }

    fn tx_status(&mut self, channel: u32) -> &mut u8 {
        if channel != 0 { &mut self.m_srb } else { &mut self.m_sra }
    }

    fn tx_interrupt(&mut self, channel: u32) {
        let mode = if channel != 0 { self.m_mr2b } else { self.m_mr2a };
        let status = self.tx_status(channel);
        if mode & 0x40 != 0 {
            *status &= !0x04;
        }
        let status = *status;
        let bit = if channel != 0 { Self::INT_TXRDY_B } else { Self::INT_TXRDY_A };
        self.m_isr = (self.m_isr & !bit) | (if status & 0x04 != 0 { bit } else { 0 });
        self.update_interrupts();
    }

    fn tx_frame_bits(&self, channel: u32) -> u8 {
        let mr1 = if channel != 0 { self.m_mr1b } else { self.m_mr1a };
        let mr2 = if channel != 0 { self.m_mr2b } else { self.m_mr2a };
        let parity: u32 = if ((mr1 >> 3) & 3) < 2 { 1 } else { 0 };
        let stop: u32 = if (mr2 & 0x0c) < 8 { 1 } else { 2 };
        (1 + 5 + (mr1 & 3) as u32 + parity + stop) as u8
    }

    fn tx_clock_select(&mut self, channel: u32) {
        const RATES: [[u32; 13]; 2] = [
            [50, 110, 134, 200, 300, 600, 1200, 1050, 2400, 4800, 7200, 9600, 38400],
            [75, 110, 134, 150, 300, 600, 1200, 2000, 2400, 4800, 1800, 9600, 19200],
        ];
        let select = (if channel != 0 { self.m_csrb } else { self.m_csra } & 15) as usize;
        let external: u32 = if channel != 0 { 500000 } else { 1000000 };
        let baud = if select < 13 {
            RATES[(self.m_acr >> 7) as usize][select]
        } else if select == 14 {
            external / 16
        } else if select == 15 {
            external
        } else {
            0
        };
        let tx = &mut self.m_tx[channel as usize];
        tx.baud = baud;
        tx.phase = 0;
        tx.remaining = 0;
        tx.running = false;
    }

    fn tx_command(&mut self, channel: u32, data: u8) {
        let mut status = *self.tx_status(channel);
        let ch = channel as usize;
        match (data >> 4) & 7 {
            1 => {
                if channel != 0 {
                    self.m_mr_ptrb = 0;
                } else {
                    self.m_mr_ptra = 0;
                }
            }
            2 => status &= !0xf3,
            3 => {
                status &= !0x0c;
                self.m_tx[ch].remaining = 0;
                self.m_tx[ch].sent = 0;
                self.m_tx[ch].enabled = false;
                self.m_tx[ch].buffered = false;
            }
            4 => status &= !0xf0,
            5 => self.m_isr &= !(if channel != 0 { 0x40 } else { 0x04 }),
            _ => {}
        }
        if (data & 0x04) != 0 && !self.m_tx[ch].enabled {
            self.m_tx[ch].enabled = true;
            self.m_tx[ch].buffered = false;
            status |= 0x0c;
        }
        if data & 0x08 != 0 {
            self.m_tx[ch].enabled = false;
            self.m_tx[ch].buffered = false;
            status &= !0x0c;
        }
        *self.tx_status(channel) = status;
        if channel != 0 {
            self.m_crb = data;
        } else {
            self.m_cra = data;
        }
        self.tx_interrupt(channel);
    }

    fn tx_write(&mut self, channel: u32) {
        let ch = channel as usize;
        let mut status = *self.tx_status(channel);
        if status & 0x04 == 0 {
            return;
        }
        status &= !0x0c;
        *self.tx_status(channel) = status;
        if self.m_tx[ch].remaining != 0 {
            self.m_tx[ch].buffered = true;
        } else {
            let bits = self.tx_frame_bits(channel);
            let tx = &mut self.m_tx[ch];
            tx.remaining = bits;
            tx.sent = 0;
            tx.phase = 0;
            tx.running = true;
        }
        self.tx_interrupt(channel);
    }

    fn tx_bit(&mut self, channel: u32) {
        let ch = channel as usize;
        if self.m_tx[ch].remaining == 0 {
            return;
        }
        let mut status = *self.tx_status(channel);
        let before = status;
        self.m_tx[ch].sent = self.m_tx[ch].sent.wrapping_add(1);
        if self.m_tx[ch].sent > 1 && !self.m_tx[ch].buffered && self.m_tx[ch].enabled {
            status |= 0x04;
        }
        self.m_tx[ch].remaining = self.m_tx[ch].remaining.wrapping_sub(1);
        if self.m_tx[ch].remaining == 0 {
            if self.m_tx[ch].buffered {
                let bits = self.tx_frame_bits(channel);
                let tx = &mut self.m_tx[ch];
                tx.remaining = bits;
                tx.sent = 0;
                tx.buffered = false;
                status |= 0x04;
            } else {
                status |= 0x08;
            }
        }
        *self.tx_status(channel) = status;
        if status != before {
            self.tx_interrupt(channel);
        }
    }

    fn tx_advance(&mut self, channel: u32, cycles: u32) {
        let ch = channel as usize;
        if !self.m_tx[ch].running || self.m_tx[ch].baud == 0 {
            return;
        }
        let baud = self.m_tx[ch].baud;
        self.m_tx[ch].phase = self.m_tx[ch]
            .phase
            .wrapping_add((cycles as u64).wrapping_mul(baud as u64).wrapping_mul(2));
        while self.m_tx[ch].running && self.m_tx[ch].phase >= 4000000 {
            self.m_tx[ch].phase -= 4000000;
            self.m_tx[ch].clock = !self.m_tx[ch].clock;
            if self.m_tx[ch].clock {
                self.tx_bit(channel);
                if self.m_tx[ch].remaining == 0 {
                    self.m_tx[ch].running = false;
                }
            }
        }
    }

    pub fn read(&mut self, offset: u32) -> u8 {
        let offset = offset & 0x0f;
        match offset {
            0x00 => {
                let val = if self.m_mr_ptra == 0 { self.m_mr1a } else { self.m_mr2a };
                self.m_mr_ptra = 1;
                val
            }
            0x01 => self.m_sra,
            0x03 => 0,

            0x04 => {
                let val = self.m_ipcr;
                self.m_ipcr &= 0x0f;
                self.m_isr &= !Self::INT_INPUT_PORT_CHANGE;
                self.update_interrupts();
                val
            }
            0x05 => self.m_isr,
            0x06 => ((self.m_ct_remaining / self.counter_divider()) >> 8) as u8,
            0x07 => (self.m_ct_remaining / self.counter_divider()) as u8,

            0x08 => {
                let val = if self.m_mr_ptrb == 0 { self.m_mr1b } else { self.m_mr2b };
                self.m_mr_ptrb = 1;
                val
            }
            0x09 => self.m_srb,
            0x0a => 0x61,
            0x0b => 0,

            0x0c => self.m_ivr,

            0x0d => 0xbf,

            0x0e => {
                if self.m_acr & 0x40 != 0 {
                    self.m_half_period = 0;
                }
                self.start_counter();
                0
            }

            0x0f => {
                if self.m_acr & 0x40 == 0 {
                    self.m_ct_running = false;
                }
                self.m_isr &= !Self::INT_COUNTER_READY;
                self.update_interrupts();
                0
            }

            _ => 0xff,
        }
    }

    pub fn write(&mut self, offset: u32, data: u8) {
        let offset = offset & 0x0f;
        match offset {
            0x00 => {
                if self.m_mr_ptra == 0 {
                    self.m_mr1a = data;
                    self.m_mr_ptra = 1;
                } else {
                    self.m_mr2a = data;
                }
                self.tx_interrupt(0);
            }
            0x01 => {
                self.m_csra = data;
                self.tx_clock_select(0);
            }
            0x02 => self.tx_command(0, data),
            0x03 => self.tx_write(0),

            0x04 => {
                let old_acr = self.m_acr;
                self.m_acr = data;
                if (old_acr ^ data) & 0x40 != 0 {
                    if data & 0x40 != 0 {
                        self.m_half_period = 0;
                        self.start_counter();
                    } else {
                        self.m_ct_running = false;
                    }
                }
                if (old_acr ^ data) & 0x80 != 0 {
                    self.tx_clock_select(0);
                    self.tx_clock_select(1);
                }
            }
            0x05 => {
                self.m_imr = data;
                self.update_interrupts();
            }

            0x06 => self.m_ctr_preset = (self.m_ctr_preset & 0x00ff) | ((data as u16) << 8),
            0x07 => self.m_ctr_preset = (self.m_ctr_preset & 0xff00) | data as u16,

            0x08 => {
                if self.m_mr_ptrb == 0 {
                    self.m_mr1b = data;
                    self.m_mr_ptrb = 1;
                } else {
                    self.m_mr2b = data;
                }
                self.tx_interrupt(1);
            }
            0x09 => {
                self.m_csrb = data;
                self.tx_clock_select(1);
            }
            0x0a => self.tx_command(1, data),
            0x0b => self.tx_write(1),

            0x0c => self.m_ivr = data,
            0x0d => self.m_opcr = data,

            0x0e => {
                self.m_opr |= data;
                self.outport_notify();
            }

            0x0f => {
                self.m_opr &= !data;
                self.outport_notify();
            }
            _ => {}
        }
    }

    fn counter_divider(&self) -> u32 {
        if self.m_acr & 0x40 != 0 {
            return match (self.m_acr >> 4) & 3 {
                0 => 4,
                1 => 64,
                2 => 1,
                _ => 16,
            };
        }
        if (self.m_acr & 0x30) == 0x30 { 16 } else { 4 }
    }

    fn start_counter(&mut self) {
        self.m_ct_remaining = (self.m_ctr_preset.max(1) as u32).wrapping_mul(self.counter_divider());
        self.m_ct_running = true;
    }

    pub fn advance(&mut self, mut duart_cycles: u32) {
        self.tx_advance(0, duart_cycles);
        self.tx_advance(1, duart_cycles);
        if !self.m_ct_running {
            return;
        }

        while duart_cycles >= self.m_ct_remaining {
            duart_cycles -= self.m_ct_remaining;
            if self.m_acr & 0x40 != 0 {
                self.m_half_period = (self.m_half_period == 0) as u8;
                for channel in 0..2u32 {
                    let csr = if channel != 0 { self.m_csrb } else { self.m_csra };
                    if (csr & 15) == 13 && self.m_half_period != 0 {
                        let ch = channel as usize;
                        self.m_tx[ch].counter_prescaler = self.m_tx[ch].counter_prescaler.wrapping_sub(1);
                        if self.m_tx[ch].counter_prescaler == 8 {
                            let rising = !self.m_tx[ch].clock;
                            self.m_tx[ch].clock = true;
                            if rising {
                                self.tx_bit(channel);
                            }
                        } else if self.m_tx[ch].counter_prescaler == 0 {
                            self.m_tx[ch].counter_prescaler = 16;
                            self.m_tx[ch].clock = false;
                        }
                    }
                }
                if self.m_half_period == 0 {
                    self.m_isr |= Self::INT_COUNTER_READY;
                    self.update_interrupts();
                }
                self.start_counter();
            } else {
                self.m_isr |= Self::INT_COUNTER_READY;
                self.update_interrupts();
                self.m_ct_remaining = 0xffffu32.wrapping_mul(self.counter_divider());
            }
        }
        self.m_ct_remaining -= duart_cycles;
    }

    const STATE_SIZE: usize = 8 + 2 + 4 + 1 + 1 + 12 + 2 * (8 + 4 + 7);

    pub fn state_size(&self) -> usize {
        Self::STATE_SIZE
    }

    pub fn save_state(&self, writer: &mut StateWriter) {
        writer.write_u8(self.m_acr);
        writer.write_u8(self.m_imr);
        writer.write_u8(self.m_isr);
        writer.write_u8(self.m_ivr);
        writer.write_u8(self.m_opcr);
        writer.write_u8(self.m_opr);
        writer.write_u8(self.m_ipcr);
        writer.write_u8(self.m_ip_last_state);
        writer.write_u16(self.m_ctr_preset);
        writer.write_u32(self.m_ct_remaining);
        writer.write_u8(self.m_half_period);
        writer.write_bool(self.m_ct_running);
        for v in [self.m_mr1a, self.m_mr2a, self.m_mr_ptra, self.m_sra, self.m_csra, self.m_cra] {
            writer.write_u8(v);
        }
        for v in [self.m_mr1b, self.m_mr2b, self.m_mr_ptrb, self.m_srb, self.m_csrb, self.m_crb] {
            writer.write_u8(v);
        }
        for tx in &self.m_tx {
            writer.write_u64(tx.phase);
            writer.write_u32(tx.baud);
            writer.write_u8(tx.remaining);
            writer.write_u8(tx.sent);
            writer.write_u8(tx.counter_prescaler);
            writer.write_bool(tx.clock);
            writer.write_bool(tx.running);
            writer.write_bool(tx.enabled);
            writer.write_bool(tx.buffered);
        }
    }

    pub fn load_state(&mut self, reader: &mut StateReader) {
        let mut record = [0u8; MC68681::STATE_SIZE];
        reader.read_bytes(&mut record);
        let ct_remaining = u32::from_le_bytes([record[10], record[11], record[12], record[13]]);
        let ct_running = record[15];
        require_state(ct_running <= 1 && (ct_running == 0 || ct_remaining != 0));
        let reader = &mut StateReader::new(&record);
        self.m_acr = reader.read_u8();
        self.m_imr = reader.read_u8();
        self.m_isr = reader.read_u8();
        self.m_ivr = reader.read_u8();
        self.m_opcr = reader.read_u8();
        self.m_opr = reader.read_u8();
        self.m_ipcr = reader.read_u8();
        self.m_ip_last_state = reader.read_u8();
        self.m_ctr_preset = reader.read_u16();
        self.m_ct_remaining = reader.read_u32();
        self.m_half_period = reader.read_u8();
        self.m_ct_running = reader.read_u8() != 0;
        self.m_mr1a = reader.read_u8();
        self.m_mr2a = reader.read_u8();
        self.m_mr_ptra = reader.read_u8();
        self.m_sra = reader.read_u8();
        self.m_csra = reader.read_u8();
        self.m_cra = reader.read_u8();
        self.m_mr1b = reader.read_u8();
        self.m_mr2b = reader.read_u8();
        self.m_mr_ptrb = reader.read_u8();
        self.m_srb = reader.read_u8();
        self.m_csrb = reader.read_u8();
        self.m_crb = reader.read_u8();
        for i in 0..2 {
            let tx = &mut self.m_tx[i];
            tx.phase = reader.read_u64();
            tx.baud = reader.read_u32();
            tx.remaining = reader.read_u8();
            tx.sent = reader.read_u8();
            tx.counter_prescaler = reader.read_u8();
            tx.clock = reader.read_u8() != 0;
            tx.running = reader.read_u8() != 0;
            tx.enabled = reader.read_u8() != 0;
            tx.buffered = reader.read_u8() != 0;
        }
        self.outport_notify();
        self.update_interrupts();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reset_vector_and_output_port_inversion() {
        let mut duart = MC68681::new();
        assert_eq!(duart.get_irq_vector(), 0x0f);
        assert_eq!(duart.output_port(), 0xff);
        duart.write(0x0e, 0x40);
        assert_eq!(duart.output_port(), 0xbf);
        duart.write(0x0f, 0x40);
        assert_eq!(duart.output_port(), 0xff);
    }

    #[test]
    fn counter_mode_expiry_raises_counter_ready() {
        let mut duart = MC68681::new();
        duart.write(0x06, 0x00);
        duart.write(0x07, 0x02);
        duart.write(0x05, MC68681::INT_COUNTER_READY);
        duart.read(0x0e);
        duart.advance(7);
        assert!(!duart.irq_pending());
        duart.advance(1);
        assert!(duart.irq_pending());
        assert_eq!(duart.read(0x05), MC68681::INT_COUNTER_READY);
    }
}
