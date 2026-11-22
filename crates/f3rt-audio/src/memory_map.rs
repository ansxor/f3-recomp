//! Bus-map constants the sound board uses.

pub const SOUND_ROM_BASE: u32 = 0xc00000;
pub const SOUND_ROM_SIZE: usize = 0x80000;

// Mailbox protocol offsets within the 2 KiB shared DPRAM.

pub const MAILBOX_RING_MASK: u32 = 0x3ff;

pub const MAILBOX_PRODUCER_H: u32 = 0x480;
pub const MAILBOX_PRODUCER_L: u32 = 0x481;

pub const MAILBOX_CONSUMER_H: u32 = 0x482;

pub const MAILBOX_GAIN_CH0: u32 = 0x7fa;
pub const MAILBOX_GAIN_CH1: u32 = 0x7fb;
