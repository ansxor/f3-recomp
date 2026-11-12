//! `audio/reference/engine.cpp`: low-level cycle-accurate reference audio synthesis engine
//! (ES5505 OTIS + ES5510 ESP DSP).

use crate::es5505::ES5505;
use crate::es5510::ES5510;
use crate::state_io::{StateReader, StateWriter};
use f3rt_audio::memory_map::{
    SOUND_BANK_TABLE_BASE, SOUND_BANK_TABLE_END, SOUND_ES5505_BASE, SOUND_ES5505_END,
    SOUND_ES5510_BASE, SOUND_ES5510_END,
};

const MAX_VOICES: usize = ES5505::MAX_VOICES as usize;
const NUM_CHANNELS: usize = ES5505::NUM_CHANNELS as usize;

fn clamp_f32(v: f32, lo: f32, hi: f32) -> f32 {
    if v < lo {
        lo
    } else if hi < v {
        hi
    } else {
        v
    }
}

pub struct Engine {
    m_es5505: ES5505,
    m_es5510: ES5510,
    m_bank_table: [u16; MAX_VOICES],
    m_bank_mask: u32,
    m_esp_halted: bool,
}

impl Default for Engine {
    fn default() -> Self {
        Self::new()
    }
}

impl Engine {
    pub fn new() -> Self {
        Engine {
            m_es5505: ES5505::default(),
            m_es5510: ES5510::new(),
            m_bank_table: [0; MAX_VOICES],
            m_bank_mask: 7,
            m_esp_halted: true,
        }
    }

    pub fn reset(&mut self) {
        self.m_es5510.reset();
        self.m_bank_table = [0; MAX_VOICES];
    }

    /// # Safety
    /// As `ES5505::set_sample_rom`: the words must outlive every later use of the engine.
    pub unsafe fn set_sample_rom(&mut self, rom_words: *const u16, word_count: usize) {
        unsafe { self.m_es5505.set_sample_rom(rom_words, word_count) };
    }

    pub fn set_bank_mask(&mut self, mask: u32) {
        self.m_bank_mask = mask;
    }

    pub fn bank_mask(&self) -> u32 {
        self.m_bank_mask
    }

    pub fn set_bank_table(&mut self, voice: i32, value: u16) {
        if voice >= 0 && voice < ES5505::MAX_VOICES {
            self.m_bank_table[voice as usize] = value;
        }
    }

    pub fn bank_table(&self, voice: i32) -> u16 {
        if voice >= 0 && voice < ES5505::MAX_VOICES {
            self.m_bank_table[voice as usize]
        } else {
            0
        }
    }

    pub fn set_esp_halted(&mut self, halted: bool) {
        self.m_esp_halted = halted;
    }

    pub fn esp_halted(&self) -> bool {
        self.m_esp_halted
    }

    pub fn sample_rate(&self) -> u32 {
        self.m_es5505.sample_rate()
    }

    pub fn is_mapped(&self, address: u32) -> bool {
        let address = address & 0x00ffffff;
        (address >= SOUND_ES5505_BASE && address < SOUND_ES5505_END)
            || (address >= SOUND_ES5510_BASE && address < SOUND_ES5510_END)
            || (address >= SOUND_BANK_TABLE_BASE && address < SOUND_BANK_TABLE_END)
    }

    pub fn read8(&mut self, address: u32) -> u8 {
        let address = address & 0x00ffffff;
        if address >= SOUND_ES5505_BASE && address < SOUND_ES5505_END {
            let w = self.m_es5505.read((address >> 1) & 0x0f);
            return if address & 1 != 0 { (w & 0xff) as u8 } else { (w >> 8) as u8 };
        }
        if address >= SOUND_ES5510_BASE && address < SOUND_ES5510_END {
            if address & 1 != 0 {
                return self.m_es5510.host_r((address >> 1) & 0xff);
            }
            return 0x00;
        }
        if address >= SOUND_BANK_TABLE_BASE && address < SOUND_BANK_TABLE_END {
            let voice = ((address >> 1) & 0x1f) as usize;
            let val = if voice < MAX_VOICES { self.m_bank_table[voice] } else { 0 };
            return if address & 1 != 0 { (val & 0xff) as u8 } else { (val >> 8) as u8 };
        }
        0xff
    }

    pub fn read16(&mut self, address: u32) -> u16 {
        let address = address & 0x00ffffff;
        if address >= SOUND_ES5505_BASE && address < SOUND_ES5505_END {
            return self.m_es5505.read((address >> 1) & 0x0f);
        }
        if address >= SOUND_ES5510_BASE && address < SOUND_ES5510_END {
            return self.m_es5510.host_r((address >> 1) & 0xff) as u16;
        }
        if address >= SOUND_BANK_TABLE_BASE && address < SOUND_BANK_TABLE_END {
            let voice = ((address >> 1) & 0x1f) as usize;
            return if voice < MAX_VOICES { self.m_bank_table[voice] } else { 0 };
        }
        0xffff
    }

    pub fn read32(&mut self, address: u32) -> u32 {
        let hi = self.read16(address) as u32;
        (hi << 16) | self.read16(address.wrapping_add(2)) as u32
    }

    pub fn write8(&mut self, address: u32, value: u8) {
        let address = address & 0x00ffffff;
        if address >= SOUND_ES5505_BASE && address < SOUND_ES5505_END {
            let mem_mask: u16 = if address & 1 != 0 { 0x00ff } else { 0xff00 };
            let data: u16 = if address & 1 != 0 { value as u16 } else { (value as u16) << 8 };
            self.m_es5505.write((address >> 1) & 0x0f, data, mem_mask);
            return;
        }
        if address >= SOUND_ES5510_BASE && address < SOUND_ES5510_END {
            if address & 1 != 0 {
                self.m_es5510.host_w((address >> 1) & 0xff, value);
            }
            return;
        }
        if address >= SOUND_BANK_TABLE_BASE && address < SOUND_BANK_TABLE_END {
            let voice = ((address >> 1) & 0x1f) as usize;
            if voice < MAX_VOICES {
                if address & 1 != 0 {
                    self.m_bank_table[voice] = (self.m_bank_table[voice] & 0xff00) | value as u16;
                } else {
                    self.m_bank_table[voice] =
                        (self.m_bank_table[voice] & 0x00ff) | ((value as u16) << 8);
                }
                self.m_es5505.set_voice_bank(
                    voice as i32,
                    (self.m_bank_table[voice] as u32 & self.m_bank_mask) << 20,
                );
            }
        }
    }

    pub fn write16(&mut self, address: u32, value: u16) {
        let address = address & 0x00ffffff;
        if address >= SOUND_ES5505_BASE && address < SOUND_ES5505_END {
            self.m_es5505.write((address >> 1) & 0x0f, value, 0xffff);
            return;
        }
        if address >= SOUND_ES5510_BASE && address < SOUND_ES5510_END {
            self.m_es5510.host_w((address >> 1) & 0xff, (value & 0xff) as u8);
            return;
        }
        if address >= SOUND_BANK_TABLE_BASE && address < SOUND_BANK_TABLE_END {
            let voice = ((address >> 1) & 0x1f) as usize;
            if voice < MAX_VOICES {
                self.m_bank_table[voice] = value;
                self.m_es5505
                    .set_voice_bank(voice as i32, (value as u32 & self.m_bank_mask) << 20);
            }
        }
    }

    pub fn write32(&mut self, address: u32, value: u32) {
        self.write16(address, (value >> 16) as u16);
        self.write16(address.wrapping_add(2), (value & 0xffff) as u16);
    }

    /// Generate one PCM frame: ES5505 voice render -> serial DSP -> board gains -> push_frame.
    pub fn generate_one_frame(
        &mut self,
        otis_gain: &[f32; 2],
        output_gain: &[f32; 2],
        push_frame: &mut dyn FnMut(f32, f32),
    ) {
        let mut cursample = [0i32; NUM_CHANNELS];
        self.m_es5505.generate_one_sample(&mut cursample);

        let mut channels = [0.0f32; NUM_CHANNELS];
        for i in 0..NUM_CHANNELS {
            channels[i] = clamp_f32(cursample[i] as f32 / 524288.0f32, -1.0f32, 1.0f32) * otis_gain[i & 1];
        }
        for i in 0..6 {
            self.m_es5510
                .ser_w(i as i32, (channels[i + 2] * 32768.0f32) as i32 as i16);
        }

        if !self.m_esp_halted {
            self.m_es5510.run_once();
        }

        let main_l = self.m_es5510.ser_r(6);
        let main_r = self.m_es5510.ser_r(7);

        let out_l = main_l as f32 / 32768.0f32 * output_gain[0];
        let out_r = main_r as f32 / 32768.0f32 * output_gain[1];

        push_frame(out_l, out_r);
    }

    pub fn save_state(&self, writer: &mut StateWriter) {
        self.m_es5505.save_state(writer);
        self.m_es5510.save_state(writer);
    }

    pub fn load_state(&mut self, reader: &mut StateReader) {
        self.m_es5505.load_state(reader);
        self.m_es5510.load_state(reader);
    }

    pub fn es5505(&mut self) -> &mut ES5505 {
        &mut self.m_es5505
    }

    pub fn es5505_ref(&self) -> &ES5505 {
        &self.m_es5505
    }

    pub fn es5510(&mut self) -> &mut ES5510 {
        &mut self.m_es5510
    }

    pub fn es5510_ref(&self) -> &ES5510 {
        &self.m_es5510
    }
}
