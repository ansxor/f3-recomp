//! `audio/hle/track_parameters.hpp`: the 0x38-byte track parameter record.
//!
//! Controllers occupy the even bytes from 0x0a; the following odd byte is the low half of that
//! big-endian word. `byte()` and `bytes` address the rest of the record.

#![allow(non_upper_case_globals)]

/// `TrackParameters::At`: the byte offsets inside the record.
pub mod at {
    pub const SIZE: usize = 0x38;
    pub const program_high: usize = 0x04;
    pub const program_low: usize = 0x05;
    pub const controllers: usize = 0x0a;
    pub const bend: usize = 0x0a;
    pub const mod_0c: usize = 0x0c;
    pub const mod_10: usize = 0x10;
    pub const mod_12: usize = 0x12;
    pub const expression: usize = 0x14;
    pub const mod_1a: usize = 0x1a;
    pub const layer_mask: usize = 0x1c;
    pub const volume: usize = 0x20;
    pub const previous_key: usize = 0x22;
    pub const pan: usize = 0x26;
    pub const program_record: usize = 0x2a;
    pub const volume_record: usize = 0x2c;
    pub const pan_record: usize = 0x2d;
    pub const mod_1a_record: usize = 0x2e;
    pub const layer_mask_record: usize = 0x2f;
    pub const selector_record: usize = 0x30;
    pub const pair_select: usize = 0x31;
    pub const flags: usize = 0x32;
    pub const selector: usize = 0x33;
    pub const transpose: usize = 0x35;
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct TrackParameters {
    pub bytes: [u8; at::SIZE],
}

impl Default for TrackParameters {
    fn default() -> Self {
        TrackParameters { bytes: [0; at::SIZE] }
    }
}

impl TrackParameters {
    pub const size: usize = at::SIZE;
    pub const controller_center: u8 = 0x40;
    pub const extended_selector: u8 = 0x40;
    pub const bend_center: u32 = 0x4000;

    pub fn byte(&self, offset: usize) -> u8 {
        self.bytes[offset]
    }

    pub fn set_byte(&mut self, offset: usize, value: u8) {
        self.bytes[offset] = value;
    }

    pub fn word(&self, offset: usize) -> u16 {
        ((self.bytes[offset] as u16) << 8) | self.bytes[offset + 1] as u16
    }

    /// Controller k writes the even byte at 0x0a + 2k. The layer-mask byte is stored centered
    /// (value - 0x40); every other controller stores the value. Returns false when the byte would
    /// fall outside the record.
    pub fn set_controller(&mut self, index: usize, value: u8) -> bool {
        let offset = at::controllers + index * 2;
        if offset >= at::SIZE {
            return false;
        }
        self.bytes[offset] = if offset == at::layer_mask {
            value.wrapping_sub(Self::controller_center)
        } else {
            value
        };
        true
    }

    pub fn set_program_image(&mut self, program: u16) {
        self.bytes[at::program_high] = (program >> 8) as u8;
        self.bytes[at::program_low] = program as u8;
    }

    /// Header track-record fields copied into the live controller bytes.
    pub fn copy_record_to_live(&mut self) {
        self.bytes[at::volume] = self.bytes[at::volume_record];
        self.bytes[at::pan] = self.bytes[at::pan_record];
        self.bytes[at::mod_1a] = self.bytes[at::mod_1a_record];
        self.bytes[at::layer_mask] = self.bytes[at::layer_mask_record];
        self.bytes[at::selector] = self.bytes[at::selector_record];
    }

    pub fn bend(&self) -> u16 {
        self.word(at::bend)
    }

    pub fn mod_0c(&self) -> u16 {
        self.word(at::mod_0c)
    }

    pub fn mod_10(&self) -> u16 {
        self.word(at::mod_10)
    }

    pub fn mod_12(&self) -> u16 {
        self.word(at::mod_12)
    }

    pub fn mod_1a(&self) -> u16 {
        self.word(at::mod_1a)
    }

    pub fn expression(&self) -> u8 {
        self.bytes[at::expression]
    }

    /// Added into the attenuation sum. Zero when the expression byte is zero.
    pub fn expression_offset(&self) -> u8 {
        let value = self.expression();
        if value != 0 {
            value.wrapping_add(127)
        } else {
            0
        }
    }

    /// Non-zero replaces the program layer-enable mask. A controller write to this byte subtracts
    /// 0x40 (firmware PAN_CONTROLLER_OFFSET).
    pub fn layer_mask(&self) -> u8 {
        self.bytes[at::layer_mask]
    }

    pub fn volume(&self) -> u8 {
        self.bytes[at::volume]
    }

    pub fn volume_index(&self) -> u8 {
        self.volume() & 127
    }

    pub fn set_volume(&mut self, value: u8) {
        self.bytes[at::volume] = value;
        self.bytes[at::volume_record] = value;
    }

    /// Previous factory key. A fresh legato voice glides from this byte.
    pub fn previous_key(&self) -> u8 {
        self.bytes[at::previous_key]
    }

    pub fn set_previous_key(&mut self, key: u8) {
        self.bytes[at::previous_key] = key;
    }

    pub fn pan(&self) -> u8 {
        self.bytes[at::pan]
    }

    pub fn signed_pan(&self) -> i8 {
        self.bytes[at::pan] as i8
    }

    pub fn set_pan(&mut self, value: u8) {
        self.bytes[at::pan] = value;
        self.bytes[at::pan_record] = value;
    }

    pub fn pair_select(&self) -> u8 {
        self.bytes[at::pair_select]
    }

    pub fn flags(&self) -> u8 {
        self.bytes[at::flags]
    }

    /// Bits 4 and 5 both set: the note-on is dropped.
    pub fn notes_suppressed(&self) -> bool {
        (self.flags() & 0x30) == 0x30
    }

    pub fn set_extended_selector(&mut self, on: bool) {
        if on {
            self.bytes[at::flags] |= Self::extended_selector;
        } else {
            self.bytes[at::flags] &= !Self::extended_selector;
        }
    }

    pub fn selector(&self) -> u8 {
        self.bytes[at::selector]
    }

    pub fn set_selector(&mut self, value: u8) {
        self.bytes[at::selector] = value;
    }

    pub fn transpose(&self) -> u8 {
        self.bytes[at::transpose]
    }

    pub fn signed_transpose(&self) -> i8 {
        self.bytes[at::transpose] as i8
    }

    pub fn transposed_key(&self, kernel_key: u8) -> u8 {
        kernel_key.wrapping_add(self.bytes[at::transpose])
    }
}
