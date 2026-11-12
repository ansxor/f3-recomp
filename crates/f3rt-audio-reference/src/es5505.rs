//! `es5505.cpp` - Ensoniq ES5505 OTIS sound generator.
//!
//! The sample ROM is a non-owning `*const u16` + word count, exactly as the C++ keeps its
//! `const uint16_t *`; the caller guarantees the words outlive every later `read`/`write`/
//! `generate_one_sample`, and `set_sample_rom` is therefore `unsafe`.

use crate::state_io::{StateReader, StateWriter, require_state};

const FINE_FILTER_BIT: u32 = 16;
const FILTER_BIT: u32 = 12;
const FILTER_SHIFT: u32 = FINE_FILTER_BIT - FILTER_BIT;

const ULAW_MAXBITS: u32 = 8;

const CONTROL_IRQ: u32 = 0x0080;
const CONTROL_DIR: u32 = 0x0040;
const CONTROL_IRQE: u32 = 0x0020;
const CONTROL_BLE: u32 = 0x0010;
const CONTROL_LPE: u32 = 0x0008;
const CONTROL_STOP1: u32 = 0x0002;
const CONTROL_STOP0: u32 = 0x0001;

const CONTROL_LOOPMASK: u32 = CONTROL_BLE | CONTROL_LPE;
const CONTROL_STOPMASK: u32 = CONTROL_STOP1 | CONTROL_STOP0;

const MAX_VOICES_USIZE: usize = ES5505::MAX_VOICES as usize;
const NUM_CHANNELS_USIZE: usize = ES5505::NUM_CHANNELS as usize;

const ADDRESS_FRAC_BIT: i32 = 11;
const ADDRESS_ACC_MASK: u64 = 0x7fffffff;

fn get_address_acc_shifted_val(val: u64, bias: i32) -> u64 {
    let shift = 2 - bias;
    if shift >= 0 { val << shift } else { val >> (-shift) }
}

fn get_address_acc_res(val: u64, bias: i32) -> u64 {
    let shift = 2 - bias;
    if shift >= 0 { val >> shift } else { val << (-shift) }
}

fn get_integer_addr(accum: u64) -> u64 {
    (accum & ADDRESS_ACC_MASK) >> ADDRESS_FRAC_BIT
}

fn apply_lowpass(out: i32, cutoff: i32, input: i32) -> i32 {
    ((cutoff >> FILTER_SHIFT).wrapping_mul(out.wrapping_sub(input)) / (1 << FILTER_BIT)).wrapping_add(input)
}

fn apply_highpass(out: i32, cutoff: i32, input: i32, prev: i32) -> i32 {
    out.wrapping_sub(prev)
        .wrapping_add((cutoff >> FILTER_SHIFT).wrapping_mul(input) / (1 << (FILTER_BIT + 1)))
        .wrapping_add(input / 2)
}

#[derive(Clone, Copy)]
pub struct Voice {
    pub control: u32,
    pub freqcount: u64,
    pub start: u64,
    pub lvol: u32,
    pub end: u64,
    pub lvramp: u32,
    pub accum: u64,
    pub rvol: u32,
    pub rvramp: u32,
    pub ecount: u32,
    pub k2: u32,
    pub k2ramp: u32,
    pub k1: u32,
    pub k1ramp: u32,
    pub o4n1: i32,
    pub o3n1: i32,
    pub o3n2: i32,
    pub o2n1: i32,
    pub o2n2: i32,
    pub o1n1: i32,
    pub index: u8,
    pub filtcount: u8,
}

impl Default for Voice {
    fn default() -> Self {
        Voice {
            control: 0x0001,
            freqcount: 0,
            start: 0,
            lvol: 0x80,
            end: 0,
            lvramp: 0,
            accum: 0,
            rvol: 0x80,
            rvramp: 0,
            ecount: 0,
            k2: 0,
            k2ramp: 0,
            k1: 0,
            k1ramp: 0,
            o4n1: 0,
            o3n1: 0,
            o3n2: 0,
            o2n1: 0,
            o2n2: 0,
            o1n1: 0,
            index: 0,
            filtcount: 0,
        }
    }
}

pub struct ES5505 {
    m_master_clock: u32,
    m_sample_rate: u32,
    m_active_voices: u8,
    m_current_page: u8,
    m_irqv: u8,
    m_mode: u16,
    m_voice_index: i32,

    pub m_voices: [Voice; MAX_VOICES_USIZE],
    m_voice_bank: [u32; MAX_VOICES_USIZE],

    m_rom: *const u16,
    m_rom_words: usize,

    m_ulaw_lookup: Vec<i16>,
    m_volume_lookup: Vec<u32>,

    m_irq_cb: Option<Box<dyn FnMut(bool)>>,
    m_sample_rate_cb: Option<Box<dyn FnMut(u32)>>,
}

impl Default for ES5505 {
    fn default() -> Self {
        Self::new(15238090)
    }
}

impl ES5505 {
    pub const MAX_VOICES: i32 = 32;
    pub const NUM_CHANNELS: i32 = 8;

    const STATE_SIZE: usize = 4 + 4 + 1 + 1 + 1 + 2 + 4 + 4 * 32 + 32 * ES5505::VOICE_STATE_SIZE;
    const VOICE_STATE_SIZE: usize = 4 + 8 + 8 + 4 + 8 + 4 + 8 + 4 + 4 + 4 + 4 + 4 + 4 + 4 + 6 * 4 + 1 + 1;

    pub fn new(master_clock: u32) -> Self {
        let mut this = ES5505 {
            m_master_clock: master_clock,
            m_sample_rate: 0,
            m_active_voices: 31,
            m_current_page: 0,
            m_irqv: 0x80,
            m_mode: 0x7f8,
            m_voice_index: 0,
            m_voices: [Voice::default(); MAX_VOICES_USIZE],
            m_voice_bank: [0; MAX_VOICES_USIZE],
            m_rom: std::ptr::null(),
            m_rom_words: 0,
            m_ulaw_lookup: Vec::new(),
            m_volume_lookup: Vec::new(),
            m_irq_cb: None,
            m_sample_rate_cb: None,
        };
        this.compute_tables();
        this.reset();
        this
    }

    fn update_sample_rate(&mut self) {
        let voices = self.m_active_voices as u32 + 1;
        self.m_sample_rate = self.m_master_clock.wrapping_add(8 * voices) / (16 * voices);
        let rate = self.m_sample_rate;
        if let Some(cb) = &mut self.m_sample_rate_cb {
            cb(rate);
        }
    }

    pub fn reset(&mut self) {
        self.m_active_voices = 31;
        self.m_current_page = 0;
        self.m_irqv = 0x80;
        self.m_mode = 0x7f8;
        self.m_voice_index = 0;

        self.update_sample_rate();

        for j in 0..MAX_VOICES_USIZE {
            let v = &mut self.m_voices[j];
            v.index = j as u8;
            v.control = CONTROL_STOPMASK;
            v.freqcount = 0;
            v.start = 0;
            v.end = 0;
            v.accum = 0;
            v.lvol = 0x80;
            v.rvol = 0x80;
            v.k1 = 0;
            v.k2 = 0;
            v.o1n1 = 0;
            v.o2n1 = 0;
            v.o2n2 = 0;
            v.o3n1 = 0;
            v.o3n2 = 0;
            v.o4n1 = 0;
            v.filtcount = 0;
            self.m_voice_bank[j] = 0;
        }
    }

    /// # Safety
    /// `rom_words` must point to `word_count` readable `u16`s that stay valid and unmoved for as
    /// long as this chip is used with them (the C++ stores the same non-owning pointer).
    pub unsafe fn set_sample_rom(&mut self, rom_words: *const u16, word_count: usize) {
        self.m_rom = rom_words;
        self.m_rom_words = word_count;
    }

    pub fn set_voice_bank(&mut self, voice: i32, bank_words: u32) {
        if voice >= 0 && (voice as usize) < MAX_VOICES_USIZE {
            self.m_voice_bank[voice as usize] = bank_words;
        }
    }

    pub fn set_irq_callback(&mut self, cb: Box<dyn FnMut(bool)>) {
        self.m_irq_cb = Some(cb);
    }

    pub fn set_sample_rate_callback(&mut self, cb: Box<dyn FnMut(u32)>) {
        self.m_sample_rate_cb = Some(cb);
    }

    pub fn sample_rate(&self) -> u32 {
        self.m_sample_rate
    }

    pub fn active_voices(&self) -> i32 {
        self.m_active_voices as i32
    }

    pub fn irqv(&self) -> u8 {
        self.m_irqv
    }

    pub fn get_voice_index(&self) -> i32 {
        self.m_voice_index
    }

    fn compute_tables(&mut self) {
        self.m_ulaw_lookup.resize(1 << ULAW_MAXBITS, 0);
        for i in 0..(1i32 << ULAW_MAXBITS) {
            let rawval: u16 = ((i << (16 - ULAW_MAXBITS)) | (1 << (15 - ULAW_MAXBITS))) as u16;
            let exponent: u8 = (rawval >> 13) as u8;
            let mut mantissa: u32 = ((rawval as u32) << 3) & 0xffff;
            if exponent == 0 {
                self.m_ulaw_lookup[i as usize] = (mantissa as u16 as i16) >> 7;
            } else {
                mantissa = (mantissa >> 1) | (!mantissa & 0x8000);
                self.m_ulaw_lookup[i as usize] = (mantissa as u16 as i16) >> (7 - exponent as u32);
            }
        }

        const VOLUME_LEN: usize = 256;
        self.m_volume_lookup.resize(VOLUME_LEN, 0);
        for i in 0..256u32 {
            let exponent: u32 = (i >> 4) & 15;
            let mantissa: u32 = (i & 15) | 16;
            self.m_volume_lookup[i as usize] = (mantissa << 11) >> (16 - exponent);
        }
    }

    fn read_sample(&mut self, voice: usize, word_addr: u32) -> u16 {
        let index = self.m_voices[voice].index as usize;
        self.m_voice_index = index as i32;
        if self.m_rom.is_null() || self.m_rom_words == 0 {
            return 0;
        }
        let full_addr = (self.m_voice_bank[index].wrapping_add(word_addr) as usize) & (self.m_rom_words - 1);
        unsafe { *self.m_rom.add(full_addr) }
    }

    fn interpolate(&self, s1: i32, s2: i32, mut accum: u64) -> i32 {
        const SHIFTED: u32 = 1 << ADDRESS_FRAC_BIT;
        const MASK: u32 = SHIFTED - 1;
        accum &= MASK as u64 & ADDRESS_ACC_MASK;
        s1.wrapping_mul((SHIFTED as u64).wrapping_sub(accum) as i32)
            .wrapping_add(s2.wrapping_mul(accum as i32))
            >> ADDRESS_FRAC_BIT
    }

    fn apply_filters(&mut self, voice: usize, sample: &mut i32) {
        let v = &mut self.m_voices[voice];
        *sample = apply_lowpass(*sample, v.k1 as i32, v.o1n1);
        v.o1n1 = *sample;

        *sample = apply_lowpass(*sample, v.k1 as i32, v.o2n1);
        v.o2n2 = v.o2n1;
        v.o2n1 = *sample;

        let lp = (v.control >> 10) & 3;
        match lp {
            0 => {
                *sample = apply_highpass(*sample, v.k2 as i32, v.o3n1, v.o2n2);
                v.o3n2 = v.o3n1;
                v.o3n1 = *sample;
                *sample = apply_highpass(*sample, v.k2 as i32, v.o4n1, v.o3n2);
                v.o4n1 = *sample;
            }
            1 => {
                *sample = apply_lowpass(*sample, v.k1 as i32, v.o3n1);
                v.o3n2 = v.o3n1;
                v.o3n1 = *sample;
                *sample = apply_highpass(*sample, v.k2 as i32, v.o4n1, v.o3n2);
                v.o4n1 = *sample;
            }
            2 => {
                *sample = apply_lowpass(*sample, v.k2 as i32, v.o3n1);
                v.o3n2 = v.o3n1;
                v.o3n1 = *sample;
                *sample = apply_lowpass(*sample, v.k2 as i32, v.o4n1);
                v.o4n1 = *sample;
            }
            _ => {
                *sample = apply_lowpass(*sample, v.k1 as i32, v.o3n1);
                v.o3n2 = v.o3n1;
                v.o3n1 = *sample;
                *sample = apply_lowpass(*sample, v.k2 as i32, v.o4n1);
                v.o4n1 = *sample;
            }
        }
    }

    fn check_for_end_forward(&mut self, voice: usize, accum: &mut u64) {
        let voice = &mut self.m_voices[voice];
        if *accum > voice.end {
            if voice.control & CONTROL_IRQE != 0 {
                voice.control |= CONTROL_IRQ;
            }
            match voice.control & CONTROL_LOOPMASK {
                0 | CONTROL_BLE => voice.control |= CONTROL_STOP0,
                CONTROL_LPE => {
                    *accum = voice.start.wrapping_add(accum.wrapping_sub(voice.end)) & ADDRESS_ACC_MASK;
                }
                _ => {
                    *accum = voice.end.wrapping_sub(accum.wrapping_sub(voice.end)) & ADDRESS_ACC_MASK;
                    voice.control ^= CONTROL_DIR;
                }
            }
        }
    }

    fn check_for_end_reverse(&mut self, voice: usize, accum: &mut u64) {
        let voice = &mut self.m_voices[voice];
        if *accum < voice.start {
            if voice.control & CONTROL_IRQE != 0 {
                voice.control |= CONTROL_IRQ;
            }
            match voice.control & CONTROL_LOOPMASK {
                0 | CONTROL_BLE => voice.control |= CONTROL_STOP0,
                CONTROL_LPE => {
                    *accum = voice.end.wrapping_sub(voice.start.wrapping_sub(*accum)) & ADDRESS_ACC_MASK;
                }
                _ => {
                    *accum = voice.start.wrapping_add(voice.start.wrapping_sub(*accum)) & ADDRESS_ACC_MASK;
                    voice.control ^= CONTROL_DIR;
                }
            }
        }
    }

    fn generate_pcm(&mut self, voice: usize, dest: &mut [i32]) {
        let freqcount = self.m_voices[voice].freqcount as u32;
        let mut accum = self.m_voices[voice].accum & ADDRESS_ACC_MASK;

        if self.m_voices[voice].control & CONTROL_STOPMASK == 0 {
            let reverse = self.m_voices[voice].control & CONTROL_DIR != 0;
            let word_addr = (accum >> ADDRESS_FRAC_BIT) as u32;
            let next_word_addr = word_addr.wrapping_add(1) & ((ADDRESS_ACC_MASK >> ADDRESS_FRAC_BIT) as u32);
            let mut val1 = self.read_sample(voice, word_addr) as i16 as i32;
            let val2 = self.read_sample(voice, next_word_addr) as i16 as i32;

            val1 = self.interpolate(val1, val2, accum);
            accum = (if reverse {
                accum.wrapping_sub(freqcount as u64)
            } else {
                accum.wrapping_add(freqcount as u64)
            }) & ADDRESS_ACC_MASK;

            self.apply_filters(voice, &mut val1);

            let lvol = self.m_voices[voice].lvol;
            let rvol = self.m_voices[voice].rvol;
            dest[0] = (dest[0] as i64)
                .wrapping_add((val1 as i64).wrapping_mul(self.m_volume_lookup[(lvol & 0xff) as usize] as i64) >> 11)
                as i32;
            dest[1] = (dest[1] as i64)
                .wrapping_add((val1 as i64).wrapping_mul(self.m_volume_lookup[(rvol & 0xff) as usize] as i64) >> 11)
                as i32;

            if reverse {
                self.check_for_end_reverse(voice, &mut accum);
            } else {
                self.check_for_end_forward(voice, &mut accum);
            }
        }

        self.m_voices[voice].accum = accum;
    }

    fn generate_irq(&mut self, voice: usize, v: i32) {
        if self.m_voices[voice].control & CONTROL_IRQ != 0 && self.m_irqv & 0x80 != 0 {
            self.m_irqv = (v & 0x1f) as u8;
            self.m_voices[voice].control &= !CONTROL_IRQ;
            if let Some(cb) = &mut self.m_irq_cb {
                cb(true);
            }
        }
    }

    fn update_internal_irq_state(&mut self) {
        self.m_irqv = 0x80;
        if let Some(cb) = &mut self.m_irq_cb {
            cb(false);
        }
    }

    pub fn generate_one_sample(&mut self, output8: &mut [i32; NUM_CHANNELS_USIZE]) {
        *output8 = [0; NUM_CHANNELS_USIZE];

        for v in 0..=(self.m_active_voices as i32) {
            let voice = v as usize;
            let voice_channel = ((self.m_voices[voice].control >> 8) & 3) as usize;
            let left_idx = voice_channel * 2;

            self.generate_pcm(voice, &mut output8[left_idx..]);
            self.generate_irq(voice, v);
        }
    }

    fn reg_read_low(&mut self, voice: usize, offset: u32) -> u16 {
        let v = &self.m_voices[voice];
        match offset {
            0x00 => (v.control | 0xf000) as u16,
            0x01 => get_address_acc_res(v.freqcount, 1) as u16,
            0x02 => (get_address_acc_res(v.start, 0) >> 16) as u16,
            0x03 => get_address_acc_res(v.start, 0) as u16,
            0x04 => (get_address_acc_res(v.end, 0) >> 16) as u16,
            0x05 => get_address_acc_res(v.end, 0) as u16,
            0x06 => v.k2 as u16,
            0x07 => v.k1 as u16,
            0x08 => (v.lvol << 8) as u16,
            0x09 => (v.rvol << 8) as u16,
            0x0a => (get_address_acc_res(v.accum, 0) >> 16) as u16,
            0x0b => get_address_acc_res(v.accum, 0) as u16,
            0x0d => self.m_active_voices as u16,
            0x0e => {
                let res = self.m_irqv as u16;
                self.update_internal_irq_state();
                res
            }
            0x0f => self.m_current_page as u16,
            _ => 0,
        }
    }

    fn reg_read_high(&mut self, voice: usize, offset: u32) -> u16 {
        let v = &self.m_voices[voice];
        match offset {
            0x00 => (v.control | 0xf000) as u16,
            0x01 => (v.o4n1 & 0xffff) as u16,
            0x02 => (v.o3n1 & 0xffff) as u16,
            0x03 => (v.o3n2 & 0xffff) as u16,
            0x04 => (v.o2n1 & 0xffff) as u16,
            0x05 => (v.o2n2 & 0xffff) as u16,
            0x06 => {
                if v.control & CONTROL_STOPMASK != 0 {
                    let accum = v.accum;
                    let sample = self.read_sample(voice, get_integer_addr(accum) as u32);
                    self.m_voices[voice].o1n1 = sample as i32;
                }
                (self.m_voices[voice].o1n1 & 0xffff) as u16
            }
            0x0d => self.m_active_voices as u16,
            0x0e => {
                let res = self.m_irqv as u16;
                self.update_internal_irq_state();
                res
            }
            0x0f => self.m_current_page as u16,
            _ => 0,
        }
    }

    fn reg_read_test(&mut self, _voice: usize, offset: u32) -> u16 {
        match offset {
            0x08 => self.m_mode | 0x7f8,
            0x0d => self.m_active_voices as u16,
            0x0e => {
                let res = self.m_irqv as u16;
                self.update_internal_irq_state();
                res
            }
            0x0f => self.m_current_page as u16,
            _ => 0,
        }
    }

    fn write_control(voice: &mut Voice, data: u16, mem_mask: u16) {
        voice.control |= 0xf000;
        if mem_mask & 0x00ff != 0 {
            voice.control = (voice.control & !0x00ff) | (data & 0x00ff) as u32;
        }
        if mem_mask & 0xff00 != 0 {
            voice.control = (voice.control & !0x0f00) | (data & 0x0f00) as u32;
        }
    }

    fn set_active_voices(&mut self, data: u16, mem_mask: u16) {
        if mem_mask & 0x00ff != 0 {
            self.m_active_voices = (data & 0x1f) as u8;
            self.update_sample_rate();
        }
    }

    fn reg_write_low(&mut self, voice: usize, offset: u32, data: u16, mem_mask: u16) {
        match offset {
            0x00 => Self::write_control(&mut self.m_voices[voice], data, mem_mask),
            0x01 => {
                let v = &mut self.m_voices[voice];
                if mem_mask & 0x00ff != 0 {
                    v.freqcount = (v.freqcount & !get_address_acc_shifted_val(0x00fe, 1))
                        | get_address_acc_shifted_val((data & 0x00fe) as u64, 1);
                }
                if mem_mask & 0xff00 != 0 {
                    v.freqcount = (v.freqcount & !get_address_acc_shifted_val(0xff00, 1))
                        | get_address_acc_shifted_val((data & 0xff00) as u64, 1);
                }
            }
            0x02 => {
                let v = &mut self.m_voices[voice];
                Self::write_hi(&mut v.start, data, mem_mask);
            }
            0x03 => {
                let v = &mut self.m_voices[voice];
                Self::write_lo(&mut v.start, data, mem_mask);
            }
            0x04 => {
                let v = &mut self.m_voices[voice];
                Self::write_hi(&mut v.end, data, mem_mask);
            }
            0x05 => {
                let v = &mut self.m_voices[voice];
                Self::write_lo(&mut v.end, data, mem_mask);
            }
            0x06 => {
                let v = &mut self.m_voices[voice];
                if mem_mask & 0x00ff != 0 {
                    v.k2 = (v.k2 & !0x00f0) | (data & 0x00f0) as u32;
                }
                if mem_mask & 0xff00 != 0 {
                    v.k2 = (v.k2 & !0xff00) | (data & 0xff00) as u32;
                }
            }
            0x07 => {
                let v = &mut self.m_voices[voice];
                if mem_mask & 0x00ff != 0 {
                    v.k1 = (v.k1 & !0x00f0) | (data & 0x00f0) as u32;
                }
                if mem_mask & 0xff00 != 0 {
                    v.k1 = (v.k1 & !0xff00) | (data & 0xff00) as u32;
                }
            }
            0x08 => {
                if mem_mask & 0xff00 != 0 {
                    self.m_voices[voice].lvol = ((data >> 8) & 0xff) as u32;
                }
            }
            0x09 => {
                if mem_mask & 0xff00 != 0 {
                    self.m_voices[voice].rvol = ((data >> 8) & 0xff) as u32;
                }
            }
            0x0a => {
                let v = &mut self.m_voices[voice];
                Self::write_hi(&mut v.accum, data, mem_mask);
            }
            0x0b => {
                let v = &mut self.m_voices[voice];
                if mem_mask & 0x00ff != 0 {
                    v.accum = (v.accum & !get_address_acc_shifted_val(0x000000ff, 0))
                        | get_address_acc_shifted_val((data & 0x00ff) as u64, 0);
                }
                if mem_mask & 0xff00 != 0 {
                    v.accum = (v.accum & !get_address_acc_shifted_val(0x0000ff00, 0))
                        | get_address_acc_shifted_val((data & 0xff00) as u64, 0);
                }
            }
            0x0d => self.set_active_voices(data, mem_mask),
            0x0f => {
                if mem_mask & 0x00ff != 0 {
                    self.m_current_page = (data & 0x7f) as u8;
                }
            }
            _ => {}
        }
    }

    fn write_hi(field: &mut u64, data: u16, mem_mask: u16) {
        if mem_mask & 0x00ff != 0 {
            *field = (*field & !get_address_acc_shifted_val(0x00ff0000, 0))
                | get_address_acc_shifted_val(((data & 0x00ff) as u64) << 16, 0);
        }
        if mem_mask & 0xff00 != 0 {
            *field = (*field & !get_address_acc_shifted_val(0x1f000000, 0))
                | get_address_acc_shifted_val(((data & 0x1f00) as u64) << 16, 0);
        }
    }

    fn write_lo(field: &mut u64, data: u16, mem_mask: u16) {
        if mem_mask & 0x00ff != 0 {
            *field = (*field & !get_address_acc_shifted_val(0x000000e0, 0))
                | get_address_acc_shifted_val((data & 0x00e0) as u64, 0);
        }
        if mem_mask & 0xff00 != 0 {
            *field = (*field & !get_address_acc_shifted_val(0x0000ff00, 0))
                | get_address_acc_shifted_val((data & 0xff00) as u64, 0);
        }
    }

    fn write_pole(pole: &mut i32, data: u16, mem_mask: u16) {
        if mem_mask & 0x00ff != 0 {
            *pole = (*pole & !0x00ff) | (data & 0x00ff) as i32;
        }
        if mem_mask & 0xff00 != 0 {
            *pole = ((*pole & !0xff00) | (data & 0xff00) as i32) as i16 as i32;
        }
    }

    fn reg_write_high(&mut self, voice: usize, offset: u32, data: u16, mem_mask: u16) {
        match offset {
            0x00 => Self::write_control(&mut self.m_voices[voice], data, mem_mask),
            0x01 => Self::write_pole(&mut self.m_voices[voice].o4n1, data, mem_mask),
            0x02 => Self::write_pole(&mut self.m_voices[voice].o3n1, data, mem_mask),
            0x03 => Self::write_pole(&mut self.m_voices[voice].o3n2, data, mem_mask),
            0x04 => Self::write_pole(&mut self.m_voices[voice].o2n1, data, mem_mask),
            0x05 => Self::write_pole(&mut self.m_voices[voice].o2n2, data, mem_mask),
            0x06 => Self::write_pole(&mut self.m_voices[voice].o1n1, data, mem_mask),
            0x0d => self.set_active_voices(data, mem_mask),
            0x0f => {
                if mem_mask & 0x00ff != 0 {
                    self.m_current_page = (data & 0x7f) as u8;
                }
            }
            _ => {}
        }
    }

    fn reg_write_test(&mut self, _voice: usize, offset: u32, data: u16, mem_mask: u16) {
        match offset {
            0x08 => {
                self.m_mode |= 0x7f8;
                if mem_mask & 0xff00 != 0 {
                    self.m_mode = ((self.m_mode as i32 & !0xf800) | (data & 0xf800) as i32) as u16;
                }
                if mem_mask & 0x00ff != 0 {
                    self.m_mode = ((self.m_mode as i32 & !0x0007) | (data & 0x0007) as i32) as u16;
                }
            }
            0x0d => self.set_active_voices(data, mem_mask),
            0x0f => {
                if mem_mask & 0x00ff != 0 {
                    self.m_current_page = (data & 0x7f) as u8;
                }
            }
            _ => {}
        }
    }

    pub fn read(&mut self, offset: u32) -> u16 {
        let voice = (self.m_current_page & 0x1f) as usize;
        let offset = offset & 0x0f;
        if self.m_current_page < 0x20 {
            self.reg_read_low(voice, offset)
        } else if self.m_current_page < 0x40 {
            self.reg_read_high(voice, offset)
        } else {
            self.reg_read_test(voice, offset)
        }
    }

    pub fn write(&mut self, offset: u32, data: u16, mem_mask: u16) {
        let voice = (self.m_current_page & 0x1f) as usize;
        let offset = offset & 0x0f;
        if self.m_current_page < 0x20 {
            self.reg_write_low(voice, offset, data, mem_mask);
        } else if self.m_current_page < 0x40 {
            self.reg_write_high(voice, offset, data, mem_mask);
        } else {
            self.reg_write_test(voice, offset, data, mem_mask);
        }
    }

    pub fn state_size(&self) -> usize {
        Self::STATE_SIZE
    }

    pub fn save_state(&self, writer: &mut StateWriter) {
        writer.write_u32(self.m_master_clock);
        writer.write_u32(self.m_sample_rate);
        writer.write_u8(self.m_active_voices);
        writer.write_u8(self.m_current_page);
        writer.write_u8(self.m_irqv);
        writer.write_u16(self.m_mode);
        writer.write_i32(self.m_voice_index);
        for i in 0..MAX_VOICES_USIZE {
            writer.write_u32(self.m_voice_bank[i]);
        }
        for v in &self.m_voices {
            writer.write_u32(v.control);
            writer.write_u64(v.freqcount);
            writer.write_u64(v.start);
            writer.write_u32(v.lvol);
            writer.write_u64(v.end);
            writer.write_u32(v.lvramp);
            writer.write_u64(v.accum);
            writer.write_u32(v.rvol);
            writer.write_u32(v.rvramp);
            writer.write_u32(v.ecount);
            writer.write_u32(v.k2);
            writer.write_u32(v.k2ramp);
            writer.write_u32(v.k1);
            writer.write_u32(v.k1ramp);
            writer.write_i32(v.o4n1);
            writer.write_i32(v.o3n1);
            writer.write_i32(v.o3n2);
            writer.write_i32(v.o2n1);
            writer.write_i32(v.o2n2);
            writer.write_i32(v.o1n1);
            writer.write_u8(v.index);
            writer.write_u8(v.filtcount);
        }
    }

    pub fn load_state(&mut self, reader: &mut StateReader) {
        let mut record = vec![0u8; Self::STATE_SIZE];
        reader.read_bytes(&mut record);
        let r = &mut StateReader::new(&record);

        let master_clock = r.read_u32();
        let sample_rate = r.read_u32();
        let active_voices = r.read_u8();
        let current_page = r.read_u8();
        let irqv = r.read_u8();
        let mode = r.read_u16();
        let voice_index = r.read_i32();
        let mut voice_bank = [0u32; MAX_VOICES_USIZE];
        for b in &mut voice_bank {
            *b = r.read_u32();
        }
        let mut voices = [Voice::default(); MAX_VOICES_USIZE];
        for v in &mut voices {
            v.control = r.read_u32();
            v.freqcount = r.read_u64();
            v.start = r.read_u64();
            v.lvol = r.read_u32();
            v.end = r.read_u64();
            v.lvramp = r.read_u32();
            v.accum = r.read_u64();
            v.rvol = r.read_u32();
            v.rvramp = r.read_u32();
            v.ecount = r.read_u32();
            v.k2 = r.read_u32();
            v.k2ramp = r.read_u32();
            v.k1 = r.read_u32();
            v.k1ramp = r.read_u32();
            v.o4n1 = r.read_i32();
            v.o3n1 = r.read_i32();
            v.o3n2 = r.read_i32();
            v.o2n1 = r.read_i32();
            v.o2n2 = r.read_i32();
            v.o1n1 = r.read_i32();
            v.index = r.read_u8();
            v.filtcount = r.read_u8();
        }

        require_state(
            active_voices < 32
                && current_page < 128
                && voice_index >= 0
                && voice_index < 32
                && master_clock != 0
                && sample_rate != 0
                && sample_rate == master_clock / (16 * (active_voices as u32 + 1)),
        );
        for v in &voices {
            require_state(v.index < 32);
        }
        require_state(master_clock == self.m_master_clock);

        self.m_master_clock = master_clock;
        self.m_sample_rate = sample_rate;
        self.m_active_voices = active_voices;
        self.m_current_page = current_page;
        self.m_irqv = irqv;
        self.m_mode = mode;
        self.m_voice_index = voice_index;
        self.m_voice_bank = voice_bank;
        self.m_voices = voices;
    }
}
