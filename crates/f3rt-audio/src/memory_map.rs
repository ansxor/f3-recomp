//! `f3rt/memory_map.hpp`: the bus-map constants the sound board uses.

// Main CPU (68EC020) bus map: the mailbox lives in shared DPRAM.

/// Shared dual-port RAM with the sound CPU (2 KiB).
pub const SHARED_BASE: u32 = 0xc00000;
pub const SHARED_END: u32 = 0xc00800;
pub const SHARED_RAM_SIZE: usize = 0x800;

// Sound CPU (68000) bus map.

pub const SOUND_RAM_BASE: u32 = 0x000000;
pub const SOUND_RAM_END: u32 = 0x040000;
pub const SOUND_RAM_MASK: u32 = 0xffff;

pub const SOUND_SHARED_BASE: u32 = 0x140000;
pub const SOUND_SHARED_END: u32 = 0x141000;

pub const SOUND_ES5505_BASE: u32 = 0x200000;
pub const SOUND_ES5505_END: u32 = 0x200020;

pub const SOUND_ES5510_BASE: u32 = 0x260000;
pub const SOUND_ES5510_END: u32 = 0x260200;

pub const SOUND_DUART_BASE: u32 = 0x280000;
pub const SOUND_DUART_END: u32 = 0x280020;

pub const SOUND_BANK_TABLE_BASE: u32 = 0x300000;
pub const SOUND_BANK_TABLE_END: u32 = 0x300040;

pub const SOUND_VOLUME_BASE: u32 = 0x340000;
pub const SOUND_VOLUME_END: u32 = 0x340004;

pub const SOUND_ROM_BASE: u32 = 0xc00000;
pub const SOUND_ROM_END: u32 = 0xc80000;
pub const SOUND_ROM_SIZE: usize = 0x80000;

pub const SOUND_RAM_HIGH_BASE: u32 = 0xff0000;
pub const SOUND_RAM_HIGH_END: u32 = 0x1000000;

// Shared DPRAM mailbox protocol offsets (within the 2 KiB shared RAM).

pub const MAILBOX_RING_BASE: u32 = 0x000;
pub const MAILBOX_RING_SIZE: usize = 1024;
pub const MAILBOX_RING_MASK: u32 = 0x3ff;

pub const MAILBOX_PRODUCER_H: u32 = 0x480;
pub const MAILBOX_PRODUCER_L: u32 = 0x481;

pub const MAILBOX_CONSUMER_H: u32 = 0x482;
pub const MAILBOX_CONSUMER_L: u32 = 0x483;

pub const MAILBOX_GAIN_CH0: u32 = 0x7fa;
pub const MAILBOX_GAIN_CH1: u32 = 0x7fb;
