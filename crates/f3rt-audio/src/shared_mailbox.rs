//! `audio/shared_mailbox.hpp`: the main-CPU mailbox ring in shared RAM.
//!
//! `shared` is the 0x800-byte dual-port RAM owned by the main CPU bus. C++ writes it between calls,
//! so it is touched through the pointer at each use and never held as a slice.

use crate::bytes::{read_be16, write_be16};
use crate::hle::command::CommandPacket;
use crate::memory_map::{
    MAILBOX_CONSUMER_H, MAILBOX_GAIN_CH0, MAILBOX_PRODUCER_H, MAILBOX_RING_MASK,
};

pub const INDEX_MASK: u16 = 0x7fe;

pub struct SharedMailbox {
    shared: *mut u8,
    consumer: u16,
}

impl SharedMailbox {
    pub fn valid_index(index: u16) -> bool {
        (index & !INDEX_MASK) == 0
    }

    /// Panics when `shared` is null.
    pub fn new(shared: *mut u8) -> SharedMailbox {
        if shared.is_null() {
            panic!("HLE requires shared RAM");
        }
        SharedMailbox { shared, consumer: 0 }
    }

    pub fn drain<F: FnMut(CommandPacket)>(&mut self, mut on_packet: F) {
        let producer = unsafe { read_be16(self.shared.add(MAILBOX_PRODUCER_H as usize)) } & INDEX_MASK;
        while self.consumer != producer {
            let index = self.consumer >> 1;
            let length = unsafe { *self.shared.add(index as usize) };
            let available = (producer.wrapping_sub(self.consumer) & 0x7ff) >> 1;
            if length < 2 || u16::from(length) > available {
                break;
            }
            if usize::from(length) <= CommandPacket::MAX_BYTES {
                let mut bytes = [0u8; CommandPacket::MAX_BYTES];
                for (n, byte) in bytes.iter_mut().enumerate().take(length as usize) {
                    *byte = unsafe {
                        *self.shared.add(((index as u32 + n as u32) & MAILBOX_RING_MASK) as usize)
                    };
                }
                on_packet(CommandPacket::parse(&bytes[..length as usize]));
            }
            self.consumer = (self.consumer.wrapping_add(u16::from(length) * 2)) & 0x7ff;
        }
        unsafe { write_be16(self.shared.add(MAILBOX_CONSUMER_H as usize), self.consumer) };
    }

    pub fn gain(&self, channel: u32) -> u8 {
        unsafe { *self.shared.add((MAILBOX_GAIN_CH0 + (channel & 1)) as usize) }
    }

    pub fn consumer(&self) -> u16 {
        self.consumer
    }

    pub fn reset(&mut self) {
        self.consumer = 0;
    }

    pub fn restore(&mut self, consumer: u16) {
        self.consumer = consumer;
    }
}
