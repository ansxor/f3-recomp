//! One program record and the behavior its kind selects.
//!
//! A Program is one 0x198-byte instrument. A nonzero enable mask holds three Layer records. A zero
//! mask holds seventeen DirectRegion key regions. `ProgramQuery::resolve` turns the records a note
//! selects into `Patch` values, and the voice runs the behavior the record's kind selects. Every
//! field sits on an even byte. The odd bytes are unread.

#![allow(non_upper_case_globals)]

use crate::bytes::{s8, s16};
use crate::hle::events::{NO_PROGRAM, NoteOnEvent};
use crate::hle::sound_bank::SoundBank;
use crate::hle::sound_rom::{
    REVERSE_START_OFFSET, SPECIAL_SPLIT_SKIP, program_slot,
};
use crate::hle::track_parameters::TrackParameters;
use crate::hle::track_service::Track;
use crate::hle::voice::{NoiseState, Voice, volume_target, wrap_product};

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

impl ModSource {
    /// The selector lives in the low nibble of its byte; the high nibble carries unrelated flags.
    pub fn from_nibble(byte: u8) -> ModSource {
        match byte & 15 {
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
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct EnvelopePatch<'a, const ROLE: u8> {
    pub bytes: &'a [u8],
}

impl<'a, const ROLE: u8> EnvelopePatch<'a, ROLE> {
    pub const size: usize = EnvelopePatchAt::size;

    pub fn new(bytes: &'a [u8]) -> EnvelopePatch<'a, ROLE> {
        EnvelopePatch { bytes }
    }

    fn get(&self, offset: usize) -> u8 {
        self.bytes[offset]
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

impl EnvelopePatch<'_, { EnvelopeRole::Pitch as u8 }> {
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

/// A layer record view: the record's bytes in the sound ROM.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Layer<'a> {
    pub bytes: &'a [u8],
}

impl<'a> Layer<'a> {
    pub const size: usize = LayerAt::size;

    pub fn new(bytes: &'a [u8]) -> Layer<'a> {
        Layer { bytes }
    }

    fn get(&self, offset: usize) -> u8 {
        self.bytes[offset]
    }

    pub fn pitch(&self) -> EnvelopePatch<'a, { EnvelopeRole::Pitch as u8 }> {
        EnvelopePatch::new(&self.bytes[LayerAt::pitch_envelope..][..EnvelopePatchAt::size])
    }

    pub fn filter(&self) -> EnvelopePatch<'a, { EnvelopeRole::Filter as u8 }> {
        EnvelopePatch::new(&self.bytes[LayerAt::filter_envelope..][..EnvelopePatchAt::size])
    }

    pub fn amplitude(&self) -> EnvelopePatch<'a, { EnvelopeRole::Amplitude as u8 }> {
        EnvelopePatch::new(&self.bytes[LayerAt::amplitude_envelope..][..EnvelopePatchAt::size])
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
        ModSource::from_nibble(self.get(LayerAt::flags))
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
        ModSource::from_nibble(self.get(LayerAt::filter_source))
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
        ModSource::from_nibble(self.get(LayerAt::amplitude_source))
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
        ModSource::from_nibble(self.get(LayerAt::lfo_source))
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
        SampleKind {
            id: self.sample_id(),
        }
    }

    // Whole signed byte. Bits 7..4 are the velocity gate; the low nibble is start_source.
    pub fn velocity_gate(&self) -> i8 {
        self.get(LayerAt::gate) as i8
    }

    pub fn start_source(&self) -> ModSource {
        ModSource::from_nibble(self.get(LayerAt::gate))
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

/// A direct region view: the region's bytes in the sound ROM.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct DirectRegion<'a> {
    pub bytes: &'a [u8],
}

impl<'a> DirectRegion<'a> {
    pub const size: usize = DirectRegionAt::size;

    pub fn new(bytes: &'a [u8]) -> DirectRegion<'a> {
        DirectRegion { bytes }
    }

    fn get(&self, offset: usize) -> u8 {
        self.bytes[offset]
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
        SampleKind {
            id: self.sample_id(),
        }
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

/// An instrument record view: the record's bytes in the sound ROM.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Program<'a> {
    pub bytes: &'a [u8],
}

impl<'a> Program<'a> {
    pub const size: usize = ProgramAt::size;
    pub const layer_count: u32 = ProgramAt::layer_count;
    pub const region_count: u32 = ProgramAt::region_count;

    pub fn new(bytes: &'a [u8]) -> Program<'a> {
        Program { bytes }
    }

    fn get(&self, offset: usize) -> u8 {
        self.bytes[offset]
    }

    // Top three bits of byte 0x196. Zero selects the seventeen key regions.
    pub fn enabled(&self) -> u8 {
        self.get(ProgramAt::enabled) >> 5
    }

    pub fn glide(&self) -> u8 {
        self.get(ProgramAt::glide)
    }

    pub fn layer(&self, index: u32) -> Layer<'a> {
        Layer::new(&self.bytes[index as usize * Layer::size..][..Layer::size])
    }

    pub fn region(&self, index: u32) -> DirectRegion<'a> {
        DirectRegion::new(&self.bytes[index as usize * DirectRegion::size..][..DirectRegion::size])
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
    pub previous_key: u8,
    pub kernel: i32,
    pub transpose: i32,
    pub mode: AllocationMode,
    pub prior_pitch: i32,
    pub prior_found: bool,
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

fn pair_choice(selected: u32, patch: u32) -> u32 {
    if selected < 3 {
        return selected;
    }
    if selected == 5 { 3 } else { patch }
}

/// What a note asks the program table for. `key` is the kernel key after the track transpose;
/// `layer_mask` is the track's controller layer mask, zero when the program's own enable mask
/// applies.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ProgramQuery {
    pub program: u16,
    pub key: u8,
    pub layer_mask: u8,
    pub notes_suppressed: bool,
}

impl Default for ProgramQuery {
    fn default() -> ProgramQuery {
        ProgramQuery {
            program: NO_PROGRAM,
            key: 0,
            layer_mask: 0,
            notes_suppressed: false,
        }
    }
}

impl ProgramQuery {
    pub fn for_note(note: &NoteOnEvent, track: &Track) -> ProgramQuery {
        let parameters = &track.config.parameters;
        ProgramQuery {
            program: note.program,
            key: parameters.transposed_key(note.kernel_key),
            layer_mask: parameters.layer_mask(),
            notes_suppressed: parameters.notes_suppressed(),
        }
    }

    /// A layered program yields up to three layer patches in descending layer order, a direct
    /// program yields the one key region containing the key.
    pub fn resolve(self, bank: &SoundBank) -> PatchList {
        let mut list = PatchList::default();
        if self.program == NO_PROGRAM {
            return list;
        }
        let Some(program_offset) = bank.program_data(program_slot(self.program)).base else {
            return list;
        };
        let program = bank.program_at(program_offset);
        if program.enabled() == 0 {
            let key = i32::from(s8(u32::from(self.key)));
            let region = (0..Program::region_count)
                .map(|index| DirectPatch::new(program_offset, index as u8))
                .find(|patch| patch.contains_key(key, bank));
            if let Some(patch) = region {
                list.push(Patch::Direct(patch));
            }
            return list;
        }
        if self.notes_suppressed {
            return list;
        }
        let enabled = if self.layer_mask != 0 {
            u32::from(self.layer_mask)
        } else {
            u32::from(program.enabled())
        };
        // Descending layer order keeps the voice allocation and the driver start order of the
        // layered onset.
        for layer in (0..Program::layer_count).rev() {
            if enabled & (1 << layer) != 0 {
                list.push(Patch::Layer(LayerPatch::new(program_offset, layer as u8)));
            }
        }
        list
    }
}

/// The patches one note starts, in voice-allocation order.
#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
pub struct PatchList {
    patches: [Option<Patch>; Program::layer_count as usize],
    count: u8,
}

impl PatchList {
    fn push(&mut self, patch: Patch) {
        self.patches[usize::from(self.count)] = Some(patch);
        self.count += 1;
    }

    pub fn len(&self) -> usize {
        usize::from(self.count)
    }

    pub fn is_empty(&self) -> bool {
        self.count == 0
    }

    pub fn iter(&self) -> impl Iterator<Item = Patch> + '_ {
        self.patches[..self.len()].iter().flatten().copied()
    }
}

/// One layer record of a layered program.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct LayerPatch {
    pub program_offset: usize,
    pub index: u8,
}

impl LayerPatch {
    pub fn new(program_offset: usize, index: u8) -> LayerPatch {
        LayerPatch {
            program_offset,
            index,
        }
    }

    pub fn program<'a>(&self, bank: &'a SoundBank) -> Program<'a> {
        bank.program_at(self.program_offset)
    }

    pub fn view<'a>(&self, bank: &'a SoundBank) -> Layer<'a> {
        self.program(bank).layer(u32::from(self.index))
    }

    pub fn delay_ticks(&self, bank: &SoundBank) -> u8 {
        self.view(bank).delay_ticks()
    }

    pub fn starts_on_release(&self, bank: &SoundBank) -> bool {
        self.view(bank).starts_on_release()
    }

    pub fn allocation(&self, bank: &SoundBank) -> AllocationMode {
        self.view(bank).allocation()
    }

    pub fn priority(&self, bank: &SoundBank) -> u32 {
        u32::from(self.view(bank).priority())
    }

    // Key weight, or -1 when this note is gated out of the patch.
    pub fn weight(&self, note: &NoteOnEvent, kernel: i32, bank: &SoundBank) -> i32 {
        let view = self.view(bank);
        let key_weight = bank.key_weight(view, kernel);
        if key_weight < 0 {
            return -1;
        }
        let threshold = i32::from(view.velocity_gate());
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
        matches!(voice.patch, Some(Patch::Layer(other)) if other.index == self.index)
    }

    // Pitch, envelopes and LFO for the new voice; then its split and sample window.
    pub fn initialize(
        &self,
        v: &mut Voice,
        start: &NoteStart,
        noise: &mut NoiseState,
        bank: &SoundBank,
        track: &Track,
    ) {
        let view = self.view(bank);
        v.glide_target = view.pitch_word(start.kernel);
        v.pitch = i32::from(v.glide_target).clamp(0, 0x7f00) as i16;
        let glide = self.program(bank).glide();
        if glide != 0 && (start.mode == AllocationMode::Legato || start.prior_found)
        {
            let destination = v.glide_target;
            v.glide_target = if start.prior_found {
                s16(start.prior_pitch as u32)
            } else {
                s16(u32::from(start.previous_key.wrapping_add(view.coarse_pitch())) * 256)
            };
            v.set_glide(destination, u32::from(glide), bank);
        }
        v.envelope[Voice::pitch_envelope as usize].initialize(
            view.pitch(),
            v.owner.velocity,
            v.effective_key,
            bank,
        );
        v.envelope[Voice::filter_envelope as usize].initialize(
            view.filter(),
            v.owner.velocity,
            v.effective_key,
            bank,
        );
        v.envelope[Voice::amplitude_envelope as usize].initialize(
            view.amplitude(),
            v.owner.velocity,
            v.effective_key,
            bank,
        );
        v.lfo_target = (u32::from(view.lfo_depth()) << 8) as i16;
        v.lfo_phase = if view.lfo_phase_reset() {
            0
        } else {
            noise.value
        };
        v.lfo_step = ((v.lfo_target as u32).wrapping_mul(u32::from(
            bank.rom
                .read_u16(bank.layout.rate_table + u32::from(view.lfo_fade_offset())),
        )) >> 16) as i16;
        v.filter.mode = view.filter_mode() as u8;
        v.random_period = view.random_countdown();
        v.random_countdown = view.random_countdown();
        noise.history = noise.history.wrapping_add(noise.value);
        noise.value = noise.value.wrapping_add(noise.history);
        v.random = s16(u32::from(noise.value));
        v.previous_random = v.random;
        // The opening LFO step precedes the initial parameters, which read v.lfo.
        v.update_lfo(&view, bank, track);
    }

    pub fn address(&self, v: &mut Voice, prior: &PriorWindow, bank: &SoundBank) {
        let view = self.view(bank);
        v.split = bank.split_for(
            view,
            u32::from(self.index),
            i32::from(v.pitch),
            &mut v.selector,
            v.owner.program,
        );
        let sample = view.sample();
        let bit = u32::from(v.selector & 1) << 28;
        let board = (u32::from(v.selector >> 1) & bank.board_bank_mask) << 20;
        let mut control = if sample.probe() {
            0x0b
        } else {
            bank.rom
                .read_byte(bank.layout.split_mode_table + u32::from(v.split.mode))
        };
        v.reverse = view.reverse() && sample.ordinary();
        if v.reverse {
            control = 0x43;
        }
        let mut start = if v.reverse {
            v.split.accumulator
        } else {
            v.split.start
        };
        let mut phase = if v.reverse {
            v.split.end
        } else {
            v.split.accumulator
        };
        let mut end = v.split.end;
        if sample.phase_aligned() {
            phase &= 0xfffffe00;
        }
        if !sample.table() {
            phase &= 0xffffff00;
        } else {
            let mut d0 = u32::from(view.start_offset());
            let amount_end = d0 + u32::from(view.start_amount_byte());
            let mut d1 = if amount_end < 0xfa { amount_end } else { 0xfa };
            d0 *= 14;
            d1 *= 14;
            let split_table = bank.layout.special_split_table;
            let mut d4 = bank.rom.read_u32(split_table + d0 + 2);
            if d4 == 0x1dcaa07f {
                d0 += 14;
                d4 = bank.rom.read_u32(split_table + d0 + 2);
                if d0 == d1 {
                    d1 += 14;
                }
            }
            let mut d5 = bank.rom.read_u32(split_table + d1 + 2);
            if d5 == 0x1dcaa07f {
                d5 = bank.rom.read_u32(split_table + d1 + SPECIAL_SPLIT_SKIP);
            }
            v.reverse = view.reverse();
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
        v.set_window(bit, board, start, phase, end);
        if sample.ordinary() {
            let mut amount = i32::from(view.start_amount()) * i32::from(v.owner.velocity)
                + (i32::from(view.start_offset()) << 7);
            if amount > 0 {
                let doubled = amount * 2;
                amount = if 32767 < doubled { 32767 } else { doubled };
                let length = i32::from(s16((v.split.end.wrapping_sub(v.split.accumulator)) >> 10));
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
                + if prior.valid {
                    prior.start
                } else {
                    f64::from(bank.reset_start) / 512.0
                };
            v.end = f64::from(board)
                + if prior.valid {
                    prior.end
                } else {
                    f64::from(bank.reset_end) / 512.0
                };
            v.position = f64::from(board)
                + f64::from(bank.banks[usize::from(v.selector)].probe & 0x1fffffff) / 512.0;
        }
    }

    // Legato re-entry of a still-running voice: the new key's pitch becomes the glide destination.
    pub fn retarget(&self, v: &mut Voice, kernel: i32, program: &Program, bank: &SoundBank) {
        let view = self.view(bank);
        v.set_glide(view.pitch_word(kernel), u32::from(program.glide()), bank);
    }

    pub fn step(
        &self,
        v: &mut Voice,
        noise: &mut NoiseState,
        bank: &SoundBank,
        track: &Track,
    ) -> EnvelopeTick {
        let view = self.view(bank);
        let result = self.advance_envelopes(v, bank);
        v.update_lfo(&view, bank, track);
        v.update_random(noise);
        result
    }

    pub fn onset(
        &self,
        v: &mut Voice,
        noise: &mut NoiseState,
        bank: &SoundBank,
        track: &Track,
    ) -> OnsetStep {
        let view = self.view(bank);
        let envelopes = self.advance_envelopes(v, bank);
        if envelopes.ended {
            return OnsetStep {
                ended: true,
                restarted: envelopes.restarted,
                parameters: false,
            };
        }
        v.update_lfo(&view, bank, track);
        v.update_random(noise);
        v.advance_glide();
        self.apply_parameters(v, false, bank, track); // c181da calls c18372 before key-on.
        OnsetStep {
            ended: false,
            restarted: envelopes.restarted,
            parameters: true,
        }
    }

    // Computes and writes the voice's parameter latches (LVOL/RVOL, FC, K1/K2, pair) from the track
    // record and the current envelope/LFO state. Called at key-on and whenever a track event or the
    // ring re-syncs the voice; `initial` also snaps the volume/filter ramps instead of ramping to
    // the new targets.
    pub fn apply_parameters(&self, v: &mut Voice, initial: bool, bank: &SoundBank, track: &Track) {
        let view = self.view(bank);
        let c = &track.config.parameters;
        v.owner.program = track.config.program; // program in force, as the note's channel snapshot held it
        let mut common = i32::from(bank.attenuation[usize::from(c.volume_index())])
            + i32::from(bank.attenuation[usize::from(track.sequence_volume & 127)]);
        common += i32::from(c.expression_offset());
        let amp = view.amplitude_source();
        let mut base = if amp == ModSource::Off {
            254
        } else {
            let amp_source = i32::from(v.modulation(amp, track));
            let x = if amp == ModSource::Lfo {
                (amp_source >> 1) + 0x4000
            } else if 0 < amp_source {
                amp_source
            } else {
                0
            };
            let factor = i32::from(view.amplitude_source_depth());
            let mut value = (x * factor) >> 15;
            if factor >= 0 {
                value -= factor;
            }
            (value + 127) * 2
        };
        base += i32::from(bank.attenuation[usize::from(view.level())])
            + (if view.level_plus_43() { 43 } else { 0 })
            + common
            + i32::from(bank.attenuation[usize::from(v.weight & 127)]);
        let mut pan_value = i32::from(c.signed_pan());
        if pan_value <= -127 {
            pan_value = i32::from(view.default_pan());
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
        let mut first = (filter_env * i32::from(view.k1_envelope())
            + i32::from(v.modulation(view.filter_source(), track))
                * i32::from(view.filter_source_depth()))
            >> 12;
        first += (key * i32::from(view.k1_key())) >> 3;
        first += i32::from(view.k1_base()) * 8;
        v.filter_cutoff_low = bank.coefficient(first, 0);
        let mut second = (filter_env * i32::from(view.k2_envelope())
            + (if view.k2_apply_source() {
                i32::from(v.modulation(view.filter_source(), track))
            } else {
                0
            }) * i32::from(view.filter_source_depth()))
            >> 12;
        second += (key * i32::from(view.k2_key())) >> 3;
        second += i32::from(view.k2_base()) * 8;
        v.filter_cutoff_high = bank.coefficient(second, view.k2_table());
        let pitch_env = i32::from(s16((i32::from(
            v.envelope[Voice::pitch_envelope as usize].level(),
        ) - (i32::from(view.pitch().pitch_zero()) << 8))
            as u32));
        let mut pitch_mod: i64 = i64::from(pitch_env) * i64::from(view.pitch_envelope_depth())
            + i64::from(v.lfo) * i64::from(view.lfo_pitch_depth());
        let depth = i32::from(view.pitch_source_depth());
        let mut multiplier = i32::from(
            bank.rom
                .read_u16(bank.layout.pitch_mod_table + depth.unsigned_abs() * 2),
        );
        if depth < 0 {
            multiplier = -i32::from(s16(multiplier as u32));
        }
        pitch_mod +=
            (i64::from(v.modulation(view.pitch_source(), track)) * i64::from(multiplier)) >> 4;
        let bend = i32::from(s16(
            u32::from(c.bend()).wrapping_sub(TrackParameters::bend_center)
        ));
        pitch_mod >>= 4;
        pitch_mod += i64::from(bend) * i64::from(bank.bend_range);
        pitch_mod >>= 6;
        v.pitch = s16(
            (i32::from(v.glide_target) + i32::from(v.split.tuning) + (pitch_mod as i32)) as u32,
        );
        if (v.split.mode & 128) != 0 {
            let mut amount = (i32::from(view.start_amount())
                * i32::from(v.modulation(view.start_source(), track)))
                >> 7;
            let biased = amount + (i32::from(view.start_offset()) << 8);
            amount = biased.clamp(0, 32767);
            let delta = i32::from(s16(((i64::from(amount)
                * i64::from(s16(v.split.accumulator >> 16)))
                >> 16) as u32));
            let span = ((v.split.end.wrapping_sub(v.split.start)) >> 4) as u16 as i16;
            let offset = u32::from(span as u16).wrapping_mul(u32::from(bank.frequency(delta))) >> 6;
            let start = v.split.end.wrapping_sub(offset) | (u32::from(v.selector & 1) << 28);
            v.start =
                f64::from(u32::from(v.selector >> 1) << 20) + f64::from(start & 0x1fffffe0) / 512.0;
            v.pitch = s16((i32::from(v.pitch) + delta) as u32);
        }
        v.frequency = bank.frequency(i32::from(v.pitch));
        v.finish_parameters(
            initial,
            pair_choice(
                u32::from(c.pair_select()),
                u32::from(view.pair_choice()),
            ),
        );
    }

    // True when the release ends the voice immediately.
    pub fn release(&self, v: &mut Voice, bank: &SoundBank) -> bool {
        for e in v.envelope.iter_mut() {
            if (e.flags & 64) != 0 {
                continue;
            }
            e.done = false;
            e.stage = 4;
            let rate = i32::from(e.rates[3]);
            e.start_segment(0, rate, bank);
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

    fn advance_envelopes(&self, v: &mut Voice, bank: &SoundBank) -> EnvelopeTick {
        let mut result = EnvelopeTick::default();
        for which in [
            Voice::pitch_envelope,
            Voice::filter_envelope,
            Voice::amplitude_envelope,
        ] {
            let tick = self.advance(v, which, bank);
            result.ended |= tick.ended;
            result.restarted |= tick.restarted;
        }
        result
    }

    fn advance(&self, v: &mut Voice, which: u32, bank: &SoundBank) -> EnvelopeTick {
        let view = self.view(bank);
        let e = &mut v.envelope[which as usize];
        if e.done {
            return EnvelopeTick::default();
        }
        if !e.advance() {
            return EnvelopeTick::default();
        }
        e.settle_level();
        if e.stage == 4 {
            e.done = true;
            return EnvelopeTick {
                ended: which == Voice::amplitude_envelope,
                restarted: false,
            };
        }
        if e.intermediate {
            e.intermediate = false;
            let target = u32::from(e.levels[1]);
            let rate = i32::from(e.rates[0]);
            e.start_segment(target, rate, bank);
            return EnvelopeTick::default();
        }
        e.stage = e.stage.wrapping_add(1);
        if e.stage == 1 {
            let target = u32::from(e.levels[2]);
            let rate = i32::from(e.rates[1]);
            e.start_segment(target, rate, bank);
        } else if e.stage == 2 {
            let target = u32::from(e.levels[3]);
            let rate = i32::from(e.rates[2]);
            e.start_segment(target, rate, bank);
        } else if (e.flags & 0x40) != 0 {
            e.stage = 4;
            let rate = i32::from(e.rates[3]);
            e.start_segment(0, rate, bank);
        } else if (e.flags & 0x80) != 0 {
            e.stage = 0;
            let target = u32::from(e.levels[1]);
            let rate = i32::from(e.rates[0]);
            e.start_segment(target, rate, bank);
            if which == Voice::amplitude_envelope && view.sample().ordinary() {
                v.position = if v.reverse {
                    v.end
                } else {
                    v.accumulator_start()
                };
                if v.sample_finished {
                    v.sample_finished = false;
                    return EnvelopeTick {
                        ended: false,
                        restarted: true,
                    };
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

/// One key region of a direct program.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct DirectPatch {
    pub program_offset: usize,
    pub index: u8,
}

impl DirectPatch {
    pub fn new(program_offset: usize, index: u8) -> DirectPatch {
        DirectPatch {
            program_offset,
            index,
        }
    }

    pub fn program<'a>(&self, bank: &'a SoundBank) -> Program<'a> {
        bank.program_at(self.program_offset)
    }

    pub fn view<'a>(&self, bank: &'a SoundBank) -> DirectRegion<'a> {
        self.program(bank).region(u32::from(self.index))
    }

    pub fn contains_key(&self, key: i32, bank: &SoundBank) -> bool {
        self.view(bank).contains(key)
    }

    pub fn allocation(&self) -> AllocationMode {
        AllocationMode::Poly
    }

    pub fn priority(&self) -> u32 {
        64
    }

    pub fn weight(&self) -> i32 {
        127
    }

    pub fn initialize(&self, v: &mut Voice, start: &NoteStart, bank: &SoundBank) {
        let view = self.view(bank);
        let key = i32::from(view.sounding_key(start.kernel, start.transpose));
        let clamped = (key * 256).clamp(0, 0x7f00);
        v.pitch = s16((clamped | (i32::from(view.fine_pitch()) << 4)) as u32);
    }

    pub fn address(&self, v: &mut Voice, bank: &SoundBank) {
        let view = self.view(bank);
        v.split = bank.split_for_region(
            view,
            u32::from(self.index),
            i32::from(v.pitch),
            &mut v.selector,
            v.owner.program,
        );
        let sample = view.sample();
        if sample.id == 0x78 {
            panic!("ROM direct sample78 enters infinite branch c1968e");
        }
        let bit = u32::from(v.selector & 1) << 28;
        let board = (u32::from(v.selector >> 1) & bank.board_bank_mask) << 20;
        let control = bank
            .rom
            .read_byte(bank.layout.split_mode_table + u32::from(v.split.mode));
        v.reverse = view.reverse() && sample.ordinary();
        let final_control = if v.reverse { 0x43 } else { control };
        let start = if v.reverse {
            v.split.accumulator
        } else {
            v.split.start
        };
        let mut phase = if v.reverse {
            v.split.end
        } else {
            v.split.accumulator
        };
        let end = v.split.end;
        if sample.phase_aligned() {
            phase &= 0xfffffe00;
        }
        v.r#loop = (final_control & 8) != 0;
        v.bidirectional = (final_control & 16) != 0;
        v.set_window(bit, board, start, phase, end);
    }

    pub fn step(&self, v: &mut Voice, bank: &SoundBank) -> EnvelopeTick {
        let view = self.view(bank);
        let e = &mut v.envelope[Voice::amplitude_envelope as usize];
        if e.done {
            return EnvelopeTick::default();
        }
        if !e.advance() {
            return EnvelopeTick::default();
        }
        e.settle_level();
        // Timer expiry dispatches on the envelope stage (c1971c -> stage routine), not on note-off:
        // stage 0 starts the release ramp (c1983c: ends the voice if the level's top byte is already
        // 0), stage 4 ends the voice (c19888). A patch whose note-off is ignored (patch[0] bit 7)
        // still decays when its timer expires.
        if e.stage == 4 || (e.position >> 24) == 0 {
            e.done = true;
            return EnvelopeTick {
                ended: true,
                restarted: false,
            };
        }
        let release_rate = u32::from(view.release_rate());
        e.start_direct_release(release_rate, bank);
        EnvelopeTick::default()
    }

    pub fn apply_parameters(&self, v: &mut Voice, initial: bool, bank: &SoundBank, track: &Track) {
        let view = self.view(bank);
        let c = &track.config.parameters;
        v.owner.program = track.config.program;
        let common = i32::from(bank.attenuation[usize::from(c.volume_index())])
            + i32::from(bank.attenuation[usize::from(track.sequence_volume & 127)])
            + i32::from(c.expression_offset());
        let curve = velocity_curve_index(view.velocity_curve(), v.owner.velocity) as i32;
        let sensitivity = velocity_level_scale(view.velocity_level());
        let scale = i32::from(s16((0x4000
            - (128 - i32::from(bank.velocity_curves[curve as usize])) * sensitivity)
            as u32))
            >> 6;
        if initial {
            let e = &mut v.envelope[Voice::amplitude_envelope as usize];
            e.position = (u32::from((i32::from(view.level()) * scale) as u16) << 16) | 0x7fff;
            e.slope = -i32::from(s16(u32::from(
                bank.rom
                    .read_u16(bank.layout.rate_table + 2 * u32::from(view.decay_rate())),
            )));
            e.target = u32::from(((e.position >> 16) ^ 0x8000) as u16) << 16;
        }
        let pan_position = if c.pan() == 128 {
            view.centered_pan()
        } else {
            (i32::from(c.signed_pan()) >> 1) + 64
        };
        let level = i32::from(v.envelope[Voice::amplitude_envelope as usize].level() >> 5)
            + (if view.level_plus_168() { 0xa8 } else { 0 });
        v.left_volume = volume_target(
            (common + i32::from(bank.pan[(127 - pan_position) as usize])) * 4 + level,
            0xfa0,
        );
        v.right_volume = volume_target(
            (common + i32::from(bank.pan[pan_position as usize])) * 4 + level,
            0xfa0,
        );
        let filter_index = direct_filter_index(view.filter_index(), v.owner.velocity);
        v.filter_cutoff_high = bank.coefficient(filter_index, 0);
        v.filter_cutoff_low = v.filter_cutoff_high;
        v.frequency = bank.frequency(i32::from(v.pitch) + i32::from(v.split.tuning));
        v.finish_parameters(
            initial,
            pair_choice(
                u32::from(c.pair_select()),
                u32::from(view.pair_choice()),
            ),
        );
    }

    pub fn release(&self, v: &mut Voice, bank: &SoundBank) -> bool {
        let view = self.view(bank);
        if view.ignore_note_off() {
            return false;
        }
        let release_rate = u32::from(view.release_rate());
        v.envelope[Voice::amplitude_envelope as usize].start_direct_release(release_rate, bank);
        false
    }
}

/// The behavior of one program record; each variant is one record kind.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Patch {
    Layer(LayerPatch),
    Direct(DirectPatch),
}

impl Patch {
    pub fn program<'a>(&self, bank: &'a SoundBank) -> Program<'a> {
        match self {
            Patch::Layer(p) => p.program(bank),
            Patch::Direct(p) => p.program(bank),
        }
    }

    // Layer 0..2, or region 0..16.
    pub fn index(&self) -> u8 {
        match self {
            Patch::Layer(p) => p.index,
            Patch::Direct(p) => p.index,
        }
    }

    pub fn delay_ticks(&self, bank: &SoundBank) -> u8 {
        match self {
            Patch::Layer(p) => p.delay_ticks(bank),
            Patch::Direct(_) => 0,
        }
    }

    pub fn starts_on_release(&self, bank: &SoundBank) -> bool {
        match self {
            Patch::Layer(p) => p.starts_on_release(bank),
            Patch::Direct(_) => false,
        }
    }

    pub fn allocation(&self, bank: &SoundBank) -> AllocationMode {
        match self {
            Patch::Layer(p) => p.allocation(bank),
            Patch::Direct(p) => p.allocation(),
        }
    }

    pub fn priority(&self, bank: &SoundBank) -> u32 {
        match self {
            Patch::Layer(p) => p.priority(bank),
            Patch::Direct(p) => p.priority(),
        }
    }

    // Key weight, or -1 when this note is gated out of the patch.
    pub fn weight(&self, note: &NoteOnEvent, kernel: i32, bank: &SoundBank) -> i32 {
        match self {
            Patch::Layer(p) => p.weight(note, kernel, bank),
            Patch::Direct(p) => p.weight(),
        }
    }

    // Whether an active voice of the same layer slot may be taken over for legato or retrigger.
    pub fn reuses(&self, voice: &Voice) -> bool {
        match self {
            Patch::Layer(p) => p.reuses(voice),
            Patch::Direct(_) => false,
        }
    }

    // Pitch, envelopes and LFO for the new voice; then its split and sample window.
    pub fn initialize(
        &self,
        voice: &mut Voice,
        start: &NoteStart,
        noise: &mut NoiseState,
        bank: &SoundBank,
        track: &Track,
    ) {
        match self {
            Patch::Layer(p) => p.initialize(voice, start, noise, bank, track),
            Patch::Direct(p) => p.initialize(voice, start, bank),
        }
    }

    pub fn address(&self, voice: &mut Voice, prior: &PriorWindow, bank: &SoundBank) {
        match self {
            Patch::Layer(p) => p.address(voice, prior, bank),
            Patch::Direct(p) => p.address(voice, bank),
        }
    }

    // Legato re-entry of a still-running voice: the new key's pitch becomes the glide destination.
    pub fn retarget(&self, voice: &mut Voice, kernel: i32, program: &Program, bank: &SoundBank) {
        if let Patch::Layer(p) = self {
            p.retarget(voice, kernel, program, bank);
        }
    }

    pub fn step(
        &self,
        voice: &mut Voice,
        noise: &mut NoiseState,
        bank: &SoundBank,
        track: &Track,
    ) -> EnvelopeTick {
        match self {
            Patch::Layer(p) => p.step(voice, noise, bank, track),
            Patch::Direct(p) => p.step(voice, bank),
        }
    }

    // A direct voice writes its parameters once and waits for the ring.
    pub fn onset(
        &self,
        voice: &mut Voice,
        noise: &mut NoiseState,
        bank: &SoundBank,
        track: &Track,
    ) -> OnsetStep {
        match self {
            Patch::Layer(p) => p.onset(voice, noise, bank, track),
            Patch::Direct(_) => OnsetStep::default(),
        }
    }

    pub fn apply_parameters(&self, voice: &mut Voice, initial: bool, bank: &SoundBank, track: &Track) {
        match self {
            Patch::Layer(p) => p.apply_parameters(voice, initial, bank, track),
            Patch::Direct(p) => p.apply_parameters(voice, initial, bank, track),
        }
    }

    // True when the release ends the voice immediately.
    pub fn release(&self, voice: &mut Voice, bank: &SoundBank) -> bool {
        match self {
            Patch::Layer(p) => p.release(voice, bank),
            Patch::Direct(p) => p.release(voice, bank),
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
