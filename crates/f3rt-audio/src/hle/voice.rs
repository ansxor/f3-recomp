//! `audio/hle/voice.hpp`: the voice record, its envelope/filter/gain helpers and the per-tick
//! advancement rules.

#![allow(non_upper_case_globals)]

use crate::bytes::s16;
use crate::hle::events::{NoteOnEvent, SAMPLE_RATE};
use crate::hle::patch::{
    EnvelopePatch, EnvelopeRole, EnvelopeTick, NoteStart, OnsetStep, Patch, PriorWindow, Program,
    velocity_curve_index, velocity_level_scale,
};
use crate::hle::sound_bank::SoundBank;
use crate::hle::sound_rom_5a7e9117::{Split, RATE_TABLE};
use crate::hle::track_service::Track;

pub const NATIVE_RATE: u32 = (15238090 + 8 * 32) / (16 * 32);
pub const RATE_RATIO: f64 = NATIVE_RATE as f64 / SAMPLE_RATE as f64;
pub const VOLUME_RAMP: u32 = SAMPLE_RATE * 4 / 1000;
pub const FILTER_RAMP: u32 = SAMPLE_RATE * 3 / 1000;

pub fn wrap_product(x: i64) -> i32 {
    x as u32 as i32
}

pub fn volume_target(value: i32, threshold: i32) -> u16 {
    let value = i32::from(s16(value.wrapping_sub(threshold) as u32));
    if value <= 0 {
        return 0x0ff0;
    }
    let scaled = value * 48 + 0xff0;
    (if scaled < 0xfff0 { scaled } else { 0xfff0 }) as u16
}

pub const fn make_gain_table() -> [f32; 256] {
    let mut table = [0.0f32; 256];
    let mut i: u32 = 0;
    while i < 256 {
        table[i as usize] =
            ((((i & 15) | 16) << 11) >> (16 - (i >> 4))) as f32 / 2048.0;
        i += 1;
    }
    table
}

pub static GAIN_TABLE: [f32; 256] = make_gain_table();

pub fn interpolated_gain(encoded: f64) -> f32 {
    let scaled = encoded / 256.0;
    let index = scaled.clamp(0.0, 255.0);
    let i = index as u32 as usize;
    let next = if i + 1 < 255 { i + 1 } else { 255 };
    (f64::from(GAIN_TABLE[i])
        + f64::from(GAIN_TABLE[next] - GAIN_TABLE[i]) * (index - i as f64)) as f32
}

#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct GainSegment {
    pub value: f64,
    pub slope: f64,
    pub frames: u32,
}

// gain[] is linear between adjacent entries and ramps move the encoded
// volume by a constant step, so a running sum tracks interpolated_gain (to
// double rounding) until the ramp crosses into the next table interval.
pub fn ramp_gain(encoded: f64, step: f64, s: &mut GainSegment) -> f32 {
    if s.frames != 0 {
        s.frames -= 1;
        s.value += s.slope;
        return s.value as f32;
    }
    let exact = interpolated_gain(encoded);
    let index = encoded / 256.0;
    if index > 0.0 && index < 255.0 {
        let i = index as u32 as usize;
        let per_frame = step / 256.0;
        let delta = f64::from(GAIN_TABLE[i + 1]) - f64::from(GAIN_TABLE[i]);
        s.value = f64::from(GAIN_TABLE[i]) + delta * (index - i as f64);
        s.slope = delta * per_frame;
        let room = if per_frame > 0.0 {
            ((i + 1) as f64 - index) / per_frame
        } else if per_frame < 0.0 {
            (index - i as f64) / -per_frame
        } else {
            1e9
        };
        s.frames = if room > 2.0 { (if room < 1e6 { room } else { 1e6 }) as u32 - 1 } else { 0 };
    }
    exact
}

pub fn interpolate_sample(a: f64, b: f64, frac: f64) -> f32 {
    (a + (b - a) * frac) as f32
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum SampleAdvance {
    Normal,
    Ended,
}

// The C++ takes the same eight parameters; splitting them would invent a struct the translation does
// not have.
#[allow(clippy::too_many_arguments)]
pub fn advance_voice_position(
    pos: &mut f64,
    reverse: &mut bool,
    inc: f64,
    start: f64,
    end: f64,
    length: f64,
    r#loop: bool,
    bidi: bool,
) -> SampleAdvance {
    *pos += if *reverse { -inc } else { inc };
    if !*reverse && *pos > end {
        if !r#loop || length <= 0.0 {
            return SampleAdvance::Ended;
        }
        if bidi {
            *pos = end - (*pos - end);
            *reverse = true;
        } else {
            *pos = start + (*pos - end) % length;
        }
    } else if *reverse && *pos < start {
        if !r#loop || length <= 0.0 {
            return SampleAdvance::Ended;
        }
        if bidi {
            *pos = start + (start - *pos);
            *reverse = false;
        } else {
            *pos = end - (start - *pos) % length;
        }
    }
    SampleAdvance::Normal
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Envelope {
    pub position: u32,
    pub target: u32,
    pub slope: i32,
    pub levels: [u16; 4],
    pub rates: [u8; 4],
    pub flags: u8,
    pub stage: u8,
    pub intermediate: bool,
    pub done: bool,
}

impl Envelope {
    pub fn level(&self) -> i16 {
        s16(self.position >> 16)
    }

    pub fn advance(&mut self) -> bool {
        self.position = self.position.wrapping_add(self.slope as u32);
        self.position.wrapping_sub(self.target) as i32 >= 0
    }
}

#[derive(Clone, Copy, PartialEq, Debug)]
pub struct Filter {
    pub a1: f64,
    pub a2: f64,
    pub target1: f64,
    pub target2: f64,
    pub high_pole: f64,
    pub target_high_pole: f64,
    pub ramp: u32,
    pub mode: u8,
    pub last_k1: u16,
    pub last_k2: u16,
}

impl Default for Filter {
    fn default() -> Filter {
        Filter {
            a1: 1.0,
            a2: 1.0,
            target1: 1.0,
            target2: 1.0,
            high_pole: 1.0,
            target_high_pole: 1.0,
            ramp: 0,
            mode: 3,
            last_k1: 0,
            last_k2: 0,
        }
    }
}

impl Filter {
    pub fn targets(&mut self, k1: u16, k2: u16, initial: bool) {
        if !initial && k1 == self.last_k1 && k2 == self.last_k2 {
            return;
        }
        self.last_k1 = k1;
        self.last_k2 = k2;
        // Preserve native pole positions in continuous time, not device
        // register/clock execution. Quantization masks come from ROM's format.
        self.target1 = 1.0 - (1.0 - f64::from(k1 & 0xfff0) / 65536.0).powf(RATE_RATIO);
        self.target2 = 1.0 - (1.0 - f64::from(k2 & 0xfff0) / 65536.0).powf(RATE_RATIO);
        // Native highpass pole is (1+k/65536)/2. Converting its
        // pole rather than cutoff-Hz guesses keeps low-k differences.
        self.target_high_pole = ((1.0 + f64::from(k2 & 0xfff0) / 65536.0) * 0.5).powf(RATE_RATIO);
        if initial {
            self.a1 = self.target1;
            self.a2 = self.target2;
            self.high_pole = self.target_high_pole;
            self.ramp = 0;
        } else {
            self.ramp = FILTER_RAMP;
        }
    }

    pub fn advance(&mut self) {
        if self.ramp != 0 {
            self.a1 += (self.target1 - self.a1) / f64::from(self.ramp);
            self.a2 += (self.target2 - self.a2) / f64::from(self.ramp);
            self.high_pole += (self.target_high_pole - self.high_pole) / f64::from(self.ramp);
            self.ramp -= 1;
        }
    }
}

// The driver's two shared noise words.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct NoiseState {
    pub value: u16,
    pub history: u16,
}

pub struct Voice {
    pub owner: NoteOnEvent,
    pub patch: *const Patch,
    pub track: *mut Track,
    pub filter_epoch: u32,
    pub split: Split,
    // envelope[] slots, in EnvelopeRole order.
    pub envelope: [Envelope; 3],
    pub filter: Filter,
    pub position: f64,
    pub start: f64,
    pub end: f64,
    pub increment: f64,
    pub left: f64,
    pub right: f64,
    pub target_left: f64,
    pub target_right: f64,
    pub left_gain: f32,
    pub right_gain: f32,
    pub gain_dirty: bool,
    pub serial: u64,
    pub frequency: u16,
    pub left_volume: u16,
    pub right_volume: u16,

    pub filter_cutoff_low: u16, // k1 register
    pub filter_cutoff_high: u16, // k2 register
    pub lfo: i16,
    pub lfo_depth: i16,
    pub lfo_target: i16,
    pub lfo_step: i16,
    pub lfo_phase: u16,
    pub random_countdown: u16,
    pub random_period: u16,
    pub random: i16,
    pub previous_random: i16,
    pub pitch: i16,
    pub glide: i16,
    pub glide_target: i16,
    pub glide_destination: i16,
    pub selector: u8,
    pub pair: u8,
    pub priority: u8,
    pub allocation: u8,
    pub effective_key: u8,
    pub weight: u8,
    pub pressure: u8,
    pub slot: u8,
    pub gain_frames: u32,
    pub step_left: f64,
    pub step_right: f64,
    pub left_segment: GainSegment,
    pub right_segment: GainSegment,
    pub active: bool,
    pub released: bool,
    pub reverse: bool,
    pub r#loop: bool,
    pub bidirectional: bool,
    pub boundary: bool,
    pub sample_finished: bool,
    pub hardware_valid: bool,
    pub release_serial: u64,
}

impl Default for Voice {
    fn default() -> Voice {
        Voice {
            owner: NoteOnEvent::default(),
            patch: core::ptr::null(),
            track: core::ptr::null_mut(),
            filter_epoch: 0,
            split: Split::default(),
            envelope: [Envelope::default(); 3],
            filter: Filter::default(),
            position: 0.0,
            start: 0.0,
            end: 0.0,
            increment: 0.0,
            left: 0.0,
            right: 0.0,
            target_left: 0.0,
            target_right: 0.0,
            left_gain: 0.0,
            right_gain: 0.0,
            gain_dirty: true,
            serial: 0,
            frequency: 0,
            left_volume: 0,
            right_volume: 0,
            filter_cutoff_low: 0,
            filter_cutoff_high: 0,
            lfo: 0,
            lfo_depth: 0,
            lfo_target: 0,
            lfo_step: 0,
            lfo_phase: 0,
            random_countdown: 0,
            random_period: 0,
            random: 0,
            previous_random: 0,
            pitch: 0,
            glide: 0,
            glide_target: 0,
            glide_destination: 0,
            selector: 14,
            pair: 0,
            priority: 0,
            allocation: 0,
            effective_key: 0,
            weight: 127,
            pressure: 0,
            slot: 0,
            gain_frames: 0,
            step_left: 0.0,
            step_right: 0.0,
            left_segment: GainSegment::default(),
            right_segment: GainSegment::default(),
            active: false,
            released: false,
            reverse: false,
            r#loop: false,
            bidirectional: false,
            boundary: false,
            sample_finished: false,
            hardware_valid: false,
            release_serial: 0,
        }
    }
}

impl Voice {
    // envelope[] slots, in EnvelopeRole order.
    pub const pitch_envelope: u32 = EnvelopeRole::Pitch as u32;
    pub const filter_envelope: u32 = EnvelopeRole::Filter as u32;
    pub const amplitude_envelope: u32 = EnvelopeRole::Amplitude as u32;

    // The patch owns the kind-specific behavior; the voice exposes one interface for it.
    pub fn initialize(&mut self, start: &NoteStart, noise: &mut NoiseState, prior: &PriorWindow) {
        unsafe { (*self.patch).initialize(self, start, noise) };
        unsafe { (*self.patch).address(self, prior) };
    }

    pub fn apply_parameters(&mut self, initial: bool) {
        unsafe { (*self.patch).apply_parameters(self, initial) };
    }

    pub fn retarget(&mut self, kernel: i32, program: &Program) {
        unsafe { (*self.patch).retarget(self, kernel, program) };
    }

    pub fn step(&mut self, noise: &mut NoiseState) -> EnvelopeTick {
        unsafe { (*self.patch).step(self, noise) }
    }

    pub fn onset(&mut self, noise: &mut NoiseState) -> OnsetStep {
        unsafe { (*self.patch).onset(self, noise) }
    }

    pub fn release(&mut self) -> bool {
        unsafe { (*self.patch).release(self) }
    }
}

const _: () = assert!(Voice::pitch_envelope == 0);
const _: () = assert!(Voice::filter_envelope == 1);
const _: () = assert!(Voice::amplitude_envelope == 2);

pub fn segment(e: &mut Envelope, target: u32, r: i32, bank: &SoundBank) {
    let difference = i32::from(s16(target.wrapping_sub(e.level() as u16 as u32)));
    if difference != 0 {
        e.slope =
            wrap_product(i64::from(difference) * i64::from(s16(u32::from(bank.rate(r)))) * 2);
        e.position &= 0xffff0000;
    } else {
        e.slope = -i32::from(s16(u32::from(bank.rate(r))));
        e.position = (e.position & 0xffff0000) | 0x7fff;
    }
    e.target = u32::from((target ^ (if e.slope <= 0 { 0x8000 } else { 0 })) as u16) << 16;
}

pub fn initialize_envelope<const ROLE: u8>(
    e: &mut Envelope,
    patch: EnvelopePatch<ROLE>,
    v: &Voice,
    bank: &SoundBank,
) {
    *e = Envelope::default();
    e.flags = patch.flags();
    let index = velocity_curve_index(patch.velocity_curve(), v.owner.velocity);
    let gain_scale = velocity_level_scale(patch.velocity_level());
    let scale = i32::from(s16(
        (0x4000 - (128 - i32::from(bank.velocity_curves[index as usize])) * gain_scale) as u32,
    )) >> 6;
    for i in 0..4u32 {
        e.levels[i as usize] = (i32::from(patch.level(i)) * scale) as u16;
    }
    let key_scale = -(i32::from(s16(
        ((i32::from(v.effective_key) - 64) * i32::from(patch.key_rate()) * 18) as u32,
    )) >> 6);
    let vel_rate = (i32::from(patch.velocity_attack()) * 9 * i32::from(v.owner.velocity)) >> 7;
    e.rates[0] = (if i32::from(patch.rate(0)) - vel_rate < 0 {
        0
    } else if 99 < i32::from(patch.rate(0)) - vel_rate {
        99
    } else {
        i32::from(patch.rate(0)) - vel_rate
    }) as u8;
    e.rates[1] = (if i32::from(patch.rate(1)) + key_scale < 0 {
        0
    } else if 99 < i32::from(patch.rate(1)) + key_scale {
        99
    } else {
        i32::from(patch.rate(1)) + key_scale
    }) as u8;
    e.rates[2] = (if i32::from(patch.rate(2)) + key_scale < 0 {
        0
    } else if 99 < i32::from(patch.rate(2)) + key_scale {
        99
    } else {
        i32::from(patch.rate(2)) + key_scale
    }) as u8;
    e.rates[3] = patch.rate(3);
    e.position = u32::from(e.levels[0]) << 16;
    if ROLE == EnvelopeRole::Amplitude as u8
        && i32::from(s16(
            i32::from(e.levels[1]).wrapping_sub(i32::from(e.levels[0])) as u32,
        )) > 0x3000
        && e.rates[0] > 20
    {
        e.intermediate = true;
        let target = u32::from(e.levels[1].wrapping_sub(0x1400));
        let rate = i32::from(e.rates[0]) - 10;
        segment(e, target, rate, bank);
        return;
    }
    let target = u32::from(e.levels[1]);
    let rate = i32::from(e.rates[0]);
    segment(e, target, rate, bank);
}

pub fn accumulator_start(v: &Voice) -> f64 {
    let address = (v.split.accumulator | (u32::from(v.selector & 1) << 28)) & 0x1fffff00;
    f64::from(u32::from(v.selector >> 1) << 20) + f64::from(address) / 512.0
}

pub fn update_random(v: &mut Voice, noise: &mut NoiseState) {
    v.random_countdown = v.random_countdown.wrapping_sub(1);
    if v.random_countdown != 0 {
        return;
    }
    v.random_countdown = v.random_period;
    noise.history = noise.history.wrapping_add(noise.value);
    noise.value = noise.value.wrapping_add(noise.history);
    v.random = s16(u32::from(noise.value)); // previous_random retains the onset value.
}

pub fn set_glide(v: &mut Voice, destination: i16, rate_index: u32, bank: &SoundBank) {
    v.glide_destination = destination;
    if rate_index == 0 {
        v.glide = 0;
        v.glide_target = destination;
        return;
    }
    let difference = i32::from(destination.wrapping_sub(v.glide_target));
    v.glide = s16(
        ((i64::from(difference)
            * i64::from(s16(u32::from(bank.rom.read_u16(RATE_TABLE + 2 * rate_index))))
            * 2)
            >> 16) as u32,
    );
    if v.glide == 0 {
        v.glide_target = destination;
    }
}

pub fn advance_glide(v: &mut Voice) {
    if v.glide == 0 {
        return;
    }
    let mut current = v.glide_target.wrapping_add(v.glide);
    if (v.glide > 0 && current >= v.glide_destination)
        || (v.glide < 0 && current <= v.glide_destination)
    {
        current = v.glide_destination;
        v.glide = 0;
    }
    v.glide_target = current;
}
