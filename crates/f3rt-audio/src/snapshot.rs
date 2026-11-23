//! The 32-byte big-endian save-state blob.

use crate::bytes::{read_be16_at, read_be64_at, write_be16_at, write_be64_at};
use crate::shared_mailbox::SharedMailbox;

pub const BYTES: usize = 32;
pub const CLOCK_AT: usize = 0;
pub const COMMANDS_AT: usize = 8;
pub const CONSUMER_AT: usize = 16;
pub const HELD_AT: usize = 18;
pub const RESERVED_AT: usize = 19;

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct AudioSnapshot {
    pub clock: u64,
    pub commands: u64,
    pub consumer: u16,
    pub held: bool,
}

impl AudioSnapshot {
    pub fn parse(src: &[u8]) -> Result<AudioSnapshot, &'static str> {
        if src.len() != BYTES {
            return Err("Audio state size mismatch");
        }
        let index = read_be16_at(src, CONSUMER_AT);
        let index_is_valid = SharedMailbox::valid_index(index);
        let held_is_boolean = src[HELD_AT] <= 1;
        let reserved_is_zero = src[RESERVED_AT..].iter().all(|&byte| byte == 0);
        if !(index_is_valid && held_is_boolean && reserved_is_zero) {
            return Err("Invalid audio mailbox state");
        }
        Ok(AudioSnapshot {
            clock: read_be64_at(src, CLOCK_AT),
            commands: read_be64_at(src, COMMANDS_AT),
            consumer: index,
            held: src[HELD_AT] != 0,
        })
    }

    pub fn write(&self, dst: &mut [u8]) {
        if dst.len() != BYTES {
            panic!("Audio state size mismatch");
        }
        dst.fill(0);
        write_be64_at(dst, CLOCK_AT, self.clock);
        write_be64_at(dst, COMMANDS_AT, self.commands);
        write_be16_at(dst, CONSUMER_AT, self.consumer);
        dst[HELD_AT] = self.held as u8;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample() -> AudioSnapshot {
        AudioSnapshot {
            clock: 0x0102_0304_0506_0708,
            commands: 99,
            consumer: 0x2a4,
            held: true,
        }
    }

    fn blob() -> [u8; BYTES] {
        let mut blob = [0u8; BYTES];
        sample().write(&mut blob);
        blob
    }

    #[test]
    fn round_trips() {
        assert_eq!(AudioSnapshot::parse(&blob()), Ok(sample()));
    }

    #[test]
    fn rejects_wrong_size() {
        assert_eq!(
            AudioSnapshot::parse(&blob()[..BYTES - 1]),
            Err("Audio state size mismatch")
        );
    }

    #[test]
    fn rejects_bad_consumer_held_and_reserved() {
        for (at, value) in [
            (CONSUMER_AT, 0x08),
            (CONSUMER_AT + 1, 0x01),
            (HELD_AT, 2),
            (BYTES - 1, 1),
        ] {
            let mut bad = blob();
            bad[at] = value;
            assert_eq!(
                AudioSnapshot::parse(&bad),
                Err("Invalid audio mailbox state"),
                "byte {at}"
            );
        }
    }
}
