//! Record formats and addresses shared by the supported sound drivers.

// ROM addresses (68000 bus, 0xc00000 .. 0xc80000)
pub const PROGRAM_DATA_BASE: u32 = 0xc00134; // Program data bank 0
pub const SEQ_STREAM_BASE: u32 = 0xc20000; // Sequence data streams base in ROM

// Sequencer / Voice parameter record offsets
pub const HEAP_METADATA_OFFSET: u32 = 0x6e9c; // Metadata offset for heap header

// Sequence header offsets and sizes
pub const SEQ_HEADER_SIZE: u32 = 0xa8;
pub const SEQ_HEADER_BARS_OFFSET: u32 = 0x12;
pub const SEQ_HEADER_TIME_SIG_FACTOR_OFFSET: u32 = 0x14;
pub const SEQ_HEADER_TIME_SIG_EXP_OFFSET: u32 = 0x15;
pub const SEQ_HEADER_LOOP_START_OFFSET: u32 = 0x16;
pub const SEQ_HEADER_LOOP_END_OFFSET: u32 = 0x1a;
pub const SEQ_HEADER_RATE_OFFSET: u32 = 0x1e;
pub const SEQ_HEADER_TRACKS_OFFSET: u32 = 0x20;
pub const SEQ_TRACK_RECORD_SIZE: usize = 14;
pub const SEQ_HEADER_LINKED_OFFSET: u32 = 0xa6;

// Program data record layout
pub const PROGRAM_RECORD_STRIDE: usize = 0x198;
pub const PROGRAM_COLUMN_STRIDE: u32 = 0x3fc0;
pub const PROGRAM_BANK_MASK_OFFSET: usize = 0x154;
pub const PROGRAM_MAGIC_OFFSET_0: usize = 0x15c;
pub const PROGRAM_MAGIC_OFFSET_1: usize = 0x15e;
pub const PROGRAM_DEFAULT_BANK_OFFSET: usize = 0x160;
pub const SPLIT_RECORD_SIZE: usize = 14;
// Differs from SPLIT_RECORD_SIZE on purpose: the sentinel fallback of the special split table keeps
// the stride the translation was verified with.
pub const SPECIAL_SPLIT_SKIP: u32 = 0x14;

// Sample ROM layout and offsets
pub const SAMPLE_BANK_SIZE: usize = 0x100000;
pub const SAMPLE_HALF_BANK_SIZE: usize = 0x80000;
pub const SAMPLE_TAG_PROBE_OFFSET: u32 = 0x7ffff;
pub const REVERSE_START_OFFSET: u32 = 0x800;

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Split {
    pub tuning: i16,
    pub accumulator: u32,
    pub start: u32,
    pub end: u32,
    pub cutoff: u8,
    pub mode: u8,
}

impl Split {
    /// Decodes the 14-byte record at the start of `bytes`.
    pub fn read(bytes: &[u8]) -> Split {
        let record = &bytes[..SPLIT_RECORD_SIZE];
        let be32 =
            |at: usize| u32::from_be_bytes([record[at], record[at + 1], record[at + 2], record[at + 3]]);
        Split {
            tuning: i16::from_be_bytes([record[0], record[1]]),
            accumulator: be32(2),
            start: be32(6),
            end: be32(10),
            cutoff: record[5],
            mode: record[9],
        }
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Bank {
    pub tag: u8,
    pub count: u8,
    pub offset: u32,
    pub probe: u32,
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct ProgramData {
    pub base: Option<&'static [u8]>,
    pub sample_banks: [i8; 17],
    pub enabled: u8,
}

pub fn program_slot(id: u16) -> u32 {
    (((u32::from(id) >> 14) & 3) * 256) | (u32::from(id) & 0xff)
}
