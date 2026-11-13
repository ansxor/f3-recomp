//! `audio/hle/sound_rom_5a7e9117.hpp`: the sound ROM's fixed addresses, record layouts and small
//! record types.

use crate::hle::patch::{DirectRegionAt, LayerAt, ProgramAt};

// ROM addresses (68000 bus, 0xc00000 .. 0xc80000)
pub const PROGRAM_DATA_BASE: u32 = 0xc00134; // Program data bank 0
pub const BEND_RANGE_PTR: u32 = 0xc085f2; // Pitch bend range startup parameter
pub const RATE_TABLE: u32 = 0xc089dc; // Envelope / fade rate multipliers
pub const PHASE_RATE_TABLE: u32 = 0xc08aa4; // LFO phase rate table
pub const PAN_TABLE: u32 = 0xc08b6c; // Stereo pan table (128 entries)
pub const ATTENUATION_TABLE: u32 = 0xc08bec; // Attenuation curve (128 entries)
pub const VELOCITY_CURVE_TABLE: u32 = 0xc08c6c; // Velocity response curves (512 entries)
pub const FILTER_TABLE: u32 = 0xc08e6c; // Filter coefficient table
pub const PITCH_TABLE: u32 = 0xc09e6c; // Pitch frequency table
pub const PITCH_MOD_TABLE: u32 = 0xc0a46c; // Pitch modulation multipliers
pub const SPLIT_MODE_TABLE: u32 = 0xc0a734; // Sample split mode control table
pub const LFO_WAVE_TABLE: u32 = 0xc0a73c; // LFO waveform table
pub const CONTROLLER_DEFAULTS_TABLE: u32 = 0xc13556; // Track controller defaults
pub const HEAP_BASE_PTR: u32 = 0xc15fb6; // Sample RAM heap base pointer
pub const HEAP_LIMIT_PTR_A: u32 = 0xc15fba; // Sample RAM heap limit pointer word A
pub const HEAP_LIMIT_PTR_B: u32 = 0xc15fc0; // Sample RAM heap limit pointer word B
pub const HEAP_RESERVE_PTR: u32 = 0xc15fcc; // Sample RAM heap reserve size pointer
pub const HEAP_NEXT_PTR: u32 = 0xc15fd8; // Sample RAM heap next allocation pointer
pub const NOISE_SEED_PTR: u32 = 0xc16f04; // Random noise generator seed
pub const NOISE_HISTORY_PTR: u32 = 0xc16f0a; // Random noise generator history
pub const RESET_START_PTR: u32 = 0xc17b60; // Voice reset start pointer
pub const RESET_END_PTR: u32 = 0xc17b64; // Voice reset end pointer
pub const BUDGET_TABLE: u32 = 0xc1a56e; // Voice processing budget table
pub const SPLIT_TABLE_BASE: u32 = 0xc1ae76; // Sample split key table base
pub const SPECIAL_SPLIT_TABLE: u32 = 0xc1ae84; // Special sample split table
pub const SPLIT_DIRECTORY: u32 = 0xc1c1e0; // Sample split directory
pub const SEQ_STREAM_BASE: u32 = 0xc20000; // Sequence data streams base in ROM

// Sound ROM relative offsets (within 0x80000 sound image)
pub const VOLUME_PAN_TABLE_OFFSET: u32 = 0xa634; // Volume/pan attenuation table offset (0xc0a634)
pub const PRESET_TABLE_OFFSET: u32 = 0x1c2d2; // Effects preset parameter table offset (0xc1c2d2)

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
pub const PROGRAM_ENABLED_OFFSET: usize = 0x196;
pub const PROGRAM_BANK_MASK_OFFSET: usize = 0x154;
pub const PROGRAM_MAGIC_OFFSET_0: usize = 0x15c;
pub const PROGRAM_MAGIC_OFFSET_1: usize = 0x15e;
pub const PROGRAM_DEFAULT_BANK_OFFSET: usize = 0x160;
pub const PROGRAM_GLIDE_OFFSET: usize = 0x17e;
pub const SPLIT_RECORD_STRIDE: usize = 0x14;

// Sample ROM layout and offsets
pub const SAMPLE_BANK_SIZE: usize = 0x100000;
pub const SAMPLE_HALF_BANK_SIZE: usize = 0x80000;
pub const SAMPLE_TAG_PROBE_OFFSET: u32 = 0x7ffff;
pub const REVERSE_START_OFFSET: u32 = 0x800;

// Direct region record layout (20 bytes per region, 17 regions in a direct program)
pub const DIRECT_REGION_STRIDE: usize = 20;
pub const DIRECT_REGION_COUNT: usize = 17;
pub const DIRECT_PARAM_DECAY: usize = 0x00;
pub const DIRECT_PARAM_RELEASE: usize = 0x02;
pub const DIRECT_PARAM_SAMPLE_ID: usize = 0x04;
pub const DIRECT_PARAM_TRANSPOSE: usize = 0x06;
pub const DIRECT_PARAM_SENSITIVITY: usize = 0x08;
pub const DIRECT_PARAM_KEY_LOW: usize = 0x0a;
pub const DIRECT_PARAM_KEY_HIGH: usize = 0x0c;
pub const DIRECT_PARAM_FILTER_INDEX: usize = 0x0e;
pub const DIRECT_PARAM_LEVEL: usize = 0x10;
pub const DIRECT_PARAM_PAN_CHOICE: usize = 0x12;

// Layer record layout (0x76 = 118 bytes per layer, 3 layers in a layered program)
pub const LAYER_RECORD_STRIDE: usize = 0x76;
pub const LAYER_RECORD_COUNT: usize = 3;
pub const LAYER_ENV_PITCH_OFFSET: usize = 0x00;
pub const LAYER_ENV_FILTER_OFFSET: usize = 0x14; // +20
pub const LAYER_ENV_AMP_OFFSET: usize = 0x28; // +40
pub const LAYER_PARAM_BASE_PITCH: usize = 0x3c;
pub const LAYER_PARAM_BASE_FINE: usize = 0x3e;
pub const LAYER_PARAM_PITCH_ENV_DEPTH: usize = 0x40;
pub const LAYER_PARAM_LFO_PITCH_DEPTH: usize = 0x42;
pub const LAYER_PARAM_FLAGS: usize = 0x44;
pub const LAYER_PARAM_PITCH_SRC_DEPTH: usize = 0x46;
pub const LAYER_PARAM_K1_BASE: usize = 0x48;
pub const LAYER_PARAM_K1_KEY: usize = 0x4a;
pub const LAYER_PARAM_K1_ENV: usize = 0x4c;
pub const LAYER_PARAM_FILTER_SRC: usize = 0x4e;
pub const LAYER_PARAM_FILTER_SRC_DEPTH: usize = 0x50;
pub const LAYER_PARAM_K2_BASE: usize = 0x52;
pub const LAYER_PARAM_K2_KEY: usize = 0x54;
pub const LAYER_PARAM_K2_ENV: usize = 0x56;
pub const LAYER_PARAM_KEY_WEIGHT: usize = 0x58;
pub const LAYER_PARAM_KEY_LOW: usize = 0x5a;
pub const LAYER_PARAM_KEY_HIGH: usize = 0x5c;
pub const LAYER_PARAM_LEVEL: usize = 0x5e;
pub const LAYER_PARAM_AMP_SRC: usize = 0x60;
pub const LAYER_PARAM_AMP_SRC_DEPTH: usize = 0x62;
pub const LAYER_PARAM_LFO_SRC: usize = 0x64;
pub const LAYER_PARAM_LFO_DEPTH: usize = 0x66;
pub const LAYER_PARAM_LFO_RATE: usize = 0x68;
pub const LAYER_PARAM_LFO_WAVE: usize = 0x6a;
pub const LAYER_PARAM_SAMPLE_ID: usize = 0x6c;
pub const LAYER_PARAM_GATE_SRC: usize = 0x6e;
pub const LAYER_PARAM_START_AMOUNT: usize = 0x70;
pub const LAYER_PARAM_START_OFFSET: usize = 0x72;
pub const LAYER_PARAM_DELAY_TICKS: usize = 0x74;

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
    /// # Safety
    /// `p` must point to the 14 record bytes the C++ `read` reads.
    pub unsafe fn read(p: *const u8) -> Split {
        let byte = |i: usize| unsafe { *p.add(i) };
        Split {
            tuning: ((u16::from(byte(0)) << 8) | u16::from(byte(1))) as i16,
            accumulator: (u32::from(byte(2)) << 24)
                | (u32::from(byte(3)) << 16)
                | (u32::from(byte(4)) << 8)
                | u32::from(byte(5)),
            start: (u32::from(byte(6)) << 24)
                | (u32::from(byte(7)) << 16)
                | (u32::from(byte(8)) << 8)
                | u32::from(byte(9)),
            end: (u32::from(byte(10)) << 24)
                | (u32::from(byte(11)) << 16)
                | (u32::from(byte(12)) << 8)
                | u32::from(byte(13)),
            cutoff: byte(5),
            mode: byte(9),
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

// Offsets the record views and the layout constants above must agree on.
const _: () = assert!(LayerAt::size == LAYER_RECORD_STRIDE);
const _: () = assert!(LayerAt::pitch_envelope == LAYER_ENV_PITCH_OFFSET);
const _: () = assert!(LayerAt::filter_envelope == LAYER_ENV_FILTER_OFFSET);
const _: () = assert!(LayerAt::amplitude_envelope == LAYER_ENV_AMP_OFFSET);
const _: () = assert!(LayerAt::coarse_pitch == LAYER_PARAM_BASE_PITCH);
const _: () = assert!(LayerAt::fine_pitch == LAYER_PARAM_BASE_FINE);
const _: () = assert!(LayerAt::pitch_envelope_depth == LAYER_PARAM_PITCH_ENV_DEPTH);
const _: () = assert!(LayerAt::lfo_pitch_depth == LAYER_PARAM_LFO_PITCH_DEPTH);
const _: () = assert!(LayerAt::flags == LAYER_PARAM_FLAGS);
const _: () = assert!(LayerAt::pitch_source_depth == LAYER_PARAM_PITCH_SRC_DEPTH);
const _: () = assert!(LayerAt::k1_base == LAYER_PARAM_K1_BASE);
const _: () = assert!(LayerAt::k1_key == LAYER_PARAM_K1_KEY);
const _: () = assert!(LayerAt::k1_envelope == LAYER_PARAM_K1_ENV);
const _: () = assert!(LayerAt::filter_source == LAYER_PARAM_FILTER_SRC);
const _: () = assert!(LayerAt::filter_source_depth == LAYER_PARAM_FILTER_SRC_DEPTH);
const _: () = assert!(LayerAt::k2_base == LAYER_PARAM_K2_BASE);
const _: () = assert!(LayerAt::k2_key == LAYER_PARAM_K2_KEY);
const _: () = assert!(LayerAt::k2_envelope == LAYER_PARAM_K2_ENV);
const _: () = assert!(LayerAt::weight_amount == LAYER_PARAM_KEY_WEIGHT);
const _: () = assert!(LayerAt::weight_low == LAYER_PARAM_KEY_LOW);
const _: () = assert!(LayerAt::weight_high == LAYER_PARAM_KEY_HIGH);
const _: () = assert!(LayerAt::level == LAYER_PARAM_LEVEL);
const _: () = assert!(LayerAt::amplitude_source == LAYER_PARAM_AMP_SRC);
const _: () = assert!(LayerAt::amplitude_source_depth == LAYER_PARAM_AMP_SRC_DEPTH);
const _: () = assert!(LayerAt::lfo_source == LAYER_PARAM_LFO_SRC);
const _: () = assert!(LayerAt::lfo_depth == LAYER_PARAM_LFO_DEPTH);
const _: () = assert!(LayerAt::lfo_rate == LAYER_PARAM_LFO_RATE);
const _: () = assert!(LayerAt::lfo_wave == LAYER_PARAM_LFO_WAVE);
const _: () = assert!(LayerAt::sample_id == LAYER_PARAM_SAMPLE_ID);
const _: () = assert!(LayerAt::gate == LAYER_PARAM_GATE_SRC);
const _: () = assert!(LayerAt::start_amount == LAYER_PARAM_START_AMOUNT);
const _: () = assert!(LayerAt::start_offset == LAYER_PARAM_START_OFFSET);
const _: () = assert!(LayerAt::delay == LAYER_PARAM_DELAY_TICKS);
const _: () = assert!(DirectRegionAt::size == DIRECT_REGION_STRIDE);
const _: () = assert!(DirectRegionAt::decay == DIRECT_PARAM_DECAY);
const _: () = assert!(DirectRegionAt::release == DIRECT_PARAM_RELEASE);
const _: () = assert!(DirectRegionAt::sample_id == DIRECT_PARAM_SAMPLE_ID);
const _: () = assert!(DirectRegionAt::transpose == DIRECT_PARAM_TRANSPOSE);
const _: () = assert!(DirectRegionAt::sensitivity == DIRECT_PARAM_SENSITIVITY);
const _: () = assert!(DirectRegionAt::key_low == DIRECT_PARAM_KEY_LOW);
const _: () = assert!(DirectRegionAt::key_high == DIRECT_PARAM_KEY_HIGH);
const _: () = assert!(DirectRegionAt::filter_index == DIRECT_PARAM_FILTER_INDEX);
const _: () = assert!(DirectRegionAt::level == DIRECT_PARAM_LEVEL);
const _: () = assert!(DirectRegionAt::pan == DIRECT_PARAM_PAN_CHOICE);
const _: () = assert!(ProgramAt::size == PROGRAM_RECORD_STRIDE);
const _: () = assert!(ProgramAt::glide == PROGRAM_GLIDE_OFFSET);
const _: () = assert!(ProgramAt::enabled == PROGRAM_ENABLED_OFFSET);
const _: () = assert!(ProgramAt::layer_count == LAYER_RECORD_COUNT as u32);
const _: () = assert!(ProgramAt::region_count == DIRECT_REGION_COUNT as u32);
