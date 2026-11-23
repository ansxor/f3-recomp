#![allow(non_upper_case_globals)]

use crate::bytes::{s8, s16};
use crate::hle::events::{NoteOnEvent, SAMPLE_RATE};
use crate::hle::patch::{
    EnvelopePatch, EnvelopeRole, EnvelopeTick, Layer, ModSource, NoteStart, OnsetStep, Patch,
    PriorWindow, Program, velocity_curve_index, velocity_level_scale,
};
use crate::hle::sound_bank::SoundBank;
use crate::hle::sound_rom::Split;
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
        table[i as usize] = ((((i & 15) | 16) << 11) >> (16 - (i >> 4))) as f32 / 2048.0;
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
    (f64::from(GAIN_TABLE[i]) + f64::from(GAIN_TABLE[next] - GAIN_TABLE[i]) * (index - i as f64))
        as f32
}

#[derive(Clone, Copy, Default, PartialEq, Debug)]
pub struct GainSegment {
    pub value: f64,
    pub slope: f64,
    pub frames: u32,
}

impl GainSegment {
    pub fn step(&mut self, encoded: f64, step: f64) -> f32 {
        if self.frames != 0 {
            self.frames -= 1;
            self.value += self.slope;
            return self.value as f32;
        }
        let exact = interpolated_gain(encoded);
        let index = encoded / 256.0;
        if index > 0.0 && index < 255.0 {
            let i = index as u32 as usize;
            let per_frame = step / 256.0;
            let delta = f64::from(GAIN_TABLE[i + 1]) - f64::from(GAIN_TABLE[i]);
            self.value = f64::from(GAIN_TABLE[i]) + delta * (index - i as f64);
            self.slope = delta * per_frame;
            let room = if per_frame > 0.0 {
                ((i + 1) as f64 - index) / per_frame
            } else if per_frame < 0.0 {
                (index - i as f64) / -per_frame
            } else {
                1e9
            };
            self.frames = if room > 2.0 {
                (if room < 1e6 { room } else { 1e6 }) as u32 - 1
            } else {
                0
            };
        }
        exact
    }
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

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum ParameterUpdate {
    Snap,
    Ramp,
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

    pub fn encode_target(level: u16, slope: i32) -> u32 {
        let encoded = if slope <= 0 { level ^ 0x8000 } else { level };
        u32::from(encoded) << 16
    }

    pub fn decode_target(target: u32, slope: i32) -> u16 {
        let level = (target >> 16) as u16;
        if slope <= 0 { level ^ 0x8000 } else { level }
    }

    pub fn start_segment(&mut self, target: u32, rate_index: i32, bank: &SoundBank) {
        let difference = i32::from(s16(target.wrapping_sub(self.level() as u16 as u32)));
        let rate = s16(u32::from(bank.rate(rate_index)));
        if difference != 0 {
            self.slope = wrap_product(i64::from(difference) * i64::from(rate) * 2);
            self.position &= 0xffff0000;
        } else {
            self.slope = -i32::from(rate);
            self.position = (self.position & 0xffff0000) | 0x7fff;
        }
        self.target = Envelope::encode_target(target as u16, self.slope);
    }

    pub fn initialize<const ROLE: u8>(
        &mut self,
        patch: EnvelopePatch<ROLE>,
        velocity: u8,
        effective_key: u8,
        bank: &SoundBank,
    ) {
        *self = Envelope::default();
        self.flags = patch.flags();
        let index = velocity_curve_index(patch.velocity_curve(), velocity);
        let gain_scale = velocity_level_scale(patch.velocity_level());
        let scale = i32::from(s16((0x4000
            - (128 - i32::from(bank.velocity_curves[index as usize])) * gain_scale)
            as u32))
            >> 6;
        for i in 0..4u32 {
            self.levels[i as usize] = (i32::from(patch.level(i)) * scale) as u16;
        }
        let key_scale = -(i32::from(s16(
            ((i32::from(effective_key) - 64) * i32::from(patch.key_rate()) * 18) as u32,
        )) >> 6);
        let velocity_rate = (i32::from(patch.velocity_attack()) * 9 * i32::from(velocity)) >> 7;
        self.rates[0] = (i32::from(patch.rate(0)) - velocity_rate).clamp(0, 99) as u8;
        self.rates[1] = (i32::from(patch.rate(1)) + key_scale).clamp(0, 99) as u8;
        self.rates[2] = (i32::from(patch.rate(2)) + key_scale).clamp(0, 99) as u8;
        self.rates[3] = patch.rate(3);
        self.position = u32::from(self.levels[0]) << 16;
        let rise = i32::from(s16(
            i32::from(self.levels[1]).wrapping_sub(i32::from(self.levels[0])) as u32,
        ));
        if ROLE == EnvelopeRole::Amplitude as u8 && rise > 0x3000 && self.rates[0] > 20 {
            self.intermediate = true;
            let target = u32::from(self.levels[1].wrapping_sub(0x1400));
            let rate = i32::from(self.rates[0]) - 10;
            self.start_segment(target, rate, bank);
            return;
        }
        let target = u32::from(self.levels[1]);
        let rate = i32::from(self.rates[0]);
        self.start_segment(target, rate, bank);
    }

    pub fn settle_level(&mut self) {
        self.position = u32::from(Envelope::decode_target(self.target, self.slope)) << 16;
    }

    // c1983c: ramp the level to 0 at the direct region's release rate.
    pub fn start_direct_release(&mut self, release_rate: u32, bank: &SoundBank) {
        self.done = false;
        self.position &= 0xffff0000;
        let rate = s16(u32::from(bank.rom.read_u16(bank.layout.rate_table + 2 * release_rate)));
        self.slope = wrap_product(-i64::from(0x7f00) * i64::from(rate) * 2);
        self.target = Envelope::encode_target(0, self.slope);
        self.stage = 4;
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
    pub fn targets(&mut self, k1: u16, k2: u16, mode: ParameterUpdate) {
        let snap = mode == ParameterUpdate::Snap;
        if !snap && k1 == self.last_k1 && k2 == self.last_k2 {
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
        if snap {
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
    pub program: u16,
    pub patch: Option<Patch>,
    pub filter_epoch: u32,
    pub split: Split,
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

    pub filter_cutoff_low: u16,  // k1 register
    pub filter_cutoff_high: u16, // k2 register
    pub lfo: i16,
    pub lfo_depth: i16,
    pub lfo_target: i16,
    pub lfo_step: i16,
    pub lfo_phase: u16,
    pub random_countdown: u16,
    pub random_period: u16,
    pub random: i16,
    pub onset_random: i16,
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
            program: 0,
            patch: None,
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
            onset_random: 0,
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
    pub const pitch_envelope: u32 = EnvelopeRole::Pitch as u32;
    pub const filter_envelope: u32 = EnvelopeRole::Filter as u32;
    pub const amplitude_envelope: u32 = EnvelopeRole::Amplitude as u32;

    fn started_patch(&self) -> Patch {
        self.patch.expect("a voice runs only after its note named a patch")
    }

    pub fn initialize(
        &mut self,
        start: &NoteStart,
        noise: &mut NoiseState,
        prior: &PriorWindow,
        bank: &SoundBank,
        track: &Track,
    ) {
        let patch = self.started_patch();
        patch.initialize(self, start, noise, bank, track);
        patch.address(self, prior, bank);
    }

    pub fn apply_parameters(&mut self, mode: ParameterUpdate, bank: &SoundBank, track: &Track) {
        self.started_patch().apply_parameters(self, mode, bank, track);
    }

    pub fn retarget(&mut self, kernel: i32, program: &Program, bank: &SoundBank) {
        self.started_patch().retarget(self, kernel, program, bank);
    }

    pub fn step(&mut self, noise: &mut NoiseState, bank: &SoundBank, track: &Track) -> EnvelopeTick {
        self.started_patch().step(self, noise, bank, track)
    }

    pub fn onset(&mut self, noise: &mut NoiseState, bank: &SoundBank, track: &Track) -> OnsetStep {
        self.started_patch().onset(self, noise, bank, track)
    }

    pub fn release(&mut self, bank: &SoundBank) -> bool {
        self.started_patch().release(self, bank)
    }

    pub fn end_sample(&mut self) {
        if self.allocation != 0 {
            self.sample_finished = true;
        } else {
            self.stop();
        }
    }

    pub fn stop(&mut self) {
        self.active = false;
    }

    /// Moves the read position one output frame. Ends the sample when it leaves its window without
    /// a loop to wrap into.
    pub fn advance_position(&mut self) -> SampleAdvance {
        let length = self.end - self.start;
        self.position += if self.reverse { -self.increment } else { self.increment };
        if !self.reverse && self.position > self.end {
            if !self.r#loop || length <= 0.0 {
                return SampleAdvance::Ended;
            }
            if self.bidirectional {
                self.position = self.end - (self.position - self.end);
                self.reverse = true;
            } else {
                self.position = self.start + (self.position - self.end) % length;
            }
        } else if self.reverse && self.position < self.start {
            if !self.r#loop || length <= 0.0 {
                return SampleAdvance::Ended;
            }
            if self.bidirectional {
                self.position = self.start + (self.start - self.position);
                self.reverse = false;
            } else {
                self.position = self.end - (self.start - self.position) % length;
            }
        }
        SampleAdvance::Normal
    }

    pub fn accumulator_start(&self) -> f64 {
        let address = (self.split.accumulator | (u32::from(self.selector & 1) << 28)) & 0x1fffff00;
        f64::from(u32::from(self.selector >> 1) << 20) + f64::from(address) / 512.0
    }

    pub fn update_random(&mut self, noise: &mut NoiseState) {
        self.random_countdown = self.random_countdown.wrapping_sub(1);
        if self.random_countdown != 0 {
            return;
        }
        self.random_countdown = self.random_period;
        noise.history = noise.history.wrapping_add(noise.value);
        noise.value = noise.value.wrapping_add(noise.history);
        self.random = s16(u32::from(noise.value));
    }

    pub fn set_glide(&mut self, destination: i16, rate_index: u32, bank: &SoundBank) {
        self.glide_destination = destination;
        if rate_index == 0 {
            self.glide = 0;
            self.glide_target = destination;
            return;
        }
        let difference = i32::from(destination.wrapping_sub(self.glide_target));
        let rate = s16(u32::from(bank.rom.read_u16(bank.layout.rate_table + 2 * rate_index)));
        self.glide = s16(((i64::from(difference) * i64::from(rate) * 2) >> 16) as u32);
        if self.glide == 0 {
            self.glide_target = destination;
        }
    }

    pub fn advance_glide(&mut self) {
        if self.glide == 0 {
            return;
        }
        let mut current = self.glide_target.wrapping_add(self.glide);
        if (self.glide > 0 && current >= self.glide_destination)
            || (self.glide < 0 && current <= self.glide_destination)
        {
            current = self.glide_destination;
            self.glide = 0;
        }
        self.glide_target = current;
    }

    pub fn modulation(&self, selector: ModSource, track: &Track) -> i16 {
        let parameters = &track.config.parameters;
        match selector {
            ModSource::Lfo => self.lfo,
            ModSource::PitchEnvelope => self.envelope[Voice::pitch_envelope as usize].level(),
            ModSource::FilterEnvelope => self.envelope[Voice::filter_envelope as usize].level(),
            ModSource::AmplitudeEnvelope => self.envelope[Voice::amplitude_envelope as usize].level(),
            ModSource::Random => self.random,
            ModSource::PreviousRandom => self.onset_random,
            ModSource::Velocity => s16(u32::from(self.owner.velocity) * 257),
            ModSource::Key => s16(((i32::from(self.effective_key) - 60) * 256) as u32),
            ModSource::Mod1A => s16(u32::from(parameters.mod_1a())),
            ModSource::Mod12 => s16(u32::from(parameters.mod_12())),
            ModSource::Bend => s16(u32::from(parameters.bend())),
            ModSource::Mod10 => s16(u32::from(parameters.mod_10())),
            ModSource::Mod0C => s16(u32::from(parameters.mod_0c())),
            ModSource::Pressure => s16(u32::from(self.pressure) * 256),
            ModSource::Full => 0x7fff,
            ModSource::Off => 0,
        }
    }

    pub fn update_lfo(&mut self, layer: &Layer, bank: &SoundBank, track: &Track) {
        self.lfo_depth = s16((i32::from(self.lfo_depth) + i32::from(self.lfo_step)) as u32);
        if (self.lfo_depth as u16) > (self.lfo_target as u16) {
            self.lfo_step = 0;
            self.lfo_depth = self.lfo_target;
        }
        self.lfo_phase = self
            .lfo_phase
            .wrapping_add(bank.phase_rates[usize::from(layer.lfo_rate())]);
        let wave = (u32::from(layer.lfo_wave()) << 8) | u32::from(self.lfo_phase >> 8);
        let selected = i32::from(self.lfo_depth) + i32::from(self.modulation(layer.lfo_source(), track));
        let depth = selected.clamp(0, 32767);
        let shape = i32::from(s8(u32::from(bank.rom.read_byte(bank.layout.lfo_wave_table + wave))));
        self.lfo = s16(((depth * shape) >> 7) as u32);
    }

    // Split addresses are word offsets inside the sample bank the selector named.
    pub fn set_window(&mut self, bit: u32, board: u32, start: u32, phase: u32, end: u32) {
        self.start = f64::from(board) + f64::from((start | bit) & 0x1fffffe0) / 512.0;
        self.end = f64::from(board) + f64::from((end | bit) & 0x1fffffe0) / 512.0;
        self.position = f64::from(board) + f64::from((phase | bit) & 0x1fffffff) / 512.0;
    }

    pub fn finish_parameters(&mut self, mode: ParameterUpdate, pair: u32) {
        self.increment =
            f64::from(self.frequency) * f64::from(NATIVE_RATE) / (1024.0 * f64::from(SAMPLE_RATE));
        self.pair = ((pair + 1) & 3) as u8;
        self.target_left = f64::from(self.left_volume);
        self.target_right = f64::from(self.right_volume);
        if mode == ParameterUpdate::Snap {
            self.left = self.target_left;
            self.right = self.target_right;
            self.gain_frames = 0;
        } else {
            self.gain_frames = if self.left == self.target_left && self.right == self.target_right {
                0
            } else {
                VOLUME_RAMP
            };
        }
        if self.gain_frames != 0 {
            self.step_left = (self.target_left - self.left) / f64::from(self.gain_frames);
            self.step_right = (self.target_right - self.right) / f64::from(self.gain_frames);
        }
        self.left_segment.frames = 0;
        self.right_segment.frames = 0;
        self.gain_dirty = true;
        self.filter
            .targets(self.filter_cutoff_low, self.filter_cutoff_high, mode);
    }
}

const _: () = assert!(Voice::pitch_envelope == 0);
const _: () = assert!(Voice::filter_envelope == 1);
const _: () = assert!(Voice::amplitude_envelope == 2);

#[cfg(test)]
mod tests {
    use super::*;

    fn window(start: f64, end: f64, position: f64, increment: f64) -> Voice {
        Voice {
            start,
            end,
            position,
            increment,
            ..Voice::default()
        }
    }

    #[test]
    fn one_shot_ends_when_it_passes_the_window_end() {
        let mut voice = window(0.0, 10.0, 0.0, 4.0);
        assert_eq!(voice.advance_position(), SampleAdvance::Normal);
        assert_eq!(voice.advance_position(), SampleAdvance::Normal);
        assert_eq!(voice.position, 8.0);
        assert_eq!(voice.advance_position(), SampleAdvance::Ended);
    }

    #[test]
    fn forward_loop_wraps_the_overshoot_into_the_window() {
        let mut voice = window(2.0, 10.0, 8.0, 4.0);
        voice.r#loop = true;
        assert_eq!(voice.advance_position(), SampleAdvance::Normal);
        assert_eq!(voice.position, 4.0);
        assert!(!voice.reverse);
    }

    #[test]
    fn bidirectional_loop_reflects_at_both_edges() {
        let mut voice = window(2.0, 10.0, 8.0, 4.0);
        voice.r#loop = true;
        voice.bidirectional = true;
        assert_eq!(voice.advance_position(), SampleAdvance::Normal);
        assert_eq!((voice.position, voice.reverse), (8.0, true));
        assert_eq!(voice.advance_position(), SampleAdvance::Normal);
        assert_eq!((voice.position, voice.reverse), (4.0, true));
        assert_eq!(voice.advance_position(), SampleAdvance::Normal);
        assert_eq!((voice.position, voice.reverse), (4.0, false));
    }

    #[test]
    fn reverse_one_shot_ends_below_the_window_start() {
        let mut voice = window(2.0, 10.0, 7.0, 4.0);
        voice.reverse = true;
        assert_eq!(voice.advance_position(), SampleAdvance::Normal);
        assert_eq!(voice.position, 3.0);
        assert_eq!(voice.advance_position(), SampleAdvance::Ended);
    }

    #[test]
    fn reverse_loop_wraps_from_the_window_end() {
        let mut voice = window(2.0, 10.0, 3.0, 4.0);
        voice.reverse = true;
        voice.r#loop = true;
        assert_eq!(voice.advance_position(), SampleAdvance::Normal);
        assert_eq!(voice.position, 7.0);
    }

    #[test]
    fn empty_loop_window_ends_the_sample() {
        let mut voice = window(5.0, 5.0, 5.0, 1.0);
        voice.r#loop = true;
        assert_eq!(voice.advance_position(), SampleAdvance::Ended);
    }

    #[test]
    fn envelope_advance_reports_when_the_target_is_reached() {
        let mut envelope = Envelope {
            position: 0x1000_0000,
            slope: 0x0800_0000,
            target: 0x2000_0000,
            ..Envelope::default()
        };
        assert!(!envelope.advance());
        assert!(envelope.advance());
        assert_eq!(envelope.position, 0x2000_0000);
    }

    #[test]
    fn envelope_advance_compares_across_the_position_wrap() {
        let mut envelope = Envelope {
            position: 0xf000_0000,
            slope: 0x2000_0000,
            target: 0xf800_0000,
            ..Envelope::default()
        };
        assert!(envelope.advance());
        assert_eq!(envelope.position, 0x1000_0000);
    }

    #[test]
    fn envelope_level_is_the_signed_top_half_of_the_position() {
        let envelope = Envelope {
            position: 0x8000_1234,
            ..Envelope::default()
        };
        assert_eq!(envelope.level(), i16::MIN);
    }

    #[test]
    fn gain_segment_replays_its_slope_while_frames_remain() {
        let mut segment = GainSegment {
            value: 1.0,
            slope: 0.5,
            frames: 2,
        };
        assert_eq!(segment.step(0.0, 0.0), 1.5);
        assert_eq!(segment.step(0.0, 0.0), 2.0);
        assert_eq!(segment.frames, 0);
    }

    #[test]
    fn gain_segment_arms_a_linear_run_inside_a_table_interval() {
        let mut segment = GainSegment::default();
        let exact = segment.step(200.5 * 256.0, 16.0);
        assert_eq!(exact, 1.53125);
        assert_eq!(segment.value, 1.53125);
        assert_eq!(segment.slope, 0.00390625);
        assert_eq!(segment.frames, 7);
        assert_eq!(segment.step(0.0, 0.0), 1.535_156_3);
        assert_eq!(segment.frames, 6);
    }

    #[test]
    fn gain_segment_outside_the_table_interior_has_no_run() {
        let mut segment = GainSegment::default();
        segment.step(0.0, 16.0);
        assert_eq!(segment.frames, 0);
    }
}
