//! `mb87078.cpp` - Fujitsu MB87078 6-bit 4-channel electronic volume controller.

use crate::state_io::{StateReader, StateWriter, require_state};

pub struct MB87078 {
    m_gains: [f32; 64 + 3],
    m_gain_index: [u8; 4],
    m_channel_latch: [u16; 4],
    m_control: u8,
    m_data: u8,
    m_gain_cb: Option<Box<dyn FnMut(i32, f32)>>,
}

impl Default for MB87078 {
    fn default() -> Self {
        Self::new()
    }
}

impl MB87078 {
    pub fn new() -> Self {
        let mut m_gains = [0.0f32; 64 + 3];
        for i in 0..=64 {
            m_gains[i] = 10.0f32.powf((-0.5f32 * i as f32) / 20.0f32);
        }
        m_gains[65] = 0.0f32;
        m_gains[66] = m_gains[0];

        let mut this = MB87078 {
            m_gains,
            m_gain_index: [66; 4],
            m_channel_latch: [0x7f; 4],
            m_control: 0,
            m_data: 0,
            m_gain_cb: None,
        };
        this.reset();
        this
    }

    pub fn reset(&mut self) {
        self.m_control = 0;
        self.m_data = 0;

        for i in 0..4 {
            self.m_channel_latch[i] = 0x7f;
        }

        self.gain_recalc();
    }

    pub fn gain(&self, channel: i32) -> f32 {
        self.m_gains[self.m_gain_index[(channel & 3) as usize] as usize]
    }

    pub fn set_gain_callback(&mut self, cb: Box<dyn FnMut(i32, f32)>) {
        self.m_gain_cb = Some(cb);
    }

    fn gain_recalc(&mut self) {
        for i in 0..4 {
            let latch = self.m_channel_latch[i];
            let gain_index: u8 = if (!latch) & 0x40 != 0 {
                65
            } else if latch & 0x100 != 0 {
                64
            } else if latch & 0x80 != 0 {
                0
            } else {
                ((!latch) & 0x3f) as u8
            };

            if gain_index != self.m_gain_index[i] {
                self.m_gain_index[i] = gain_index;
                if let Some(cb) = &mut self.m_gain_cb {
                    cb(i as i32, self.m_gains[gain_index as usize]);
                }
            }
        }
    }

    pub fn write(&mut self, offset: u32, data: u8) {
        if offset & 1 != 0 {
            self.m_control = data & 0x1f;
        } else {
            self.m_data = data & 0x3f;
            self.m_channel_latch[(self.m_control & 3) as usize] =
                (((self.m_control as u16) << 4) & 0x1c0) | self.m_data as u16;
            self.gain_recalc();
        }
    }

    pub fn read(&self, offset: u32) -> u8 {
        if offset & 1 != 0 { self.m_control } else { self.m_data }
    }

    pub fn state_size(&self) -> usize {
        4 + 4 * 2 + 1 + 1
    }

    pub fn save_state(&self, writer: &mut StateWriter) {
        for i in 0..4 {
            writer.write_u8(self.m_gain_index[i]);
        }
        for i in 0..4 {
            writer.write_u16(self.m_channel_latch[i]);
        }
        writer.write_u8(self.m_control);
        writer.write_u8(self.m_data);
    }

    pub fn load_state(&mut self, reader: &mut StateReader) {
        let mut gain_index = [0u8; 4];
        let mut channel_latch = [0u16; 4];
        for g in &mut gain_index {
            *g = reader.read_u8();
        }
        for l in &mut channel_latch {
            *l = reader.read_u16();
        }
        for g in gain_index {
            require_state(g < 67);
        }
        let control = reader.read_u8();
        let data = reader.read_u8();
        for i in 0..4 {
            self.m_gain_index[i] = gain_index[i];
            self.m_channel_latch[i] = channel_latch[i];
        }
        self.m_control = control;
        self.m_data = data;
        self.gain_recalc();
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn reset_is_unity_and_latch_selects_attenuation() {
        let mut vol = MB87078::new();
        assert_eq!(vol.gain(0), 1.0);
        vol.write(1, 0x04);
        vol.write(0, 0x3b);
        assert_eq!(vol.gain(0), 10.0f32.powf((-0.5f32 * 4.0f32) / 20.0f32));
        vol.write(1, 0x01);
        vol.write(0, 0x00);
        assert_eq!(vol.gain(1), 0.0);
    }
}
