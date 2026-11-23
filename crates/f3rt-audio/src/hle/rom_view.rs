//! The banked sound-program ROM window.

use std::sync::Arc;
use std::sync::atomic::{AtomicU8, Ordering};

use crate::memory_map::{SOUND_ROM_BASE, SOUND_ROM_SIZE};

pub const BANKED_WINDOW_BASE: u32 = 0xc20000;
pub const BANKED_WINDOW_END: u32 = 0xc40000;
pub const BANKED_OVERFLOW_BASE: u32 = 0xc40000;
pub const BANK_PAGE_SIZE: u32 = 0x20000;
pub const BANK_OVERFLOW_OFFSET: u32 = 0x40000;
pub const SOUND_ROM_IMAGE_SIZE: u32 = 0x80000;

const OUTSIDE_IMAGE: &str = "HLE ROM read outside image";

/// The image is shared with every reader of the sound program (bank, sequencer, effects).
pub struct RomView {
    rom: Arc<[u8]>,
    bank: AtomicU8,
}

impl Default for RomView {
    fn default() -> Self {
        Self::new()
    }
}

impl RomView {
    pub fn new() -> RomView {
        RomView {
            rom: Arc::from(&[][..]),
            bank: AtomicU8::new(1),
        }
    }

    pub fn with_image(image: Arc<[u8]>, bank: u8) -> RomView {
        RomView {
            rom: image,
            bank: AtomicU8::new(bank),
        }
    }

    pub fn set_bank(&self, bank: u8) {
        self.bank.store(bank, Ordering::Relaxed);
    }

    pub fn image(&self) -> &[u8] {
        &self.rom
    }

    pub fn size(&self) -> usize {
        self.rom.len()
    }

    pub fn resolve_offset(&self, address: u32) -> u32 {
        self.try_resolve_offset(address).unwrap_or_else(|reason| panic!("{reason}"))
    }

    /// The image offset `address` maps to under the current bank, or why it maps nowhere.
    pub fn try_resolve_offset(&self, address: u32) -> Result<u32, &'static str> {
        if !(SOUND_ROM_BASE..SOUND_ROM_BASE + SOUND_ROM_IMAGE_SIZE).contains(&address) {
            return Err(OUTSIDE_IMAGE);
        }
        let mut off = address - SOUND_ROM_BASE;
        if self.rom.len() > SOUND_ROM_SIZE {
            if (BANKED_WINDOW_BASE..BANKED_WINDOW_END).contains(&address) {
                off = u32::from(self.bank.load(Ordering::Relaxed)) * BANK_PAGE_SIZE
                    + (address - BANKED_WINDOW_BASE);
            } else if address >= BANKED_OVERFLOW_BASE {
                off = BANK_OVERFLOW_OFFSET + (address - BANKED_OVERFLOW_BASE);
            }
        }
        if off as usize >= self.rom.len() {
            return Err(OUTSIDE_IMAGE);
        }
        Ok(off)
    }

    pub fn read_byte(&self, address: u32) -> u8 {
        self.try_read_byte(address).unwrap_or_else(|reason| panic!("{reason}"))
    }

    pub fn read_u16(&self, address: u32) -> u16 {
        self.try_read_u16(address).unwrap_or_else(|reason| panic!("{reason}"))
    }

    pub fn read_u32(&self, address: u32) -> u32 {
        self.try_read_u32(address).unwrap_or_else(|reason| panic!("{reason}"))
    }

    pub fn try_read_byte(&self, address: u32) -> Result<u8, &'static str> {
        Ok(self.rom[self.try_resolve_offset(address)? as usize])
    }

    pub fn try_read_u16(&self, address: u32) -> Result<u16, &'static str> {
        let off = self.try_resolve_offset(address)? as usize;
        let bytes = self.rom.get(off..off + 2).ok_or(OUTSIDE_IMAGE)?;
        Ok(u16::from_be_bytes([bytes[0], bytes[1]]))
    }

    pub fn try_read_u32(&self, address: u32) -> Result<u32, &'static str> {
        let off = self.try_resolve_offset(address)? as usize;
        let bytes = self.rom.get(off..off + 4).ok_or(OUTSIDE_IMAGE)?;
        Ok(u32::from_be_bytes([bytes[0], bytes[1], bytes[2], bytes[3]]))
    }

    /// The image from `address` to its end. Descriptors read from it are at least `min_bytes` long.
    pub fn tail_from(&self, address: u32, min_bytes: usize) -> &[u8] {
        let offset = self.resolve_offset(address) as usize;
        let tail = &self.rom[offset..];
        if tail.len() < min_bytes {
            panic!("HLE descriptor exceeds sound ROM");
        }
        tail
    }

    /// `operator[](size_t offset)`: a direct image read that yields 0xff outside the image.
    pub fn get(&self, offset: usize) -> u8 {
        if offset < self.rom.len() {
            self.rom[offset]
        } else {
            0xff
        }
    }
}
