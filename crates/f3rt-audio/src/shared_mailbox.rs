//! The main-CPU mailbox ring in the 0x800-byte dual-port RAM.
//!
//! The RAM belongs to the main CPU bus and changes between calls, so it is read through the pointer
//! at each use and never held as a slice.

use crate::hle::command::CommandPacket;
use crate::memory_map::{
    MAILBOX_CONSUMER_H, MAILBOX_GAIN_CH0, MAILBOX_PRODUCER_H, MAILBOX_RING_MASK,
};

pub const INDEX_MASK: u16 = 0x7fe;

pub struct SharedMailbox {
    shared: *mut u8,
    consumer: u16,
}

/// Yields the packets between the consumer and producer indices and publishes the new consumer
/// index when dropped, so a partly consumed drain still acknowledges what it read.
pub struct MailboxDrain<'a> {
    mailbox: &'a mut SharedMailbox,
    producer: u16,
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
        SharedMailbox {
            shared,
            consumer: 0,
        }
    }

    pub fn drain(&mut self) -> MailboxDrain<'_> {
        let producer = self.read_be16(MAILBOX_PRODUCER_H) & INDEX_MASK;
        MailboxDrain {
            mailbox: self,
            producer,
        }
    }

    pub fn gain(&self, channel: u32) -> u8 {
        self.read_byte(MAILBOX_GAIN_CH0 + (channel & 1))
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

    fn read_byte(&self, offset: u32) -> u8 {
        debug_assert!(offset < 0x800);
        // SAFETY: `new` requires a non-null pointer to the 0x800-byte shared RAM and every offset
        // used here is below 0x800.
        unsafe { *self.shared.add(offset as usize) }
    }

    fn read_be16(&self, offset: u32) -> u16 {
        u16::from_be_bytes([self.read_byte(offset), self.read_byte(offset + 1)])
    }

    fn write_be16(&mut self, offset: u32, value: u16) {
        debug_assert!(offset + 1 < 0x800);
        let [high, low] = value.to_be_bytes();
        // SAFETY: same bounds as `read_byte`; the offset and its successor are inside the RAM.
        unsafe {
            *self.shared.add(offset as usize) = high;
            *self.shared.add(offset as usize + 1) = low;
        }
    }
}

impl Iterator for MailboxDrain<'_> {
    type Item = CommandPacket;

    fn next(&mut self) -> Option<CommandPacket> {
        let mailbox = &mut *self.mailbox;
        while mailbox.consumer != self.producer {
            let index = u32::from(mailbox.consumer >> 1);
            let length = mailbox.read_byte(index);
            let available = (self.producer.wrapping_sub(mailbox.consumer) & 0x7ff) >> 1;
            // A bad length means the producer is mid-write; leave it for the next drain.
            if length < 2 || u16::from(length) > available {
                return None;
            }
            let packet = if usize::from(length) <= CommandPacket::MAX_BYTES {
                let mut bytes = [0u8; CommandPacket::MAX_BYTES];
                for (n, byte) in bytes.iter_mut().enumerate().take(length as usize) {
                    *byte = mailbox.read_byte((index + n as u32) & MAILBOX_RING_MASK);
                }
                Some(CommandPacket::parse(&bytes[..length as usize]))
            } else {
                None
            };
            mailbox.consumer = mailbox.consumer.wrapping_add(u16::from(length) * 2) & 0x7ff;
            if packet.is_some() {
                return packet;
            }
        }
        None
    }
}

impl Drop for MailboxDrain<'_> {
    fn drop(&mut self) {
        let consumer = self.mailbox.consumer;
        self.mailbox.write_be16(MAILBOX_CONSUMER_H, consumer);
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ram() -> Vec<u8> {
        vec![0u8; 0x800]
    }

    fn set_producer(ram: &mut [u8], producer: u16) {
        ram[MAILBOX_PRODUCER_H as usize..][..2].copy_from_slice(&producer.to_be_bytes());
    }

    fn consumer_ack(ram: &[u8]) -> u16 {
        u16::from_be_bytes([
            ram[MAILBOX_CONSUMER_H as usize],
            ram[MAILBOX_CONSUMER_H as usize + 1],
        ])
    }

    /// Packet of `length` bytes written ring-wise from byte index `at`.
    fn put_packet(ram: &mut [u8], at: u32, length: u8) {
        ram[at as usize] = length;
        ram[((at + 1) & MAILBOX_RING_MASK) as usize] = 0xff;
        for n in 2..u32::from(length) {
            ram[((at + n) & MAILBOX_RING_MASK) as usize] = 0;
        }
    }

    #[test]
    fn drains_packet_wrapped_across_ring_end_and_publishes_consumer() {
        let mut ram = ram();
        let start_index = 0x3fe;
        put_packet(&mut ram, start_index, 4);
        set_producer(&mut ram, 4);
        let mut mailbox = SharedMailbox::new(ram.as_mut_ptr());
        mailbox.restore(0x3fe << 1);
        let drained = mailbox.drain().count();
        assert_eq!(drained, 1);
        assert_eq!(mailbox.consumer(), ((0x3fe << 1) + 8) & 0x7ff);
        assert_eq!(consumer_ack(&ram), 4);
    }

    #[test]
    fn invalid_length_stops_without_consuming() {
        let mut ram = ram();
        ram[0] = 1;
        set_producer(&mut ram, 4);
        let mut mailbox = SharedMailbox::new(ram.as_mut_ptr());
        assert_eq!(mailbox.drain().count(), 0);
        assert_eq!(mailbox.consumer(), 0);
        assert_eq!(consumer_ack(&ram), 0);
    }

    #[test]
    fn length_beyond_published_bytes_waits_for_producer() {
        let mut ram = ram();
        put_packet(&mut ram, 0, 4);
        set_producer(&mut ram, 4);
        let mut mailbox = SharedMailbox::new(ram.as_mut_ptr());
        assert_eq!(mailbox.drain().count(), 0);
        assert_eq!(mailbox.consumer(), 0);
    }

    #[test]
    fn oversize_packet_is_skipped_but_consumed() {
        let mut ram = ram();
        put_packet(&mut ram, 0, 10);
        put_packet(&mut ram, 10, 2);
        set_producer(&mut ram, 24);
        let mut mailbox = SharedMailbox::new(ram.as_mut_ptr());
        assert_eq!(mailbox.drain().count(), 1);
        assert_eq!(mailbox.consumer(), 24);
    }
}
