//! The main-CPU mailbox ring in the 0x800-byte dual-port RAM.
//!
//! The RAM belongs to the main CPU bus and changes between calls, so the mailbox owns none of it:
//! each message borrows the RAM for its own duration.

use crate::hle::command::CommandPacket;
use crate::memory_map::{
    MAILBOX_CONSUMER_H, MAILBOX_GAIN_CH0, MAILBOX_PRODUCER_H, MAILBOX_RING_MASK, SHARED_RAM_SIZE,
};

pub const INDEX_MASK: u16 = 0x7fe;

#[derive(Default, Debug)]
pub struct SharedMailbox {
    consumer: u16,
}

/// Yields the packets between the consumer and producer indices and publishes the new consumer
/// index when dropped, so a partly consumed drain still acknowledges what it read.
pub struct MailboxDrain<'a> {
    mailbox: &'a mut SharedMailbox,
    ram: &'a mut [u8; SHARED_RAM_SIZE],
    producer: u16,
}

impl SharedMailbox {
    pub fn new() -> SharedMailbox {
        SharedMailbox::default()
    }

    pub fn valid_index(index: u16) -> bool {
        (index & !INDEX_MASK) == 0
    }

    pub fn drain<'a>(&'a mut self, ram: &'a mut [u8; SHARED_RAM_SIZE]) -> MailboxDrain<'a> {
        let producer = be16(ram, MAILBOX_PRODUCER_H) & INDEX_MASK;
        MailboxDrain {
            mailbox: self,
            ram,
            producer,
        }
    }

    pub fn gain(&self, ram: &[u8; SHARED_RAM_SIZE], channel: u32) -> u8 {
        ram[(MAILBOX_GAIN_CH0 + (channel & 1)) as usize]
    }

    pub fn consumer(&self) -> u16 {
        self.consumer
    }

    pub fn reset(&mut self) {
        self.consumer = 0;
    }

    /// Installs a saved consumer index; an index the ring cannot hold is refused.
    pub fn restore(&mut self, consumer: u16) -> Result<(), &'static str> {
        if !Self::valid_index(consumer) {
            return Err("Invalid audio mailbox state");
        }
        self.consumer = consumer;
        Ok(())
    }
}

fn be16(ram: &[u8; SHARED_RAM_SIZE], offset: u32) -> u16 {
    let at = offset as usize;
    u16::from_be_bytes([ram[at], ram[at + 1]])
}

impl Iterator for MailboxDrain<'_> {
    type Item = CommandPacket;

    fn next(&mut self) -> Option<CommandPacket> {
        while self.mailbox.consumer != self.producer {
            let index = u32::from(self.mailbox.consumer >> 1);
            let length = self.ram[index as usize];
            let available = (self.producer.wrapping_sub(self.mailbox.consumer) & 0x7ff) >> 1;
            // A bad length means the producer is mid-write; leave it for the next drain.
            if length < 2 || u16::from(length) > available {
                return None;
            }
            let packet = if usize::from(length) <= CommandPacket::MAX_BYTES {
                let mut bytes = [0u8; CommandPacket::MAX_BYTES];
                for (n, byte) in bytes.iter_mut().enumerate().take(length as usize) {
                    *byte = self.ram[((index + n as u32) & MAILBOX_RING_MASK) as usize];
                }
                Some(CommandPacket::parse(&bytes[..length as usize]))
            } else {
                None
            };
            self.mailbox.consumer =
                self.mailbox.consumer.wrapping_add(u16::from(length) * 2) & 0x7ff;
            if packet.is_some() {
                return packet;
            }
        }
        None
    }
}

impl Drop for MailboxDrain<'_> {
    fn drop(&mut self) {
        let at = MAILBOX_CONSUMER_H as usize;
        self.ram[at..at + 2].copy_from_slice(&self.mailbox.consumer.to_be_bytes());
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn ram() -> [u8; SHARED_RAM_SIZE] {
        [0u8; SHARED_RAM_SIZE]
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
        let mut mailbox = SharedMailbox::new();
        mailbox.restore(0x3fe << 1).unwrap();
        let drained = mailbox.drain(&mut ram).count();
        assert_eq!(drained, 1);
        assert_eq!(mailbox.consumer(), ((0x3fe << 1) + 8) & 0x7ff);
        assert_eq!(consumer_ack(&ram), 4);
    }

    #[test]
    fn invalid_length_stops_without_consuming() {
        let mut ram = ram();
        ram[0] = 1;
        set_producer(&mut ram, 4);
        let mut mailbox = SharedMailbox::new();
        assert_eq!(mailbox.drain(&mut ram).count(), 0);
        assert_eq!(mailbox.consumer(), 0);
        assert_eq!(consumer_ack(&ram), 0);
    }

    #[test]
    fn length_beyond_published_bytes_waits_for_producer() {
        let mut ram = ram();
        put_packet(&mut ram, 0, 4);
        set_producer(&mut ram, 4);
        let mut mailbox = SharedMailbox::new();
        assert_eq!(mailbox.drain(&mut ram).count(), 0);
        assert_eq!(mailbox.consumer(), 0);
    }

    #[test]
    fn oversize_packet_is_skipped_but_consumed() {
        let mut ram = ram();
        put_packet(&mut ram, 0, 10);
        put_packet(&mut ram, 10, 2);
        set_producer(&mut ram, 24);
        let mut mailbox = SharedMailbox::new();
        assert_eq!(mailbox.drain(&mut ram).count(), 1);
        assert_eq!(mailbox.consumer(), 24);
    }

    #[test]
    fn gain_reads_the_channel_byte() {
        let mut ram = ram();
        ram[MAILBOX_GAIN_CH0 as usize] = 7;
        ram[MAILBOX_GAIN_CH0 as usize + 1] = 9;
        let mailbox = SharedMailbox::new();
        assert_eq!((mailbox.gain(&ram, 0), mailbox.gain(&ram, 1), mailbox.gain(&ram, 2)), (7, 9, 7));
    }

    #[test]
    fn restore_refuses_indices_outside_the_ring() {
        let mut mailbox = SharedMailbox::new();
        for index in [0x800, 0x802, 0x001] {
            assert_eq!(mailbox.restore(index), Err("Invalid audio mailbox state"), "{index:#x}");
            assert_eq!(mailbox.consumer(), 0);
        }
        assert_eq!(mailbox.restore(0x7fe), Ok(()));
        assert_eq!(mailbox.consumer(), 0x7fe);
    }
}
