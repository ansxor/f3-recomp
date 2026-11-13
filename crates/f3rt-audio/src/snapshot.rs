//! `audio/snapshot.hpp`: the 32-byte big-endian save-state blob.
//!
//! `parse` reports a malformed blob as an error instead of panicking, which is the Rust form of the
//! C++ `std::invalid_argument` the ABI shim catches to return F3RT_AUDIO_INVALID_STATE; the board is
//! left unchanged in that case.

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
        if !SharedMailbox::valid_index(index)
            || src[HELD_AT] > 1
            || src[RESERVED_AT..].iter().any(|&byte| byte != 0)
        {
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
