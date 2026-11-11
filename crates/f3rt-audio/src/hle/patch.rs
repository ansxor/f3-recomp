//! `audio/hle/patch.hpp` + `audio/hle/patch.cpp`: one program record and the behavior its kind
//! selects.
//!
//! A Program is one 0x198-byte instrument. A nonzero enable mask holds three Layer records. A zero
//! mask holds seventeen DirectRegion key regions. ProgramService turns each record into a
//! LayerPatch or a DirectPatch, and the voice runs the behavior the record's kind selects. Every
//! field sits on an even byte. The odd bytes are unread.

#![allow(non_upper_case_globals)]

use crate::bytes::{read_be32, s16, s8};
use crate::hle::events::{NoteOnEvent, SAMPLE_RATE};
use crate::hle::sound_bank::SoundBank;
use crate::hle::sound_rom_5a7e9117::{
    LFO_WAVE_TABLE, PITCH_MOD_TABLE, RATE_TABLE, REVERSE_START_OFFSET, SPLIT_MODE_TABLE,
    SPLIT_RECORD_STRIDE, SPECIAL_SPLIT_TABLE,
};
use crate::hle::track_parameters::TrackParameters;
use crate::hle::voice::{
    accumulator_start, advance_glide, initialize_envelope, segment, set_glide, update_random,
    volume_target, wrap_product, Envelope, NoiseState, Voice, NATIVE_RATE, VOLUME_RAMP,
};

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum EnvelopeRole {
    Pitch = 0,
    Filter = 1,
    Amplitude = 2,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum ModSource {
    Lfo = 0,
    PitchEnvelope = 1,
    FilterEnvelope = 2,
    AmplitudeEnvelope = 3,
    Random = 4,
    PreviousRandom = 5,
    Velocity = 6,
    Key = 7,
    Mod1A = 8,
    Mod12 = 9,
    Bend = 10,
    Mod10 = 11,
    Mod0C = 12,
    Pressure = 13,
    Full = 14,
    Off = 15,
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum AllocationMode {
    #[default]
    Poly = 0,
    Retrigger = 1,
    Legato = 2,
    LegatoHold = 3,
}

// Bit 7 also selects the k2 coefficient half (set = offset 0, clear = 1024),
// so modes 2 and 3 use the low half and modes 0 and 1 the high half.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum FilterMode {
    LpLpHpHp = 0,
    LpLpLpHp = 1,
    LpLpLp2Lp2 = 2,
    LpLpLpLp2 = 3,
}

// What a sample id does to start, end, and phase. The four ranges partition 0..127.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct SampleKind {
    pub id: u8,
}

impl SampleKind {
    pub fn ordinary(&self) -> bool {
        self.id < 0x54
    }

    pub fn phase_aligned(&self) -> bool {
        self.id >= 0x54 && self.id < 0x6f
    }

    pub fn probe(&self) -> bool {
        self.id >= 0x6f && self.id < 0x78
    }

    pub fn table(&self) -> bool {
        self.id >= 0x78
    }
}

// High nibble of an envelope's velocity byte, or of a direct region's sensitivity byte.
pub fn velocity_level_scale(nibble: u8) -> i32 {
    i32::from(nibble) * 8 + (i32::from(nibble) >> 1)
}

pub fn velocity_curve_index(curve: u8, velocity: u8) -> u32 {
    u32::from(curve) * 128 + u32::from(velocity)
}

pub struct EnvelopePatchAt;

impl EnvelopePatchAt {
    pub const size: usize = 20;
    pub const flags: usize = 16;
    pub const velocity: usize = 18;

    pub const fn level(i: u32) -> usize {
        (i * 4) as usize
    }

    pub const fn rate(i: u32) -> usize {
        (i * 4 + 2) as usize
    }
}

// Twenty bytes. Four breakpoints, then flags and the velocity scale.
// level(0) is where the generator starts. Rates 0..2 ramp toward level(1)..level(3).
// Rate 3 releases to zero. On the pitch envelope, level(3) is also the pitch zero.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct EnvelopePatch<const ROLE: u8> {
    pub bytes: *const u8,
}

impl<const ROLE: u8> EnvelopePatch<ROLE> {
    pub const size: usize = EnvelopePatchAt::size;

    pub fn new(bytes: *const u8) -> EnvelopePatch<ROLE> {
        EnvelopePatch { bytes }
    }

    fn get(&self, offset: usize) -> u8 {
        unsafe { *self.bytes.add(offset) }
    }

    pub fn level(&self, i: u32) -> u8 {
        self.get(EnvelopePatchAt::level(i)) & 127
    }

    pub fn rate(&self, i: u32) -> u8 {
        self.get(EnvelopePatchAt::rate(i)) & 127
    }

    pub fn flags(&self) -> u8 {
        self.get(EnvelopePatchAt::flags)
    }

    pub fn key_rate(&self) -> i8 {
        ((self.get(EnvelopePatchAt::flags) & 15) << 4) as i8 >> 4
    }

    pub fn velocity_curve(&self) -> u8 {
        (self.get(EnvelopePatchAt::flags) >> 4) & 3
    }

    pub fn one_shot(&self) -> bool {
        (self.get(EnvelopePatchAt::flags) & 0x40) != 0
    }

    pub fn r#loop(&self) -> bool {
        (self.get(EnvelopePatchAt::flags) & 0x80) != 0
    }

    pub fn velocity_level(&self) -> u8 {
        self.get(EnvelopePatchAt::velocity) >> 4
    }

    pub fn velocity_attack(&self) -> u8 {
        self.get(EnvelopePatchAt::velocity) & 15
    }
}

impl EnvelopePatch<{ EnvelopeRole::Pitch as u8 }> {
    pub fn pitch_zero(&self) -> u8 {
        self.level(3)
    }
}

/// `Layer::At`: the byte offsets of one layer record.
pub struct LayerAt;

impl LayerAt {
    pub const size: usize = 0x76;
    pub const pitch_envelope: usize = 0x00;
    pub const filter_envelope: usize = 0x14;
    pub const amplitude_envelope: usize = 0x28;
    pub const coarse_pitch: usize = 0x3c;
    pub const fine_pitch: usize = 0x3e;
    pub const pitch_envelope_depth: usize = 0x40;
    pub const lfo_pitch_depth: usize = 0x42;
    pub const flags: usize = 0x44;
    pub const pitch_source_depth: usize = 0x46;
    pub const k1_base: usize = 0x48;
    pub const k1_key: usize = 0x4a;
    pub const k1_envelope: usize = 0x4c;
    pub const filter_source: usize = 0x4e;
    pub const filter_source_depth: usize = 0x50;
    pub const k2_base: usize = 0x52;
    pub const k2_key: usize = 0x54;
    pub const k2_envelope: usize = 0x56;
    pub const weight_amount: usize = 0x58;
    pub const weight_low: usize = 0x5a;
    pub const weight_high: usize = 0x5c;
    pub const level: usize = 0x5e;
    pub const amplitude_source: usize = 0x60;
    pub const amplitude_source_depth: usize = 0x62;
    pub const lfo_source: usize = 0x64;
    pub const lfo_depth: usize = 0x66;
    pub const lfo_rate: usize = 0x68;
    pub const lfo_wave: usize = 0x6a;
    pub const sample_id: usize = 0x6c;
    pub const gate: usize = 0x6e;
    pub const start_amount: usize = 0x70;
    pub const start_offset: usize = 0x72;
    pub const delay: usize = 0x74;
}

/// A layer record view. The bytes belong to the sound ROM; the view is an unchecked borrow.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Layer {
    pub bytes: *const u8,
}

impl Layer {
    pub const size: usize = LayerAt::size;

    pub fn new(bytes: *const u8) -> Layer {
        Layer { bytes }
    }

    fn get(&self, offset: usize) -> u8 {
        unsafe { *self.bytes.add(offset) }
    }

    pub fn pitch(&self) -> EnvelopePatch<{ EnvelopeRole::Pitch as u8 }> {
        EnvelopePatch::new(unsafe { self.bytes.add(LayerAt::pitch_envelope) })
    }

    pub fn filter(&self) -> EnvelopePatch<{ EnvelopeRole::Filter as u8 }> {
        EnvelopePatch::new(unsafe { self.bytes.add(LayerAt::filter_envelope) })
    }

    pub fn amplitude(&self) -> EnvelopePatch<{ EnvelopeRole::Amplitude as u8 }> {
        EnvelopePatch::new(unsafe { self.bytes.add(LayerAt::amplitude_envelope) })
    }

    // Key * 256, plus coarse semitones and fine in steps of 2/256 semitone.
    // Bit 7 of the k1 base byte selects the played key; clear centers on key 60.
    pub fn pitch_word(&self, key: i32) -> i16 {
        let sum = (if self.use_played_key() { key } else { 60 }) * 256
            + i32::from(self.fine_pitch()) * 2
            + (i32::from(self.coarse_pitch()) << 8);
        sum as u16 as i16
    }

    pub fn coarse_pitch(&self) -> u8 {
        self.get(LayerAt::coarse_pitch)
    }

    pub fn fine_pitch(&self) -> i8 {
        self.get(LayerAt::fine_pitch) as i8
    }

    pub fn pitch_envelope_depth(&self) -> i8 {
        self.get(LayerAt::pitch_envelope_depth) as i8
    }

    pub fn lfo_pitch_depth(&self) -> i8 {
        self.get(LayerAt::lfo_pitch_depth) as i8
    }

    pub fn pitch_source(&self) -> ModSource {
        match self.get(LayerAt::flags) & 15 {
            0 => ModSource::Lfo,
            1 => ModSource::PitchEnvelope,
            2 => ModSource::FilterEnvelope,
            3 => ModSource::AmplitudeEnvelope,
            4 => ModSource::Random,
            5 => ModSource::PreviousRandom,
            6 => ModSource::Velocity,
            7 => ModSource::Key,
            8 => ModSource::Mod1A,
            9 => ModSource::Mod12,
            10 => ModSource::Bend,
            11 => ModSource::Mod10,
            12 => ModSource::Mod0C,
            13 => ModSource::Pressure,
            14 => ModSource::Full,
            _ => ModSource::Off,
        }
    }

    pub fn allocation(&self) -> AllocationMode {
        match (self.get(LayerAt::flags) >> 4) & 3 {
            0 => AllocationMode::Poly,
            1 => AllocationMode::Retrigger,
            2 => AllocationMode::Legato,
            _ => AllocationMode::LegatoHold,
        }
    }

    pub fn filter_mode(&self) -> FilterMode {
        match self.get(LayerAt::flags) >> 6 {
            0 => FilterMode::LpLpHpHp,
            1 => FilterMode::LpLpLpHp,
            2 => FilterMode::LpLpLp2Lp2,
            _ => FilterMode::LpLpLpLp2,
        }
    }

    pub fn k2_low_half(&self) -> bool {
        (self.get(LayerAt::flags) & 128) != 0
    }

    pub fn k2_table(&self) -> u32 {
        if self.k2_low_half() { 0 } else { 1024 }
    }

    pub fn pitch_source_depth(&self) -> i8 {
        self.get(LayerAt::pitch_source_depth) as i8
    }

    pub fn use_played_key(&self) -> bool {
        (self.get(LayerAt::k1_base) & 128) != 0
    }

    pub fn k1_base(&self) -> u8 {
        self.get(LayerAt::k1_base) & 127
    }

    pub fn k1_key(&self) -> i8 {
        self.get(LayerAt::k1_key) as i8
    }

    pub fn k1_envelope(&self) -> i8 {
        self.get(LayerAt::k1_envelope) as i8
    }

    pub fn filter_source(&self) -> ModSource {
        match self.get(LayerAt::filter_source) & 15 {
            0 => ModSource::Lfo,
            1 => ModSource::PitchEnvelope,
            2 => ModSource::FilterEnvelope,
            3 => ModSource::AmplitudeEnvelope,
            4 => ModSource::Random,
            5 => ModSource::PreviousRandom,
            6 => ModSource::Velocity,
            7 => ModSource::Key,
            8 => ModSource::Mod1A,
            9 => ModSource::Mod12,
            10 => ModSource::Bend,
            11 => ModSource::Mod10,
            12 => ModSource::Mod0C,
            13 => ModSource::Pressure,
            14 => ModSource::Full,
            _ => ModSource::Off,
        }
    }

    pub fn default_pan_byte(&self) -> u8 {
        self.get(LayerAt::filter_source) & 0xf0
    }

    pub fn default_pan(&self) -> i8 {
        self.default_pan_byte() as i8
    }

    pub fn filter_source_depth(&self) -> i8 {
        self.get(LayerAt::filter_source_depth) as i8
    }

    pub fn k2_apply_source(&self) -> bool {
        (self.get(LayerAt::k2_base) & 128) != 0
    }

    pub fn k2_base(&self) -> u8 {
        self.get(LayerAt::k2_base) & 127
    }

    pub fn k2_key(&self) -> i8 {
        self.get(LayerAt::k2_key) as i8
    }

    pub fn k2_envelope(&self) -> i8 {
        self.get(LayerAt::k2_envelope) as i8
    }

    pub fn weight_amount(&self) -> i8 {
        self.get(LayerAt::weight_amount) as i8
    }

    pub fn weight_low(&self) -> u8 {
        self.get(LayerAt::weight_low)
    }

    pub fn weight_high(&self) -> u8 {
        self.get(LayerAt::weight_high)
    }

    pub fn level(&self) -> u8 {
        self.get(LayerAt::level) & 127
    }

    pub fn level_plus_43(&self) -> bool {
        (self.get(LayerAt::level) & 128) != 0
    }

    pub fn amplitude_source(&self) -> ModSource {
        match self.get(LayerAt::amplitude_source) & 15 {
            0 => ModSource::Lfo,
            1 => ModSource::PitchEnvelope,
            2 => ModSource::FilterEnvelope,
            3 => ModSource::AmplitudeEnvelope,
            4 => ModSource::Random,
            5 => ModSource::PreviousRandom,
            6 => ModSource::Velocity,
            7 => ModSource::Key,
            8 => ModSource::Mod1A,
            9 => ModSource::Mod12,
            10 => ModSource::Bend,
            11 => ModSource::Mod10,
            12 => ModSource::Mod0C,
            13 => ModSource::Pressure,
            14 => ModSource::Full,
            _ => ModSource::Off,
        }
    }

    pub fn pair_choice(&self) -> u8 {
        (self.get(LayerAt::amplitude_source) >> 4) & 3
    }

    pub fn priority(&self) -> u8 {
        self.get(LayerAt::amplitude_source) & 0xc0
    }

    pub fn amplitude_source_depth(&self) -> i8 {
        self.get(LayerAt::amplitude_source_depth) as i8
    }

    pub fn lfo_source(&self) -> ModSource {
        match self.get(LayerAt::lfo_source) & 15 {
            0 => ModSource::Lfo,
            1 => ModSource::PitchEnvelope,
            2 => ModSource::FilterEnvelope,
            3 => ModSource::AmplitudeEnvelope,
            4 => ModSource::Random,
            5 => ModSource::PreviousRandom,
            6 => ModSource::Velocity,
            7 => ModSource::Key,
            8 => ModSource::Mod1A,
            9 => ModSource::Mod12,
            10 => ModSource::Bend,
            11 => ModSource::Mod10,
            12 => ModSource::Mod0C,
            13 => ModSource::Pressure,
            14 => ModSource::Full,
            _ => ModSource::Off,
        }
    }

    pub fn random_period(&self) -> u8 {
        self.get(LayerAt::lfo_source) >> 4
    }

    // High nibble expanded the way the voice stores its random countdown.
    pub fn random_countdown(&self) -> u16 {
        let mut period = u32::from(self.get(LayerAt::lfo_source) & 0xf0) >> 1;
        period |= period >> 4;
        ((-(period as i32)) & 127) as u16
    }

    pub fn lfo_depth(&self) -> u8 {
        self.get(LayerAt::lfo_depth) & 127
    }

    pub fn lfo_phase_reset(&self) -> bool {
        (self.get(LayerAt::lfo_depth) & 128) != 0
    }

    pub fn lfo_rate(&self) -> u8 {
        self.get(LayerAt::lfo_rate)
    }

    pub fn lfo_wave(&self) -> u8 {
        self.get(LayerAt::lfo_wave) & 15
    }

    pub fn lfo_fade(&self) -> u8 {
        self.get(LayerAt::lfo_wave) >> 4
    }

    pub fn lfo_fade_offset(&self) -> u8 {
        self.get(LayerAt::lfo_wave) & 0xf0
    }

    pub fn sample_id(&self) -> u8 {
        self.get(LayerAt::sample_id) & 127
    }

    pub fn reverse(&self) -> bool {
        (self.get(LayerAt::sample_id) & 128) != 0
    }

    pub fn sample(&self) -> SampleKind {
        SampleKind { id: self.sample_id() }
    }

    // Whole signed byte. Bits 7..4 are the velocity gate; the low nibble is start_source.
    pub fn velocity_gate(&self) -> i8 {
        self.get(LayerAt::gate) as i8
    }

    pub fn start_source(&self) -> ModSource {
        match self.get(LayerAt::gate) & 15 {
            0 => ModSource::Lfo,
            1 => ModSource::PitchEnvelope,
            2 => ModSource::FilterEnvelope,
            3 => ModSource::AmplitudeEnvelope,
            4 => ModSource::Random,
            5 => ModSource::PreviousRandom,
            6 => ModSource::Velocity,
            7 => ModSource::Key,
            8 => ModSource::Mod1A,
            9 => ModSource::Mod12,
            10 => ModSource::Bend,
            11 => ModSource::Mod10,
            12 => ModSource::Mod0C,
            13 => ModSource::Pressure,
            14 => ModSource::Full,
            _ => ModSource::Off,
        }
    }

    pub fn start_amount(&self) -> i8 {
        self.get(LayerAt::start_amount) as i8
    }

    pub fn start_amount_byte(&self) -> u8 {
        self.get(LayerAt::start_amount)
    }

    pub fn start_offset(&self) -> u8 {
        self.get(LayerAt::start_offset)
    }

    pub fn delay_ticks(&self) -> u8 {
        self.get(LayerAt::delay)
    }

    pub fn starts_on_release(&self) -> bool {
        self.get(LayerAt::delay) == 0xfb
    }
}

/// `DirectRegion::At`: the byte offsets of one direct key region.
pub struct DirectRegionAt;

impl DirectRegionAt {
    pub const size: usize = 20;
    pub const decay: usize = 0x00;
    pub const release: usize = 0x02;
    pub const sample_id: usize = 0x04;
    pub const transpose: usize = 0x06;
    pub const sensitivity: usize = 0x08;
    pub const key_low: usize = 0x0a;
    pub const key_high: usize = 0x0c;
    pub const filter_index: usize = 0x0e;
    pub const level: usize = 0x10;
    pub const pan: usize = 0x12;
}

/// A direct region view. The bytes belong to the sound ROM; the view is an unchecked borrow.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct DirectRegion {
    pub bytes: *const u8,
}

impl DirectRegion {
    pub const size: usize = DirectRegionAt::size;

    pub fn new(bytes: *const u8) -> DirectRegion {
        DirectRegion { bytes }
    }

    fn get(&self, offset: usize) -> u8 {
        unsafe { *self.bytes.add(offset) }
    }

    pub fn decay_rate(&self) -> u8 {
        self.get(DirectRegionAt::decay) & 127
    }

    pub fn ignore_note_off(&self) -> bool {
        (self.get(DirectRegionAt::decay) & 128) != 0
    }

    pub fn release_rate(&self) -> u8 {
        self.get(DirectRegionAt::release) & 127
    }

    pub fn root_key_relative(&self) -> bool {
        (self.get(DirectRegionAt::release) & 128) != 0
    }

    pub fn sample_id(&self) -> u8 {
        self.get(DirectRegionAt::sample_id) & 127
    }

    pub fn reverse(&self) -> bool {
        (self.get(DirectRegionAt::sample_id) & 128) != 0
    }

    pub fn sample(&self) -> SampleKind {
        SampleKind { id: self.sample_id() }
    }

    pub fn transpose(&self) -> i8 {
        self.get(DirectRegionAt::transpose) as i8
    }

    pub fn fine_pitch(&self) -> u8 {
        self.get(DirectRegionAt::sensitivity) & 15
    }

    pub fn velocity_level(&self) -> u8 {
        self.get(DirectRegionAt::sensitivity) >> 4
    }

    pub fn key_low(&self) -> i8 {
        self.get(DirectRegionAt::key_low) as i8
    }

    pub fn key_high(&self) -> i8 {
        self.get(DirectRegionAt::key_high) as i8
    }

    pub fn contains(&self, key: i32) -> bool {
        key >= i32::from(self.key_low()) && key <= i32::from(self.key_high())
    }

    // Raw low-key byte. The root-key formula subtracts this, not the signed range.
    pub fn key_low_byte(&self) -> u8 {
        self.get(DirectRegionAt::key_low)
    }

    pub fn filter_index(&self) -> u8 {
        self.get(DirectRegionAt::filter_index)
    }

    pub fn level(&self) -> u8 {
        self.get(DirectRegionAt::level) & 127
    }

    pub fn level_plus_168(&self) -> bool {
        (self.get(DirectRegionAt::level) & 128) != 0
    }

    pub fn pan(&self) -> u8 {
        self.get(DirectRegionAt::pan) & 15
    }

    // Nibble 0 is center: XOR 8 flips it around the middle, then scale into the pan table.
    pub fn centered_pan(&self) -> i32 {
        i32::from((self.pan() ^ 8) << 3)
    }

    pub fn pair_choice(&self) -> u8 {
        (self.get(DirectRegionAt::pan) >> 4) & 3
    }

    pub fn velocity_curve(&self) -> u8 {
        self.get(DirectRegionAt::pan) >> 6
    }

    // channel_transpose is already sign-extended. The result is the 8-bit key used as key * 256.
    pub fn sounding_key(&self, kernel: i32, channel_transpose: i32) -> i8 {
        let key = if self.root_key_relative() {
            (kernel - i32::from(self.key_low_byte()) + 60) as u8 as i32
        } else {
            60
        };
        (key + channel_transpose + i32::from(self.transpose())) as u8 as i8
    }
}

/// `Program::At`: the byte offsets of one instrument record.
pub struct ProgramAt;

impl ProgramAt {
    pub const size: usize = 0x198;
    pub const glide: usize = 0x17e;
    pub const enabled: usize = 0x196;
    pub const layer_count: u32 = 3;
    pub const region_count: u32 = 17;
}

/// An instrument record view. The bytes belong to the sound ROM; the view is an unchecked borrow.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Program {
    pub bytes: *const u8,
}

impl Program {
    pub const size: usize = ProgramAt::size;
    pub const layer_count: u32 = ProgramAt::layer_count;
    pub const region_count: u32 = ProgramAt::region_count;

    pub fn new(bytes: *const u8) -> Program {
        Program { bytes }
    }

    fn get(&self, offset: usize) -> u8 {
        unsafe { *self.bytes.add(offset) }
    }

    /// `operator bool`.
    pub fn is_some(&self) -> bool {
        !self.bytes.is_null()
    }

    // Top three bits of byte 0x196. Zero selects the seventeen key regions.
    pub fn enabled(&self) -> u8 {
        self.get(ProgramAt::enabled) >> 5
    }

    pub fn glide(&self) -> u8 {
        self.get(ProgramAt::glide)
    }

    pub fn layer(&self, index: u32) -> Layer {
        Layer::new(unsafe { self.bytes.add(index as usize * Layer::size) })
    }

    pub fn region(&self, index: u32) -> DirectRegion {
        DirectRegion::new(unsafe { self.bytes.add(index as usize * DirectRegion::size) })
    }
}

// Where the previous note left this physical slot's sample window. A probe sample keeps it.
#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct PriorWindow {
    pub start: f64,
    pub end: f64,
    pub valid: bool,
}

// A note being started, after the track's allocation decision.
#[derive(Clone, Copy, Debug)]
pub struct NoteStart {
    pub event: *const NoteOnEvent,
    pub kernel: i32,
    pub transpose: i32,
    pub mode: AllocationMode,
    pub prior_pitch: i32,
    pub prior_found: bool,
    pub program: Program,
}

// One envelope advance: whether the voice ends, and whether a looping amplitude envelope restarted
// the sample.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct EnvelopeTick {
    pub ended: bool,
    pub restarted: bool,
}

// The extra work a layered voice does on admission: a direct voice waits for the ring.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct OnsetStep {
    pub ended: bool,
    pub restarted: bool,
    pub parameters: bool,
}

fn clamp_i32(value: i32, low: i32, high: i32) -> i32 {
    if value < low {
        low
    } else if high < value {
        high
    } else {
        value
    }
}

fn modulation(v: &Voice, selector: ModSource) -> i16 {
    let parameters = unsafe { &(*v.track).config.parameters };
    match selector {
        ModSource::Lfo => v.lfo,
        ModSource::PitchEnvelope => v.envelope[Voice::pitch_envelope as usize].level(),
        ModSource::FilterEnvelope => v.envelope[Voice::filter_envelope as usize].level(),
        ModSource::AmplitudeEnvelope => v.envelope[Voice::amplitude_envelope as usize].level(),
        ModSource::Random => v.random,
        ModSource::PreviousRandom => v.previous_random,
        ModSource::Velocity => s16(u32::from(v.owner.velocity) * 257),
        ModSource::Key => s16(((i32::from(v.effective_key) - 60) * 256) as u32),
        ModSource::Mod1A => s16(u32::from(parameters.mod_1a())),
        ModSource::Mod12 => s16(u32::from(parameters.mod_12())),
        ModSource::Bend => s16(u32::from(parameters.bend())),
        ModSource::Mod10 => s16(u32::from(parameters.mod_10())),
        ModSource::Mod0C => s16(u32::from(parameters.mod_0c())),
        ModSource::Pressure => s16(u32::from(v.pressure) * 256),
        ModSource::Full => 0x7fff,
        ModSource::Off => 0,
    }
}

fn update_lfo(v: &mut Voice, layer: &Layer, bank: &SoundBank) {
    v.lfo_depth = s16((i32::from(v.lfo_depth) + i32::from(v.lfo_step)) as u32);
    if (v.lfo_depth as u16) > (v.lfo_target as u16) {
        v.lfo_step = 0;
        v.lfo_depth = v.lfo_target;
    }
    v.lfo_phase = v.lfo_phase.wrapping_add(bank.phase_rates[usize::from(layer.lfo_rate())]);
    let wave = (u32::from(layer.lfo_wave()) << 8) | u32::from(v.lfo_phase >> 8);
    let selected = i32::from(v.lfo_depth) + i32::from(modulation(v, layer.lfo_source()));
    let depth = clamp_i32(selected, 0, 32767);
    let shape = i32::from(s8(u32::from(bank.rom.read_byte(LFO_WAVE_TABLE + wave))));
    v.lfo = s16(((depth * shape) >> 7) as u32);
}

// The target may name the sign bit as a level; the position keeps the resolved level.
fn settle_level(e: &mut Envelope) {
    let mut level = (e.target >> 16) as u16;
    if e.slope <= 0 {
        level ^= 0x8000;
    }
    e.position = u32::from(level) << 16;
}

// c1983c: ramp the level to 0 at the direct region's release rate.
fn start_direct_release(e: &mut Envelope, release_rate: u32, bank: &SoundBank) {
    e.done = false;
    e.position &= 0xffff0000;
    let rate = s16(u32::from(bank.rom.read_u16(RATE_TABLE + 2 * release_rate)));
    e.slope = wrap_product(-i64::from(0x7f00) * i64::from(rate) * 2);
    e.target = if e.slope <= 0 { 0x80000000 } else { 0 };
    e.stage = 4;
}

// Split addresses are word offsets inside the sample bank the selector named.
fn apply_window(v: &mut Voice, bit: u32, board: u32, start: u32, phase: u32, end: u32) {
    v.start = f64::from(board) + f64::from((start | bit) & 0x1fffffe0) / 512.0;
    v.end = f64::from(board) + f64::from((end | bit) & 0x1fffffe0) / 512.0;
    v.position = f64::from(board) + f64::from((phase | bit) & 0x1fffffff) / 512.0;
}

fn pair_choice(selected: u32, patch: u32) -> u32 {
    if selected < 3 {
        return selected;
    }
    if selected == 5 { 3 } else { patch }
}

// Volume ramp, pair and filter targets shared by both kinds. The kind has already written the
// target volumes, the cutoff registers and the frequency.
fn finish_parameters(v: &mut Voice, initial: bool, pair: u32) {
    v.increment = f64::from(v.frequency) * f64::from(NATIVE_RATE) / (1024.0 * f64::from(SAMPLE_RATE));
    v.pair = ((pair + 1) & 3) as u8;
    v.target_left = f64::from(v.left_volume);
    v.target_right = f64::from(v.right_volume);
    if initial {
        v.left = v.target_left;
        v.right = v.target_right;
        v.gain_frames = 0;
    } else {
        v.gain_frames =
            if v.left == v.target_left && v.right == v.target_right { 0 } else { VOLUME_RAMP };
    }
    if v.gain_frames != 0 {
        v.step_left = (v.target_left - v.left) / f64::from(v.gain_frames);
        v.step_right = (v.target_right - v.right) / f64::from(v.gain_frames);
    }
    v.left_segment.frames = 0;
    v.right_segment.frames = 0;
    v.gain_dirty = true;
    v.filter.targets(v.filter_cutoff_low, v.filter_cutoff_high, initial);
}

/// The fields the C++ `Patch` base class holds, shared by both patch kinds.
pub struct PatchBase {
    pub bank: *const SoundBank,
    pub program: *const u8,
    pub index: u8,
    pub delay_ticks: u8,
    pub starts_on_release: bool,
}

impl PatchBase {
    pub fn new(
        bank: *const SoundBank,
        program: *const u8,
        index: u8,
        delay_ticks: u8,
        starts_on_release: bool,
    ) -> PatchBase {
        PatchBase { bank, program, index, delay_ticks, starts_on_release }
    }

    pub fn bank(&self) -> *const SoundBank {
        self.bank
    }

    pub fn program(&self) -> Program {
        Program::new(self.program)
    }

    // Layer 0..2, or region 0..16.
    pub fn index(&self) -> u8 {
        self.index
    }

    pub fn delay_ticks(&self) -> u8 {
        self.delay_ticks
    }

    pub fn starts_on_release(&self) -> bool {
        self.starts_on_release
    }
}

pub struct LayerPatch {
    pub base: PatchBase,
    pub view: Layer,
}

impl LayerPatch {
    pub fn new(
        view: Layer,
        bank: *const SoundBank,
        program: *const u8,
        index: u8,
        delay_ticks: u8,
        starts_on_release: bool,
    ) -> LayerPatch {
        LayerPatch {
            base: PatchBase::new(bank, program, index, delay_ticks, starts_on_release),
            view,
        }
    }

    pub fn layered(&self) -> bool {
        true
    }

    pub fn allocation(&self) -> AllocationMode {
        self.view.allocation()
    }

    pub fn priority(&self) -> u32 {
        u32::from(self.view.priority())
    }

    // Key weight, or -1 when this note is gated out of the patch.
    pub fn weight(&self, note: &NoteOnEvent, kernel: i32) -> i32 {
        let bank = unsafe { &*self.base.bank() };
        let key_weight = bank.key_weight(self.view, kernel);
        if key_weight < 0 {
            return -1;
        }
        let threshold = i32::from(self.view.velocity_gate());
        let boundary = ((threshold & !15).abs() * 9 / 8) & 0x7c;
        if if threshold < 0 {
            i32::from(note.velocity) >= boundary
        } else {
            i32::from(note.velocity) < boundary
        } {
            return -1;
        }
        key_weight
    }

    // Whether an active voice of the same layer slot may be taken over for legato or retrigger.
    pub fn reuses(&self, voice: &Voice) -> bool {
        unsafe { (*voice.patch).layered() && (*voice.patch).index() == self.base.index() }
    }

    // Pitch, envelopes and LFO for the new voice; then its split and sample window.
    pub fn initialize(&self, v: &mut Voice, start: &NoteStart, noise: &mut NoiseState) {
        let bank = unsafe { &*self.base.bank() };
        v.glide_target = self.view.pitch_word(start.kernel);
        v.pitch = clamp_i32(i32::from(v.glide_target), 0, 0x7f00) as i16;
        if start.program.glide() != 0
            && (start.mode == AllocationMode::Legato || start.prior_found)
        {
            let destination = v.glide_target;
            v.glide_target = if start.prior_found {
                s16(start.prior_pitch as u32)
            } else {
                s16(
                    u32::from(
                        unsafe { (*start.event).previous_key }
                            .wrapping_add(self.view.coarse_pitch()),
                    ) * 256,
                )
            };
            set_glide(v, destination, u32::from(start.program.glide()), bank);
        }
        initialize_envelope(
            unsafe { &mut *core::ptr::addr_of_mut!(v.envelope[Voice::pitch_envelope as usize]) },
            self.view.pitch(),
            v,
            bank,
        );
        initialize_envelope(
            unsafe { &mut *core::ptr::addr_of_mut!(v.envelope[Voice::filter_envelope as usize]) },
            self.view.filter(),
            v,
            bank,
        );
        initialize_envelope(
            unsafe {
                &mut *core::ptr::addr_of_mut!(v.envelope[Voice::amplitude_envelope as usize])
            },
            self.view.amplitude(),
            v,
            bank,
        );
        v.lfo_target = (u32::from(self.view.lfo_depth()) << 8) as i16;
        v.lfo_phase = if self.view.lfo_phase_reset() { 0 } else { noise.value };
        v.lfo_step = ((v.lfo_target as u32)
            .wrapping_mul(u32::from(
                bank.rom.read_u16(RATE_TABLE + u32::from(self.view.lfo_fade_offset())),
            ))
            >> 16) as i16;
        v.filter.mode = self.view.filter_mode() as u8;
        v.random_period = self.view.random_countdown();
        v.random_countdown = self.view.random_countdown();
        noise.history = noise.history.wrapping_add(noise.value);
        noise.value = noise.value.wrapping_add(noise.history);
        v.random = s16(u32::from(noise.value));
        v.previous_random = v.random;
        // The opening LFO step precedes the initial parameters, which read v.lfo.
        update_lfo(v, &self.view, bank);
    }

    pub fn address(&self, v: &mut Voice, prior: &PriorWindow) {
        let bank = unsafe { &*self.base.bank() };
        v.split = bank.split_for(
            self.view,
            u32::from(self.base.index()),
            i32::from(v.pitch),
            &mut v.selector,
            v.owner.program,
        );
        let sample = self.view.sample();
        let bit = u32::from(v.selector & 1) << 28;
        let board = (u32::from(v.selector >> 1) & bank.board_bank_mask) << 20;
        let mut control = if sample.probe() {
            0x0b
        } else {
            bank.rom.read_byte(SPLIT_MODE_TABLE + u32::from(v.split.mode))
        };
        v.reverse = self.view.reverse() && sample.ordinary();
        if v.reverse {
            control = 0x43;
        }
        let mut start = if v.reverse { v.split.accumulator } else { v.split.start };
        let mut phase = if v.reverse { v.split.end } else { v.split.accumulator };
        let mut end = v.split.end;
        if sample.phase_aligned() {
            phase &= 0xfffffe00;
        }
        if !sample.table() {
            phase &= 0xffffff00;
        } else {
            let mut d0 = u32::from(self.view.start_offset());
            let amount_end = d0 + u32::from(self.view.start_amount_byte());
            let mut d1 = if amount_end < 0xfa { amount_end } else { 0xfa };
            d0 *= 14;
            d1 *= 14;
            let split_table = SPECIAL_SPLIT_TABLE;
            let mut d4 = unsafe { read_be32(bank.rom.pointer_bytes(split_table + d0 + 2, 4)) };
            if d4 == 0x1dcaa07f {
                d0 += 14;
                d4 = unsafe { read_be32(bank.rom.pointer_bytes(split_table + d0 + 2, 4)) };
                if d0 == d1 {
                    d1 += 14;
                }
            }
            let mut d5 = unsafe { read_be32(bank.rom.pointer_bytes(split_table + d1 + 2, 4)) };
            if d5 == 0x1dcaa07f {
                d5 = unsafe {
                    read_be32(
                        bank.rom.pointer_bytes(split_table + d1 + SPLIT_RECORD_STRIDE as u32, 4),
                    )
                };
            }
            v.reverse = self.view.reverse();
            control = if v.reverse { 0x4b } else { 0x0b };
            start = d4;
            end = d5;
            phase = if v.reverse { d5 } else { d4 };
            if v.reverse {
                start += REVERSE_START_OFFSET;
            }
        }
        v.r#loop = (control & 8) != 0;
        v.bidirectional = (control & 16) != 0;
        apply_window(v, bit, board, start, phase, end);
        if sample.ordinary() {
            let mut amount = i32::from(self.view.start_amount()) * i32::from(v.owner.velocity)
                + (i32::from(self.view.start_offset()) << 7);
            if amount > 0 {
                let doubled = amount * 2;
                amount = if 32767 < doubled { 32767 } else { doubled };
                let length =
                    i32::from(s16((v.split.end.wrapping_sub(v.split.accumulator)) >> 10));
                let offset = wrap_product(i64::from(length) * i64::from(amount)) >> 5;
                v.position += f64::from(if v.reverse { -offset } else { offset }) / 512.0;
                v.position = if v.position < v.start {
                    v.start
                } else if v.end < v.position {
                    v.end
                } else {
                    v.position
                };
            }
        }
        if sample.probe() {
            // c17e8c loads ACC from this bank's probe. c17fbe leaves START/END
            // unchanged, including when the physical slot was previously free.
            v.boundary = true;
            v.r#loop = true;
            v.start = f64::from(board)
                + if prior.valid { prior.start } else { f64::from(bank.reset_start) / 512.0 };
            v.end = f64::from(board)
                + if prior.valid { prior.end } else { f64::from(bank.reset_end) / 512.0 };
            v.position = f64::from(board)
                + f64::from(bank.banks[usize::from(v.selector)].probe & 0x1fffffff) / 512.0;
        }
    }

    // Legato re-entry of a still-running voice: the new key's pitch becomes the glide destination.
    pub fn retarget(&self, v: &mut Voice, kernel: i32, program: &Program) {
        let bank = unsafe { &*self.base.bank() };
        set_glide(v, self.view.pitch_word(kernel), u32::from(program.glide()), bank);
    }

    pub fn step(&self, v: &mut Voice, noise: &mut NoiseState) -> EnvelopeTick {
        let bank = unsafe { &*self.base.bank() };
        let result = self.advance_envelopes(v);
        update_lfo(v, &self.view, bank);
        update_random(v, noise);
        result
    }

    pub fn onset(&self, v: &mut Voice, noise: &mut NoiseState) -> OnsetStep {
        let bank = unsafe { &*self.base.bank() };
        let envelopes = self.advance_envelopes(v);
        if envelopes.ended {
            return OnsetStep {
                ended: true,
                restarted: envelopes.restarted,
                parameters: false,
            };
        }
        update_lfo(v, &self.view, bank);
        update_random(v, noise);
        advance_glide(v);
        v.apply_parameters(false); // c181da calls c18372 before key-on.
        OnsetStep { ended: false, restarted: envelopes.restarted, parameters: true }
    }

    // Computes and writes the voice's parameter latches (LVOL/RVOL, FC, K1/K2, pair) from the track
    // record and the current envelope/LFO state. Called at key-on and whenever a track event or the
    // ring re-syncs the voice; `initial` also snaps the volume/filter ramps instead of ramping to
    // the new targets.
    pub fn apply_parameters(&self, v: &mut Voice, initial: bool) {
        let bank = unsafe { &*self.base.bank() };
        let c = unsafe { &(*v.track).config.parameters };
        v.owner.program = unsafe { (*v.track).config.program }; // program in force, as the note's channel snapshot held it
        let mut common = i32::from(bank.attenuation[usize::from(c.volume_index())])
            + i32::from(bank.attenuation[usize::from(unsafe { (*v.track).sequence_volume } & 127)]);
        common += i32::from(c.expression_offset());
        let amp = self.view.amplitude_source();
        let mut base = if amp == ModSource::Off {
            254
        } else {
            let amp_source = i32::from(modulation(v, amp));
            let x = if amp == ModSource::Lfo {
                (amp_source >> 1) + 0x4000
            } else if 0 < amp_source {
                amp_source
            } else {
                0
            };
            let factor = i32::from(self.view.amplitude_source_depth());
            let mut value = (x * factor) >> 15;
            if factor >= 0 {
                value -= factor;
            }
            (value + 127) * 2
        };
        base += i32::from(bank.attenuation[usize::from(self.view.level())])
            + (if self.view.level_plus_43() { 43 } else { 0 })
            + common
            + i32::from(bank.attenuation[usize::from(v.weight & 127)]);
        let mut pan_value = i32::from(c.signed_pan());
        if pan_value <= -127 {
            pan_value = i32::from(self.view.default_pan());
        }
        let pan_position = (pan_value >> 1) + 64;
        let env = i32::from(v.envelope[Voice::amplitude_envelope as usize].level() >> 5);
        v.left_volume = volume_target(
            (base + i32::from(bank.pan[(127 - pan_position) as usize])) * 4 + env,
            0x1b64,
        );
        v.right_volume = volume_target(
            (base + i32::from(bank.pan[pan_position as usize])) * 4 + env,
            0x1b64,
        );
        let filter_env = i32::from(v.envelope[Voice::filter_envelope as usize].level());
        let key = i32::from(v.effective_key) - 60;
        let mut first = (filter_env * i32::from(self.view.k1_envelope())
            + i32::from(modulation(v, self.view.filter_source()))
                * i32::from(self.view.filter_source_depth()))
            >> 12;
        first += (key * i32::from(self.view.k1_key())) >> 3;
        first += i32::from(self.view.k1_base()) * 8;
        v.filter_cutoff_low = bank.coefficient(first, 0);
        let mut second = (filter_env * i32::from(self.view.k2_envelope())
            + (if self.view.k2_apply_source() {
                i32::from(modulation(v, self.view.filter_source()))
            } else {
                0
            }) * i32::from(self.view.filter_source_depth()))
            >> 12;
        second += (key * i32::from(self.view.k2_key())) >> 3;
        second += i32::from(self.view.k2_base()) * 8;
        v.filter_cutoff_high = bank.coefficient(second, self.view.k2_table());
        let pitch_env = i32::from(s16(
            (i32::from(v.envelope[Voice::pitch_envelope as usize].level())
                - (i32::from(self.view.pitch().pitch_zero()) << 8)) as u32,
        ));
        let mut pitch_mod: i64 = i64::from(pitch_env) * i64::from(self.view.pitch_envelope_depth())
            + i64::from(v.lfo) * i64::from(self.view.lfo_pitch_depth());
        let depth = i32::from(self.view.pitch_source_depth());
        let mut multiplier =
            i32::from(bank.rom.read_u16(PITCH_MOD_TABLE + depth.unsigned_abs() * 2));
        if depth < 0 {
            multiplier = -i32::from(s16(multiplier as u32));
        }
        pitch_mod +=
            (i64::from(modulation(v, self.view.pitch_source())) * i64::from(multiplier)) >> 4;
        let bend = i32::from(s16(
            u32::from(c.bend()).wrapping_sub(TrackParameters::bend_center),
        ));
        pitch_mod >>= 4;
        pitch_mod += i64::from(bend) * i64::from(bank.bend_range);
        pitch_mod >>= 6;
        v.pitch = s16(
            (i32::from(v.glide_target) + i32::from(v.split.tuning) + (pitch_mod as i32)) as u32,
        );
        if (v.split.mode & 128) != 0 {
            let mut amount = (i32::from(self.view.start_amount())
                * i32::from(modulation(v, self.view.start_source())))
                >> 7;
            let biased = amount + (i32::from(self.view.start_offset()) << 8);
            amount = clamp_i32(biased, 0, 32767);
            let delta = i32::from(s16(
                ((i64::from(amount) * i64::from(s16(v.split.accumulator >> 16))) >> 16) as u32,
            ));
            let span = ((v.split.end.wrapping_sub(v.split.start)) >> 4) as u16 as i16;
            let offset = u32::from(span as u16)
                .wrapping_mul(u32::from(bank.frequency(delta)))
                >> 6;
            let start = v.split.end.wrapping_sub(offset) | (u32::from(v.selector & 1) << 28);
            v.start = f64::from(u32::from(v.selector >> 1) << 20)
                + f64::from(start & 0x1fffffe0) / 512.0;
            v.pitch = s16((i32::from(v.pitch) + delta) as u32);
        }
        v.frequency = bank.frequency(i32::from(v.pitch));
        finish_parameters(
            v,
            initial,
            pair_choice(u32::from(c.pair_select()), u32::from(self.view.pair_choice())),
        );
    }

    // True when the release ends the voice immediately.
    pub fn release(&self, v: &mut Voice) -> bool {
        let bank = unsafe { &*self.base.bank() };
        for e in v.envelope.iter_mut() {
            if (e.flags & 64) != 0 {
                continue;
            }
            e.done = false;
            e.stage = 4;
            let rate = i32::from(e.rates[3]);
            segment(e, 0, rate, bank);
        }
        if (v.envelope[Voice::amplitude_envelope as usize].flags & 64) == 0
            && v.envelope[Voice::amplitude_envelope as usize].level() == 0
        {
            return true;
        }
        if v.boundary {
            let value = v.split.end;
            let board = u32::from(v.selector >> 1) << 20;
            if (value & 0x80000000) != 0 {
                v.start = f64::from(board) + f64::from(value & 0x1fffffe0) / 512.0;
                v.position = v.end;
                v.reverse = true;
            } else {
                v.end = f64::from(board) + f64::from(value & 0x1fffffe0) / 512.0;
                v.position = v.start;
            }
            v.r#loop = false;
        }
        false
    }

    fn advance_envelopes(&self, v: &mut Voice) -> EnvelopeTick {
        let mut result = EnvelopeTick::default();
        for which in [Voice::pitch_envelope, Voice::filter_envelope, Voice::amplitude_envelope] {
            let tick = self.advance(v, which);
            result.ended |= tick.ended;
            result.restarted |= tick.restarted;
        }
        result
    }

    fn advance(&self, v: &mut Voice, which: u32) -> EnvelopeTick {
        let bank = unsafe { &*self.base.bank() };
        let e = &mut v.envelope[which as usize];
        if e.done {
            return EnvelopeTick::default();
        }
        if !e.advance() {
            return EnvelopeTick::default();
        }
        settle_level(e);
        if e.stage == 4 {
            e.done = true;
            return EnvelopeTick { ended: which == Voice::amplitude_envelope, restarted: false };
        }
        if e.intermediate {
            e.intermediate = false;
            let target = u32::from(e.levels[1]);
            let rate = i32::from(e.rates[0]);
            segment(e, target, rate, bank);
            return EnvelopeTick::default();
        }
        e.stage = e.stage.wrapping_add(1);
        if e.stage == 1 {
            let target = u32::from(e.levels[2]);
            let rate = i32::from(e.rates[1]);
            segment(e, target, rate, bank);
        } else if e.stage == 2 {
            let target = u32::from(e.levels[3]);
            let rate = i32::from(e.rates[2]);
            segment(e, target, rate, bank);
        } else if (e.flags & 0x40) != 0 {
            e.stage = 4;
            let rate = i32::from(e.rates[3]);
            segment(e, 0, rate, bank);
        } else if (e.flags & 0x80) != 0 {
            e.stage = 0;
            let target = u32::from(e.levels[1]);
            let rate = i32::from(e.rates[0]);
            segment(e, target, rate, bank);
            if which == Voice::amplitude_envelope && self.view.sample().ordinary() {
                v.position = if v.reverse { v.end } else { accumulator_start(v) };
                if v.sample_finished {
                    v.sample_finished = false;
                    return EnvelopeTick { ended: false, restarted: true };
                }
            }
        } else {
            e.done = true;
            return EnvelopeTick {
                ended: which == Voice::amplitude_envelope && e.level() == 0,
                restarted: false,
            };
        }
        EnvelopeTick::default()
    }
}

pub struct DirectPatch {
    pub base: PatchBase,
    pub view: DirectRegion,
}

impl DirectPatch {
    pub fn new(
        view: DirectRegion,
        bank: *const SoundBank,
        program: *const u8,
        index: u8,
    ) -> DirectPatch {
        DirectPatch { base: PatchBase::new(bank, program, index, 0, false), view }
    }

    pub fn contains_key(&self, key: i32) -> bool {
        self.view.contains(key)
    }

    pub fn layered(&self) -> bool {
        false
    }

    pub fn allocation(&self) -> AllocationMode {
        AllocationMode::Poly
    }

    pub fn priority(&self) -> u32 {
        64
    }

    pub fn weight(&self, _note: &NoteOnEvent, _kernel: i32) -> i32 {
        127
    }

    pub fn reuses(&self, _voice: &Voice) -> bool {
        false
    }

    pub fn initialize(&self, v: &mut Voice, start: &NoteStart, _noise: &mut NoiseState) {
        let key = i32::from(self.view.sounding_key(start.kernel, start.transpose));
        let clamped = clamp_i32(key * 256, 0, 0x7f00);
        v.pitch = s16((clamped | (i32::from(self.view.fine_pitch()) << 4)) as u32);
    }

    pub fn address(&self, v: &mut Voice, _prior: &PriorWindow) {
        let bank = unsafe { &*self.base.bank() };
        v.split = bank.split_for_region(
            self.view,
            u32::from(self.base.index()),
            i32::from(v.pitch),
            &mut v.selector,
            v.owner.program,
        );
        let sample = self.view.sample();
        if sample.id == 0x78 {
            panic!("ROM direct sample78 enters infinite branch c1968e");
        }
        let bit = u32::from(v.selector & 1) << 28;
        let board = (u32::from(v.selector >> 1) & bank.board_bank_mask) << 20;
        let control = bank.rom.read_byte(SPLIT_MODE_TABLE + u32::from(v.split.mode));
        v.reverse = self.view.reverse() && sample.ordinary();
        let final_control = if v.reverse { 0x43 } else { control };
        let start = if v.reverse { v.split.accumulator } else { v.split.start };
        let mut phase = if v.reverse { v.split.end } else { v.split.accumulator };
        let end = v.split.end;
        if sample.phase_aligned() {
            phase &= 0xfffffe00;
        }
        v.r#loop = (final_control & 8) != 0;
        v.bidirectional = (final_control & 16) != 0;
        apply_window(v, bit, board, start, phase, end);
    }

    pub fn retarget(&self, _v: &mut Voice, _kernel: i32, _program: &Program) {}

    pub fn step(&self, v: &mut Voice, _noise: &mut NoiseState) -> EnvelopeTick {
        let bank = unsafe { &*self.base.bank() };
        let e = &mut v.envelope[Voice::amplitude_envelope as usize];
        if e.done {
            return EnvelopeTick::default();
        }
        if !e.advance() {
            return EnvelopeTick::default();
        }
        settle_level(e);
        // Timer expiry dispatches on the envelope stage (c1971c -> stage routine), not on note-off:
        // stage 0 starts the release ramp (c1983c: ends the voice if the level's top byte is already
        // 0), stage 4 ends the voice (c19888). A patch whose note-off is ignored (patch[0] bit 7)
        // still decays when its timer expires.
        if e.stage == 4 || (e.position >> 24) == 0 {
            e.done = true;
            return EnvelopeTick { ended: true, restarted: false };
        }
        let release_rate = u32::from(self.view.release_rate());
        start_direct_release(e, release_rate, bank);
        EnvelopeTick::default()
    }

    pub fn onset(&self, _v: &mut Voice, _noise: &mut NoiseState) -> OnsetStep {
        // A direct voice writes its parameters once and waits for the ring.
        OnsetStep::default()
    }

    pub fn apply_parameters(&self, v: &mut Voice, initial: bool) {
        let bank = unsafe { &*self.base.bank() };
        let c = unsafe { &(*v.track).config.parameters };
        v.owner.program = unsafe { (*v.track).config.program };
        let common = i32::from(bank.attenuation[usize::from(c.volume_index())])
            + i32::from(bank.attenuation[usize::from(unsafe { (*v.track).sequence_volume } & 127)])
            + i32::from(c.expression_offset());
        let curve =
            velocity_curve_index(self.view.velocity_curve(), v.owner.velocity) as i32;
        let sensitivity = velocity_level_scale(self.view.velocity_level());
        let scale = i32::from(s16(
            (0x4000 - (128 - i32::from(bank.velocity_curves[curve as usize])) * sensitivity) as u32,
        )) >> 6;
        if initial {
            let e = &mut v.envelope[Voice::amplitude_envelope as usize];
            e.position = (u32::from((i32::from(self.view.level()) * scale) as u16) << 16) | 0x7fff;
            e.slope = -i32::from(s16(u32::from(
                bank.rom.read_u16(RATE_TABLE + 2 * u32::from(self.view.decay_rate())),
            )));
            e.target = u32::from(((e.position >> 16) ^ 0x8000) as u16) << 16;
        }
        let pan_position = if c.pan() == 128 {
            self.view.centered_pan()
        } else {
            (i32::from(c.signed_pan()) >> 1) + 64
        };
        let level = i32::from(v.envelope[Voice::amplitude_envelope as usize].level() >> 5)
            + (if self.view.level_plus_168() { 0xa8 } else { 0 });
        v.left_volume =
            volume_target((common + i32::from(bank.pan[(127 - pan_position) as usize])) * 4 + level, 0xfa0);
        v.right_volume =
            volume_target((common + i32::from(bank.pan[pan_position as usize])) * 4 + level, 0xfa0);
        let filter_index = direct_filter_index(self.view.filter_index(), v.owner.velocity);
        v.filter_cutoff_high = bank.coefficient(filter_index, 0);
        v.filter_cutoff_low = v.filter_cutoff_high;
        v.frequency = bank.frequency(i32::from(v.pitch) + i32::from(v.split.tuning));
        finish_parameters(
            v,
            initial,
            pair_choice(u32::from(c.pair_select()), u32::from(self.view.pair_choice())),
        );
    }

    pub fn release(&self, v: &mut Voice) -> bool {
        let bank = unsafe { &*self.base.bank() };
        if self.view.ignore_note_off() {
            return false;
        }
        let release_rate = u32::from(self.view.release_rate());
        start_direct_release(
            &mut v.envelope[Voice::amplitude_envelope as usize],
            release_rate,
            bank,
        );
        false
    }
}

/// The behavior of one program record. The C++ virtual base class; each variant is one record kind.
pub enum Patch {
    Layer(LayerPatch),
    Direct(DirectPatch),
}

impl Patch {
    fn base(&self) -> &PatchBase {
        match self {
            Patch::Layer(p) => &p.base,
            Patch::Direct(p) => &p.base,
        }
    }

    pub fn bank(&self) -> *const SoundBank {
        self.base().bank()
    }

    pub fn program(&self) -> Program {
        self.base().program()
    }

    // Layer 0..2, or region 0..16.
    pub fn index(&self) -> u8 {
        self.base().index()
    }

    pub fn delay_ticks(&self) -> u8 {
        self.base().delay_ticks()
    }

    pub fn starts_on_release(&self) -> bool {
        self.base().starts_on_release()
    }

    pub fn layered(&self) -> bool {
        match self {
            Patch::Layer(p) => p.layered(),
            Patch::Direct(p) => p.layered(),
        }
    }

    pub fn allocation(&self) -> AllocationMode {
        match self {
            Patch::Layer(p) => p.allocation(),
            Patch::Direct(p) => p.allocation(),
        }
    }

    pub fn priority(&self) -> u32 {
        match self {
            Patch::Layer(p) => p.priority(),
            Patch::Direct(p) => p.priority(),
        }
    }

    // Key weight, or -1 when this note is gated out of the patch.
    pub fn weight(&self, note: &NoteOnEvent, kernel: i32) -> i32 {
        match self {
            Patch::Layer(p) => p.weight(note, kernel),
            Patch::Direct(p) => p.weight(note, kernel),
        }
    }

    // Whether an active voice of the same layer slot may be taken over for legato or retrigger.
    pub fn reuses(&self, voice: &Voice) -> bool {
        match self {
            Patch::Layer(p) => p.reuses(voice),
            Patch::Direct(p) => p.reuses(voice),
        }
    }

    // Pitch, envelopes and LFO for the new voice; then its split and sample window.
    pub fn initialize(&self, voice: &mut Voice, start: &NoteStart, noise: &mut NoiseState) {
        match self {
            Patch::Layer(p) => p.initialize(voice, start, noise),
            Patch::Direct(p) => p.initialize(voice, start, noise),
        }
    }

    pub fn address(&self, voice: &mut Voice, prior: &PriorWindow) {
        match self {
            Patch::Layer(p) => p.address(voice, prior),
            Patch::Direct(p) => p.address(voice, prior),
        }
    }

    // Legato re-entry of a still-running voice: the new key's pitch becomes the glide destination.
    pub fn retarget(&self, voice: &mut Voice, kernel: i32, program: &Program) {
        match self {
            Patch::Layer(p) => p.retarget(voice, kernel, program),
            Patch::Direct(p) => p.retarget(voice, kernel, program),
        }
    }

    pub fn step(&self, voice: &mut Voice, noise: &mut NoiseState) -> EnvelopeTick {
        match self {
            Patch::Layer(p) => p.step(voice, noise),
            Patch::Direct(p) => p.step(voice, noise),
        }
    }

    pub fn onset(&self, voice: &mut Voice, noise: &mut NoiseState) -> OnsetStep {
        match self {
            Patch::Layer(p) => p.onset(voice, noise),
            Patch::Direct(p) => p.onset(voice, noise),
        }
    }

    pub fn apply_parameters(&self, voice: &mut Voice, initial: bool) {
        match self {
            Patch::Layer(p) => p.apply_parameters(voice, initial),
            Patch::Direct(p) => p.apply_parameters(voice, initial),
        }
    }

    // True when the release ends the voice immediately.
    pub fn release(&self, voice: &mut Voice) -> bool {
        match self {
            Patch::Layer(p) => p.release(voice),
            Patch::Direct(p) => p.release(voice),
        }
    }
}

// Filter-table index of a direct (key-region) voice, as the sound driver computes it at
// c195a4-c195d0: d0 = (patch[0xe] & 15) * word8; d0 = (d0 << 7) >> 16; d0 += patch[0xe] * 4.
// word8 is voice +8, which the note-on routine (c17baa-c17bb2) fills with the note velocity (bit 7
// cleared) in both bytes, i.e. velocity * 257. The driver then clamps the table byte offset
// 2 * index to 0x7fe.
pub fn direct_filter_index(patch_e: u8, velocity: u8) -> i32 {
    let word8 = i32::from(velocity & 127) * 257;
    i32::from(patch_e) * 4 + ((i32::from(patch_e & 15) * word8) >> 9)
}
