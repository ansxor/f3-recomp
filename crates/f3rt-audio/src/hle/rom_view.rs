//! `audio/hle/rom_view.hpp`: the banked sound-program ROM window.

use crate::memory_map::{SOUND_ROM_BASE, SOUND_ROM_SIZE};

pub const BANKED_WINDOW_BASE: u32 = 0xc20000;
pub const BANKED_WINDOW_END: u32 = 0xc40000;
pub const BANKED_OVERFLOW_BASE: u32 = 0xc40000;
pub const BANK_PAGE_SIZE: u32 = 0x20000;
pub const BANK_OVERFLOW_OFFSET: u32 = 0x40000;
pub const SOUND_ROM_IMAGE_SIZE: u32 = 0x80000;

/// The image is borrowed, not owned: it must outlive the view (the Audio implementation owns it).
pub struct RomView {
    rom: &'static [u8],
    bank: u8,
}

impl Default for RomView {
    fn default() -> Self {
        Self::new()
    }
}

impl RomView {
    pub fn new() -> RomView {
        RomView { rom: &[], bank: 1 }
    }

    pub fn with_image(image: &'static [u8], bank: u8) -> RomView {
        RomView { rom: image, bank }
    }

    pub fn set_bank(&mut self, bank: u8) {
        self.bank = bank;
    }

    pub fn data(&self) -> *const u8 {
        self.rom.as_ptr()
    }

    pub fn size(&self) -> usize {
        self.rom.len()
    }

    pub fn resolve_offset(&self, address: u32) -> u32 {
        if !(SOUND_ROM_BASE..SOUND_ROM_BASE + SOUND_ROM_IMAGE_SIZE).contains(&address) {
            panic!("HLE ROM read outside image");
        }
        let mut off = address - SOUND_ROM_BASE;
        if self.rom.len() > SOUND_ROM_SIZE {
            if (BANKED_WINDOW_BASE..BANKED_WINDOW_END).contains(&address) {
                off = self.bank as u32 * BANK_PAGE_SIZE + (address - BANKED_WINDOW_BASE);
            } else if address >= BANKED_OVERFLOW_BASE {
                off = BANK_OVERFLOW_OFFSET + (address - BANKED_OVERFLOW_BASE);
            }
        }
        if off as usize >= self.rom.len() {
            panic!("HLE ROM read outside image");
        }
        off
    }

    pub fn read_byte(&self, address: u32) -> u8 {
        self.rom[self.resolve_offset(address) as usize]
    }

    pub fn read_u16(&self, address: u32) -> u16 {
        let off = self.resolve_offset(address) as usize;
        if off + 1 >= self.rom.len() {
            panic!("HLE ROM read outside image");
        }
        u16::from_be_bytes([self.rom[off], self.rom[off + 1]])
    }

    pub fn read_u32(&self, address: u32) -> u32 {
        let off = self.resolve_offset(address) as usize;
        if off + 3 >= self.rom.len() {
            panic!("HLE ROM read outside image");
        }
        u32::from_be_bytes([self.rom[off], self.rom[off + 1], self.rom[off + 2], self.rom[off + 3]])
    }

    pub fn pointer(&self, address: u32) -> *const u8 {
        self.pointer_bytes(address, 1)
    }

    pub fn pointer_bytes(&self, address: u32, bytes: usize) -> *const u8 {
        let off = self.resolve_offset(address) as usize;
        if off + bytes > self.rom.len() {
            panic!("HLE descriptor exceeds sound ROM");
        }
        unsafe { self.rom.as_ptr().add(off) }
    }

    /// `operator[](size_t offset)`: a direct image read that yields 0xff outside the image.
    pub fn get(&self, offset: usize) -> u8 {
        if offset < self.rom.len() { self.rom[offset] } else { 0xff }
    }
}
