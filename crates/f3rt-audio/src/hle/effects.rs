//! `audio/hle/effects.hpp`: the substitute for the ES5510 effects programs, its preset/program
//! selection, the OTIS bus routing, the auto-bypass and the LFO phase of bypassed frames.
//!
//! The sound ROM holds a table of program pointers (0xc1c3a2) and a preset table (0xc1c2d2,
//! 16 bytes per preset); byte 5 of a preset row indexes the pointer table and bytes 6..15 are the ten
//! parameters p0..p9 in the order the sound commands set them. A preset loads the program its pointer
//! names; programs are identified here by that pointer, so the selection does not depend on the game.
//! Five programs are decoded (every program the three profiled games Kirameki, Land Maker and Command
//! War select) and each has its own graph; any other program panics instead of being approximated.
//! Delay lengths, gains and topology are the ones of the loaded program as it runs in the reference
//! backend: every delay is a (read offset - write offset) / 256 distance inside the DSP's single
//! 64K-word delay RAM, every gain is a GPR of the program, and each graph reproduces the emulated
//! ES5510 impulse response to about -70 dB re peak (the 16-bit truncation floor), checked by
//! tools/compare/esp_graph.py verify.
//! docs/notes/raw/tests/2026-10-16-kirameki-187-pcb-stereo-reverb.md sections 8 and 9 list the graphs
//! and the parameter mappings, which were read by patching preset rows in a ROM copy and diffing the
//! GPRs. No DSP instructions or sampled audio are stored.
//!
//! The signal processing lives in FAUST (`crates/f3rt-audio/faust/`): es5510.lib holds the all-pass
//! networks, the interpolated tap, the LFO and the shared reverb tank, and reverb.dsp / chorus.dsp /
//! echo.dsp / hall.dsp / dc_blocker.dsp are the programs, each with a signal-flow diagram and
//! descriptive signal-flow names rather than decoded register names. This module keeps what is not
//! signal processing: reading the preset table and the program pointer, mapping p0..p9 to the
//! programs' controls (update()), routing the OTIS buses, the auto-bypass and the LFO phase of
//! bypassed frames.
//!
//!   serio0 (OTIS pair 1, buses 2/3): dry in every program.
//!   serio1 (pair 2, buses 4/5) and serio2 (pair 3, buses 6/7) feed a reverb.
//!   Reverb (0xc1c836, Command War preset 1): serio1 and serio2 go to the reverb and to the output on
//!     the same frame; the reverb tank is mono (nested all-pass blocks around a 1105 frame loop, see
//!     es5510.lib).
//!   Chorus (0xc1d171, Kirameki preset 5): the same tank; serio1 is a 4-tap interpolated stereo
//!     chorus (taps 70/303 L and 261/428 R frames, +-40 frames, two LFOs) with cross-fed feedback, its
//!     output feeds the reverb.
//!   Echo (0xc1d4d4, Land Maker preset 11): the same tank; serio1 is a ping-pong echo (p2 * 112
//!     frames, feedback p1/128) whose left input passes a small-signal enhancer (a saturating cubic of
//!     16 x the input, smoothed, added back); the block A tap reads the left echo line at 256 * p1
//!     frames (G0 holds p1 << 16 | 0x7e after the driver's write).
//!   Hall (0xc1c4e2, Kirameki presets 0 and 2): two cross-coupled stereo-in/stereo-out reverb chains
//!     (a figure of eight): per side a diffuser pair, two nested all-pass blocks and a shared
//!     LFO-modulated tap; each side's output is low-passed and injected into the other side's input
//!     with gain p0/128.
//!   Mute (0xc1ce1b, Kirameki preset 4): the program is a single END, the DAC gets nothing.
//!   DAC L/R = dry + table(127-p8) * serio1 path + table(127-p9) * serio2 + reverb taps.
//!
//! LFOs: the ES5510 LFOs are GPR pairs the microcode rotates every frame, and the DSP executes its
//! whole program every frame whatever the input is (ES5510::run_once() once per frame,
//! runtime/audio/reference/engine.cpp:131), so an LFO phase keeps advancing through silence. Here
//! too: every LFO advances by its rate on every frame, including the frames the auto-bypass does not
//! compute; the phase restarts at (1, 0) only when a program is loaded (select() of a different
//! program) and on reset().

use crate::hle::block::{
    BusBlock, BusSpan, MutableStereoSpan, OtisBus, PlanarAudioBlock, StereoBlock, StereoChannel,
};
use crate::hle::events::SAMPLE_RATE;
use crate::hle::faust::generated::chorus::ChorusDsp;
use crate::hle::faust::generated::dc_blocker::DcBlockerDsp;
use crate::hle::faust::generated::echo::EchoDsp;
use crate::hle::faust::generated::hall::HallDsp;
use crate::hle::faust::generated::reverb::ReverbDsp;
use crate::hle::faust::support::{Controls, FaustDsp, FaustFloat, ParamIndex};
use crate::hle::sound_rom_5a7e9117 as sound_5a7e9117;

const _: () = assert!(
    SAMPLE_RATE == 29762,
    "effect delay lengths are in ES5510 frames (the OTIS/ESP sample rate)"
);

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
#[repr(u8)]
pub enum Program {
    #[default]
    None,
    Reverb,
    Hall,
    Chorus,
    Echo,
    Mute,
}

/// A FAUST control, assigned from update(). The Rust backend hands out a parameter index instead of a
/// `FAUSTFLOAT*` zone, so the write goes through the owning instance.
struct Zone {
    index: ParamIndex,
}

impl Zone {
    fn new(controls: &Controls, label: &str) -> Zone {
        Zone { index: controls.zone(label) }
    }

    fn set<D: FaustDsp<T = FaustFloat>>(&self, dsp: &mut D, value: f32) {
        dsp.set_param(self.index, value);
    }
}

/// Sine LFO of a program. FAUST rotates the GPR pair by eps every frame it computes; the frames it
/// does not compute (auto-bypass) are carried as a phase offset (double, mod 2 pi) that rotates the
/// LFO output. The rate and the offset are doubles (a float rate shifts the phase by up to 8e-5 rad in
/// 30 s, 0.01 frame of delay at the deepest modulation), passed through the float controls as the sum
/// of two floats.
struct Lfo {
    eps_hi: Zone,
    eps_lo: Zone,
    ph_hi: Zone,
    ph_lo: Zone,
    eps: f64,
    phase: f64,
}

impl Lfo {
    fn new(controls: &Controls, name: &str) -> Lfo {
        Lfo {
            eps_hi: Zone::new(controls, &format!("lfo_{name}_eps_hi")),
            eps_lo: Zone::new(controls, &format!("lfo_{name}_eps_lo")),
            ph_hi: Zone::new(controls, &format!("lfo_{name}_ph_hi")),
            ph_lo: Zone::new(controls, &format!("lfo_{name}_ph_lo")),
            eps: 0.0,
            phase: 0.0,
        }
    }

    fn set_rate<D: FaustDsp<T = FaustFloat>>(&mut self, dsp: &mut D, eps: f64) {
        self.eps = eps;
        Lfo::write(dsp, eps, &self.eps_hi, &self.eps_lo);
    }

    /// Frames the DSP did not compute: the LFO rotates by frames * eps.
    fn skip<D: FaustDsp<T = FaustFloat>>(&mut self, dsp: &mut D, frames: u64) {
        self.phase = (self.phase + frames as f64 * self.eps) % (2.0 * std::f64::consts::PI);
        Lfo::write(dsp, self.phase, &self.ph_hi, &self.ph_lo);
    }

    fn restart<D: FaustDsp<T = FaustFloat>>(&mut self, dsp: &mut D) {
        self.phase = 0.0;
        Lfo::write(dsp, 0.0, &self.ph_hi, &self.ph_lo);
    }

    fn write<D: FaustDsp<T = FaustFloat>>(dsp: &mut D, value: f64, hi: &Zone, lo: &Zone) {
        let high = value as f32;
        hi.set(dsp, high);
        lo.set(dsp, (value - high as f64) as f32);
    }
}

/// A FAUST program instance, its control zones and the LFOs it has.
struct Instance<D> {
    dsp: D,
    controls: Controls,
}

impl<D: FaustDsp<T = FaustFloat>> Instance<D> {
    fn new() -> Instance<D> {
        let mut dsp = D::new();
        let controls = Controls::new(&dsp);
        dsp.init(SAMPLE_RATE as i32);
        Instance { dsp, controls }
    }
}

/// Controls every ES5510 program has (p8 / p9 gains, loop decay and damping, LFO C depth).
struct Common<D> {
    instance: Instance<D>,
    chorus_out: Zone,
    dry_send: Zone,
    chorus_to_reverb: Zone,
    send: Zone,
    decay: Zone,
    damp: Zone,
    depth: Zone,
    lfo_c: Lfo,
}

impl<D: FaustDsp<T = FaustFloat>> Common<D> {
    fn new() -> Common<D> {
        let instance = Instance::new();
        let chorus_out = Zone::new(&instance.controls, "chorus_out");
        let dry_send = Zone::new(&instance.controls, "dry_send");
        let chorus_to_reverb = Zone::new(&instance.controls, "chorus_to_reverb");
        let send = Zone::new(&instance.controls, "send");
        let decay = Zone::new(&instance.controls, "decay");
        let damp = Zone::new(&instance.controls, "damp");
        let depth = Zone::new(&instance.controls, "depth");
        let lfo_c = Lfo::new(&instance.controls, "c");
        Common {
            instance,
            chorus_out,
            dry_send,
            chorus_to_reverb,
            send,
            decay,
            damp,
            depth,
            lfo_c,
        }
    }
}

/// The Reverb, Chorus and Echo programs share the reverb tank and its constants.
struct Tank<D> {
    common: Common<D>,
    scale: Zone,
    leak: Zone,
    c1: Zone,
    c2: Zone,
    g: Zone,
    tap_l0: Zone,
    tap_l1: Zone,
    tap_l2: Zone,
    tap_r0: Zone,
    tap_r1: Zone,
    tap_r2: Zone,
}

impl<D: FaustDsp<T = FaustFloat>> Tank<D> {
    fn new() -> Tank<D> {
        let common = Common::new();
        let scale = Zone::new(&common.instance.controls, "scale");
        let leak = Zone::new(&common.instance.controls, "leak");
        let c1 = Zone::new(&common.instance.controls, "c1");
        let c2 = Zone::new(&common.instance.controls, "c2");
        let g = Zone::new(&common.instance.controls, "g");
        let tap_l0 = Zone::new(&common.instance.controls, "tap_l0");
        let tap_l1 = Zone::new(&common.instance.controls, "tap_l1");
        let tap_l2 = Zone::new(&common.instance.controls, "tap_l2");
        let tap_r0 = Zone::new(&common.instance.controls, "tap_r0");
        let tap_r1 = Zone::new(&common.instance.controls, "tap_r1");
        let tap_r2 = Zone::new(&common.instance.controls, "tap_r2");
        Tank {
            common,
            scale,
            leak,
            c1,
            c2,
            g,
            tap_l0,
            tap_l1,
            tap_l2,
            tap_r0,
            tap_r1,
            tap_r2,
        }
    }
}

struct FeedbackProgram<D> {
    tank: Tank<D>,
    feedback: Zone,
    sin_mix: Zone,
    cos_mix: Zone,
}

impl<D: FaustDsp<T = FaustFloat>> FeedbackProgram<D> {
    fn new() -> FeedbackProgram<D> {
        let tank = Tank::new();
        let feedback = Zone::new(&tank.common.instance.controls, "feedback");
        let sin_mix = Zone::new(&tank.common.instance.controls, "sin_mix");
        let cos_mix = Zone::new(&tank.common.instance.controls, "cos_mix");
        FeedbackProgram { tank, feedback, sin_mix, cos_mix }
    }
}

struct ReverbProgram {
    tank: Tank<ReverbDsp>,
}

impl ReverbProgram {
    fn new() -> ReverbProgram {
        ReverbProgram { tank: Tank::new() }
    }
}

struct ChorusProgram {
    feedback: FeedbackProgram<ChorusDsp>,
    lfo_a: Lfo,
    lfo_b: Lfo,
}

impl ChorusProgram {
    fn new() -> ChorusProgram {
        let feedback = FeedbackProgram::new();
        let lfo_a = Lfo::new(&feedback.tank.common.instance.controls, "a");
        let lfo_b = Lfo::new(&feedback.tank.common.instance.controls, "b");
        ChorusProgram { feedback, lfo_a, lfo_b }
    }
}

struct EchoProgram {
    feedback: FeedbackProgram<EchoDsp>,
    echo: Zone,
    mod_center: Zone,
    enhance: Zone,
}

impl EchoProgram {
    fn new() -> EchoProgram {
        let feedback = FeedbackProgram::new();
        let echo = Zone::new(&feedback.tank.common.instance.controls, "echo");
        let mod_center = Zone::new(&feedback.tank.common.instance.controls, "mod_center");
        let enhance = Zone::new(&feedback.tank.common.instance.controls, "enhance");
        EchoProgram { feedback, echo, mod_center, enhance }
    }
}

struct HallProgram {
    common: Common<HallDsp>,
    a: Zone,
    g: Zone,
    g2: Zone,
    g108: Zone,
}

impl HallProgram {
    fn new() -> HallProgram {
        let common = Common::new();
        let a = Zone::new(&common.instance.controls, "hall_a");
        let g = Zone::new(&common.instance.controls, "hall_g");
        let g2 = Zone::new(&common.instance.controls, "hall_g2");
        let g108 = Zone::new(&common.instance.controls, "hall_g108");
        HallProgram { common, a, g, g2, g108 }
    }
}

struct DcBlocker {
    instance: Instance<DcBlockerDsp>,
    gain_l: Zone,
    gain_r: Zone,
    r: Zone,
}

impl DcBlocker {
    fn new() -> DcBlocker {
        let instance = Instance::new();
        let gain_l = Zone::new(&instance.controls, "gain_l");
        let gain_r = Zone::new(&instance.controls, "gain_r");
        let r = Zone::new(&instance.controls, "r");
        DcBlocker { instance, gain_l, gain_r, r }
    }
}

/// Reverb tank constants of the Reverb/Chorus/Echo programs (GPR values of the decoded programs).
#[derive(Clone, Copy, PartialEq, Debug)]
struct TankConstants {
    scale: f32, // input scale: G59 * G4 (Chorus/Echo) or ASR * G4 (Reverb)
    leak: f32,  // G3: one-pole leak of the mono input, gain (1 - leak) * scale
    c1: f32,
    c2: f32, // G108 / 8 and G108 * 0.875: recirculation through the damping poles
    g: f32,  // all-pass gain of the 168/276 pair (G8), (1 - g^2) = G10
    tap_l: [f32; 3], // wet weights of W48 / W68 / W85 at 384/4/26 (L)
    tap_r: [f32; 3], // ... and 1105/242/180 (R)
}

impl Default for TankConstants {
    // The tank constants are the decoded GPR values, not mathematical constants: `scale()` is .7071f in
    // the C++ and must stay that exact float, so the approximate-constant lint does not apply.
    #[allow(clippy::approx_constant)]
    fn default() -> Self {
        TankConstants {
            scale: 0.7071,
            leak: 0.0075,
            c1: 0.00703,
            c2: 0.04922,
            g: 0.4,
            tap_l: [0.13333, 0.66667, 0.46667],
            tap_r: [0.13333, 0.66667, 0.66667],
        }
    }
}

pub struct Effects {
    // ROM pointers of the decoded programs (the same pointer table in all three sound ROMs).
    // The generated classes hold their delay memory inline (about 900 KB for all four), so each
    // instance is allocated once, when the Effects is constructed, and never moves.
    reverb: Box<ReverbProgram>,
    chorus: Box<ChorusProgram>,
    echo_program: Box<EchoProgram>,
    hall: Box<HallProgram>,
    dc: Box<DcBlocker>,

    tank: TankConstants,

    // Parameter-dependent coefficients (update()).
    program: Program,
    decay: f32,
    damp: f32,
    feedback: f32,
    sin_mix: f32,
    cos_mix: f32,
    send: f32,
    dry_send: f32,
    chorus_to_reverb: f32,
    chorus_out: f32,
    echo: f32,
    mod_center: f32,
    enhance: f32,
    hall_a: f32,
    hall_g: f32,
    hall_g2: f32,
    hall_g108: f32,
    lfo_depth: f32,
    eps_a: f64,
    eps_b: f64,
    eps_c: f64, // LFO rates, rad per frame

    // The sound image is owned by the Audio implementation and outlives the Effects.
    rom: &'static [u8],
    parameters: [u8; 10],
    preset: u8,

    bucket_peaks: [f32; K_BUCKET_COUNT],
    bucket_frames: usize,
    bucket_idx: usize,
    current_bucket_peak: f32,
    silent_frames: u64,
    bypassed: bool,
    in_: PlanarAudioBlock<6, K_CHUNK, f32>,
    out: PlanarAudioBlock<3, K_CHUNK, f32>,
    dc_out: PlanarAudioBlock<2, K_CHUNK, f32>,
}

// ROM pointers of the decoded programs (the same pointer table in all three sound ROMs).
pub const K_PROGRAM_TABLE_OFFSET: u32 = 0x1c3a2;
pub const K_PTR_REVERB: u32 = 0xc1c836;
pub const K_PTR_HALL: u32 = 0xc1c4e2;
pub const K_PTR_MUTE: u32 = 0xc1ce1b;
pub const K_PTR_CHORUS: u32 = 0xc1d171;
pub const K_PTR_ECHO: u32 = 0xc1d4d4;

const K_DELAY_BUFFER_SIZE: usize = 96000 * SAMPLE_RATE as usize / 48000;
const K_BUCKET_COUNT: usize = 16;
// 16 * K_BUCKET_SIZE ~ K_DELAY_BUFFER_SIZE
const K_BUCKET_SIZE: usize = 6000 * SAMPLE_RATE as usize / 48000;
// Frames per FAUST compute() call (the caller renders at most 256 at a time).
const K_CHUNK: usize = 256;

// Silence threshold: maximum output gain is Audio::output_boost (12.0) * route (3.2) = 38.4 and the
// wet path gain is bounded by ~3.0, so a line peak below 1e-8 is under 0.02 int16 LSB at the output.
const K_THRESHOLD: f32 = 1e-8;

impl Effects {
    fn table(&self, index: u32) -> f32 {
        let p = sound_5a7e9117::VOLUME_PAN_TABLE_OFFSET as usize
            + index.min(127) as usize * 2;
        (((self.rom[p] as u16) << 8 | self.rom[p + 1] as u16) as f32) / 32768.0
    }

    /// Parameter mapping per program. Every entry was read by patching the preset row in a ROM copy and
    /// diffing the ES5510 GPRs the driver writes (section 9 of the note); GPR formulas are exact unless
    /// marked.
    ///   all programs: p8 -> G124/G125 = table(p8)/table(127-p8), p9 -> G122/G123 = table(p9)/table(127-p9).
    ///   LFO rate G = 196.608 * (p2 - 15) / 2^23 rad per frame (Reverb/Hall G106, Chorus G133; Chorus
    ///     G107 = 296.4 * ...).
    ///   Reverb: p0 -> G110 = p0/128, p1 -> G108 = p1/128, p2 -> LFO C rate, p3 -> LFO depth p3/2 frames,
    ///     p6 high nibble -> G1 = n/16, p7 -> g = p7/128 (all-pass gain G8).
    ///   Hall: p0 -> G110 = p0/128 and a = G5 = (p0 + 7)/128, p1 -> G108 = p1/128, p2 -> LFO rate,
    ///     p3 -> depth, p6 high nibble -> G1, p7 -> g = p7/128 (g2 = G17 = 0.796875 g).
    ///   Chorus: p0 -> G110 = p0/128, p1 -> G134 = p1/128, p2 -> LFO A/B rates, p6 high nibble -> G1,
    ///     p6 low nibble n -> (G118, G119) = (table(8n + 4), table(127 - (8n + 4))).
    ///   Echo (as observed in Land Maker, whose driver never writes G110/G108/G1 after the program
    ///     load): p1 -> G134 and G0 (256 p1 + 0.49 frames), p2 -> echo p2 * 112 frames, p6 low nibble ->
    ///     (G118, G119) as above, p6 high nibble -> G143 = (n^2 + 3) / 256 (the enhancer state decay;
    ///     the enhancer gain is G143 / 4).
    ///   Leak G3 depends on p6's low nibble in a way not decoded; the presets of the profiled games fix
    ///     it (Reverb 0.0703 for p6 = 94, Hall 0.0078 for p6 = 143, Chorus/Echo 0.0075).
    fn update(&mut self) {
        const Q23: f64 = 8388608.0;
        let lfo_rate = |p2: u8, slope: f64| -> f64 {
            let value = slope * (f64::from(p2) - 15.0);
            let limited = if 0.0 < value { value } else { 0.0 };
            limited / Q23
        };
        self.chorus_to_reverb = self.table(self.parameters[8] as u32);
        self.chorus_out = self.table(127u32.wrapping_sub(self.parameters[8] as u32));
        self.send = self.table(self.parameters[9] as u32);
        self.dry_send = self.table(127u32.wrapping_sub(self.parameters[9] as u32));
        let angle = (8 * (self.parameters[6] as u32 & 15) + 4).min(127);
        self.sin_mix = self.table(angle);
        self.cos_mix = self.table(127 - angle);
        let feedback = (self.parameters[1] as i8) as f32 / 128.0;
        self.feedback = feedback.clamp(-0.95, 0.95);
        self.damp = (self.parameters[6] >> 4) as f32 / 16.0;
        self.eps_c = 4864.0 / Q23;
        self.lfo_depth = 8.0;
        match self.program {
            Program::Reverb => {
                self.tank = TankConstants::default();
                self.tank.scale = 0.5;
                self.tank.leak = 0.0703125;
                let g108 = self.parameters[1] as f32 / 128.0;
                self.tank.c1 = g108 / 8.0;
                self.tank.c2 = g108 * 0.875;
                self.tank.g = self.parameters[7] as f32 / 128.0;
                self.tank.tap_l[0] = 0.125;
                self.tank.tap_l[1] = 0.625;
                self.tank.tap_l[2] = 0.4375;
                self.tank.tap_r[0] = 0.125;
                self.tank.tap_r[1] = 0.625;
                self.tank.tap_r[2] = 0.625;
                let value = self.parameters[0] as f32 / 128.0;
                self.decay = if value < 0.9 { value } else { 0.9 };
                self.eps_c = lfo_rate(self.parameters[2], 196.608);
                self.lfo_depth = self.parameters[3] as f32 / 2.0;
            }
            Program::Chorus => {
                self.tank = TankConstants::default();
                let value = self.parameters[0] as f32 / 128.0;
                self.decay = if value < 0.9 { value } else { 0.9 };
                self.eps_a = lfo_rate(self.parameters[2], 196.608);
                self.eps_b = lfo_rate(self.parameters[2], 296.4);
            }
            Program::Echo => {
                self.tank = TankConstants::default();
                self.decay = 0.25;
                self.damp = 0.3125;
                self.echo = self.parameters[2] as f32 * 112.0;
                let feedback = (self.parameters[1] as i8) as f32;
                let clamped = feedback.clamp(0.0, 127.0);
                self.mod_center = clamped * 256.0 + 0.49;
                let nibble = (self.parameters[6] >> 4) as u32;
                self.enhance = ((nibble * nibble + 3) as f32) / 1024.0;
            }
            Program::Hall => {
                let value = self.parameters[0] as f32 / 128.0;
                self.decay = if value < 0.9 { value } else { 0.9 };
                self.hall_a = (u32::from(self.parameters[0]) + 7) as f32 / 128.0;
                self.hall_g108 = self.parameters[1] as f32 / 128.0;
                self.hall_g = self.parameters[7] as f32 / 128.0;
                self.hall_g2 = 0.796875 * self.hall_g;
                self.eps_c = lfo_rate(self.parameters[2], 196.608);
                self.lfo_depth = self.parameters[3] as f32 / 2.0;
            }
            Program::Mute | Program::None => {}
        }
        self.write_controls();
    }

    /// Writes the coefficients update() computed into the zones of the loaded program.
    fn write_controls(&mut self) {
        match self.program {
            Program::Reverb => {
                let reverb = &mut *self.reverb;
                write_tank(
                    &mut reverb.tank,
                    self.chorus_out,
                    self.dry_send,
                    self.chorus_to_reverb,
                    self.send,
                    self.decay,
                    self.damp,
                    self.lfo_depth,
                    self.eps_c,
                    self.tank,
                );
            }
            Program::Chorus => {
                let chorus = &mut *self.chorus;
                write_feedback(
                    &mut chorus.feedback,
                    self.chorus_out,
                    self.dry_send,
                    self.chorus_to_reverb,
                    self.send,
                    self.decay,
                    self.damp,
                    self.lfo_depth,
                    self.eps_c,
                    self.tank,
                    self.feedback,
                    self.sin_mix,
                    self.cos_mix,
                );
                chorus.lfo_a.set_rate(&mut chorus.feedback.tank.common.instance.dsp, self.eps_a);
                chorus.lfo_b.set_rate(&mut chorus.feedback.tank.common.instance.dsp, self.eps_b);
            }
            Program::Echo => {
                let echo_program = &mut *self.echo_program;
                write_feedback(
                    &mut echo_program.feedback,
                    self.chorus_out,
                    self.dry_send,
                    self.chorus_to_reverb,
                    self.send,
                    self.decay,
                    self.damp,
                    self.lfo_depth,
                    self.eps_c,
                    self.tank,
                    self.feedback,
                    self.sin_mix,
                    self.cos_mix,
                );
                let dsp = &mut echo_program.feedback.tank.common.instance.dsp;
                echo_program.echo.set(dsp, self.echo);
                echo_program.mod_center.set(dsp, self.mod_center);
                echo_program.enhance.set(dsp, self.enhance);
            }
            Program::Hall => {
                let hall = &mut *self.hall;
                write_common(
                    &mut hall.common,
                    self.chorus_out,
                    self.dry_send,
                    self.chorus_to_reverb,
                    self.send,
                    self.decay,
                    self.damp,
                    self.lfo_depth,
                    self.eps_c,
                );
                let dsp = &mut hall.common.instance.dsp;
                hall.a.set(dsp, self.hall_a);
                hall.g.set(dsp, self.hall_g);
                hall.g2.set(dsp, self.hall_g2);
                hall.g108.set(dsp, self.hall_g108);
            }
            Program::Mute | Program::None => {}
        }
    }

    /// Output DC blocker (one pole, 20 Hz) written to the dc_blocker.dsp control "r".
    fn init_dc_blocker(&mut self) {
        let value = (-2.0f32 * std::f32::consts::PI * 20.0f32 / SAMPLE_RATE as f32).exp();
        self.dc.r.set(&mut self.dc.instance.dsp, value);
    }

    /// Runs the loaded program over n frames: planar inputs a0l a0r a1l a1r a2l a2r, outputs DAC L,
    /// DAC R, peak.
    fn compute_program(&mut self, n: usize) {
        let inputs: [&[f32]; 6] = [
            self.in_.channel(0),
            self.in_.channel(1),
            self.in_.channel(2),
            self.in_.channel(3),
            self.in_.channel(4),
            self.in_.channel(5),
        ];
        let [out0, out1, out2] = &mut self.out.storage;
        let mut outputs: [&mut [f32]; 3] = [&mut out0[..], &mut out1[..], &mut out2[..]];
        match self.program {
            Program::Reverb => {
                self.reverb.tank.common.instance.dsp.compute(n, &inputs, &mut outputs)
            }
            Program::Chorus => {
                self.chorus.feedback.tank.common.instance.dsp.compute(n, &inputs, &mut outputs)
            }
            Program::Echo => {
                self.echo_program.feedback.tank.common.instance.dsp.compute(n, &inputs, &mut outputs)
            }
            Program::Hall => self.hall.common.instance.dsp.compute(n, &inputs, &mut outputs),
            Program::Mute | Program::None => {}
        }
    }

    /// `m_dc->dsp.compute(int(n), in.data(), dc_out.data())` of the bypass branch: the DC blocker reads
    /// the first two channels of the dry input block (`in` is `m_in`, six channel pointers) and writes
    /// the two output channels.
    fn compute_dc_in(&mut self, n: usize) {
        let inputs: [&[f32]; 6] = [
            self.in_.channel(0),
            self.in_.channel(1),
            self.in_.channel(2),
            self.in_.channel(3),
            self.in_.channel(4),
            self.in_.channel(5),
        ];
        let [dc0, dc1] = &mut self.dc_out.storage;
        let mut outputs: [&mut [f32]; 2] = [&mut dc0[..], &mut dc1[..]];
        self.dc.instance.dsp.compute(n, &inputs, &mut outputs);
    }

    /// `m_dc->dsp.compute(int(n), out.data(), dc_out.data())` of the program loop: the DC blocker reads
    /// the first two channels of the DSP output block (`out` is `m_out`, three channel pointers) and
    /// writes the two output channels.
    fn compute_dc_out(&mut self, n: usize) {
        let inputs: [&[f32]; 3] = [self.out.channel(0), self.out.channel(1), self.out.channel(2)];
        let [dc0, dc1] = &mut self.dc_out.storage;
        let mut outputs: [&mut [f32]; 2] = [&mut dc0[..], &mut dc1[..]];
        self.dc.instance.dsp.compute(n, &inputs, &mut outputs);
    }

    /// Program load and reset: clear the delay memory and state of the DSPs and restart the LFOs at
    /// (1, 0).
    fn clear_program(&mut self, program: Program) {
        match program {
            Program::Reverb => {
                self.reverb.tank.common.instance.dsp.instance_clear();
                self.reverb.tank.common.lfo_c.restart(&mut self.reverb.tank.common.instance.dsp);
            }
            Program::Chorus => {
                self.chorus.feedback.tank.common.instance.dsp.instance_clear();
                self.chorus.lfo_a.restart(&mut self.chorus.feedback.tank.common.instance.dsp);
                self.chorus.lfo_b.restart(&mut self.chorus.feedback.tank.common.instance.dsp);
                self.chorus.feedback.tank.common.lfo_c.restart(
                    &mut self.chorus.feedback.tank.common.instance.dsp,
                );
            }
            Program::Echo => {
                self.echo_program.feedback.tank.common.instance.dsp.instance_clear();
                self.echo_program
                    .feedback
                    .tank
                    .common
                    .lfo_c
                    .restart(&mut self.echo_program.feedback.tank.common.instance.dsp);
            }
            Program::Hall => {
                self.hall.common.instance.dsp.instance_clear();
                self.hall.common.lfo_c.restart(&mut self.hall.common.instance.dsp);
            }
            Program::Mute | Program::None => {}
        }
    }

    /// Frames the loaded program was not computed (auto-bypass): every LFO keeps rotating.
    fn skip_lfos(&mut self, frames: u64) {
        match self.program {
            Program::Reverb => {
                self.reverb.tank.common.lfo_c.skip(&mut self.reverb.tank.common.instance.dsp, frames)
            }
            Program::Chorus => {
                self.chorus.lfo_a.skip(&mut self.chorus.feedback.tank.common.instance.dsp, frames);
                self.chorus.lfo_b.skip(&mut self.chorus.feedback.tank.common.instance.dsp, frames);
                self.chorus
                    .feedback
                    .tank
                    .common
                    .lfo_c
                    .skip(&mut self.chorus.feedback.tank.common.instance.dsp, frames);
            }
            Program::Echo => self.echo_program.feedback.tank.common.lfo_c.skip(
                &mut self.echo_program.feedback.tank.common.instance.dsp,
                frames,
            ),
            Program::Hall => self.hall.common.lfo_c.skip(&mut self.hall.common.instance.dsp, frames),
            Program::Mute | Program::None => {}
        }
    }

    fn rom_u32(&self, offset: usize) -> u32 {
        (self.rom[offset] as u32) << 24
            | (self.rom[offset + 1] as u32) << 16
            | (self.rom[offset + 2] as u32) << 8
            | self.rom[offset + 3] as u32
    }

    pub fn new(rom: &'static [u8]) -> Effects {
        let mut effects = Effects {
            reverb: Box::new(ReverbProgram::new()),
            chorus: Box::new(ChorusProgram::new()),
            echo_program: Box::new(EchoProgram::new()),
            hall: Box::new(HallProgram::new()),
            dc: Box::new(DcBlocker::new()),
            tank: TankConstants::default(),
            program: Program::None,
            decay: 0.25,
            damp: 0.3125,
            feedback: 0.0,
            sin_mix: 0.0,
            cos_mix: 1.0,
            send: 0.2448,
            dry_send: 0.9695,
            chorus_to_reverb: 0.2448,
            chorus_out: 0.9695,
            echo: 7168.0,
            mod_center: 0.49,
            enhance: 0.00683594,
            hall_a: 0.5,
            hall_g: 0.65625,
            hall_g2: 0.52295,
            hall_g108: 0.04689,
            lfo_depth: 8.0,
            eps_a: 0.0,
            eps_b: 0.0,
            eps_c: 0.0,
            rom,
            parameters: [0; 10],
            preset: 0,
            bucket_peaks: [0.0; K_BUCKET_COUNT],
            bucket_frames: 0,
            bucket_idx: 0,
            current_bucket_peak: 0.0,
            silent_frames: K_DELAY_BUFFER_SIZE as u64,
            bypassed: true,
            in_: PlanarAudioBlock::new(),
            out: PlanarAudioBlock::new(),
            dc_out: PlanarAudioBlock::new(),
        };
        effects.init_dc_blocker();
        effects.reset();
        effects
    }

    /// Loads a preset: reads its parameters and the program it names. Panics for a program that has not
    /// been decoded (the DSP would run something this model does not reproduce).
    pub fn select(&mut self, preset: u8) {
        if preset >= 13 {
            return;
        }
        let row = sound_5a7e9117::PRESET_TABLE_OFFSET as usize + preset as usize * 16;
        let pointer =
            self.rom_u32(K_PROGRAM_TABLE_OFFSET as usize + self.rom[row + 5] as usize * 4);
        let program = match pointer {
            K_PTR_REVERB => Program::Reverb,
            K_PTR_HALL => Program::Hall,
            K_PTR_CHORUS => Program::Chorus,
            K_PTR_ECHO => Program::Echo,
            K_PTR_MUTE => Program::Mute,
            _ => panic!(
                "ES5510 program at {} (preset {}) is not decoded",
                pointer, preset
            ),
        };
        // Row bytes 6..15 are p0..p9 in the order the sound commands set them (confirmed by diffing the
        // DSP registers after patching row bytes).
        for n in 0..10 {
            self.parameters[n] = self.rom[row + 6 + n];
        }
        if program != self.program {
            self.clear_program(program);
        }
        self.program = program;
        self.preset = preset;
        self.update();
    }

    pub fn parameter(&mut self, index: u8, value: u8) {
        if (index as usize) < self.parameters.len() {
            self.parameters[index as usize] = value;
            self.update();
        }
    }

    pub fn program(&self) -> Program {
        self.program
    }

    /// True while the auto-bypass skips the loaded program (silent sends and nothing audible left in
    /// it).
    pub fn bypassed(&self) -> bool {
        self.bypassed
    }

    pub fn reset(&mut self) {
        for program in [Program::Reverb, Program::Chorus, Program::Echo, Program::Hall] {
            self.clear_program(program);
        }
        self.bucket_peaks.fill(0.0);
        self.current_bucket_peak = 0.0;
        self.bucket_frames = 0;
        self.silent_frames = K_DELAY_BUFFER_SIZE as u64;
        self.dc.instance.dsp.instance_clear();
        self.bypassed = true;
        self.program = Program::None;
        self.parameters.fill(0);
        self.update();
    }

    /// `mix(BusSpan, MutableStereoSpan, ...)`: the body both `mix` overloads delegate to. Rust has no
    /// method overloading, so the span form is named separately (`mix_span`; `mix` is the
    /// `StereoBlock&` overload, `mix_block` the `BusBlock&` one).
    pub fn mix_span(
        &mut self,
        bus_in: BusSpan,
        stereo_out: MutableStereoSpan,
        input_gain: [f32; 2],
        output_gain: [f32; 2],
    ) {
        let frames = bus_in.size();
        if frames == 0 {
            return;
        }
        if stereo_out.size() < frames {
            panic!("Effects::mix: destination buffer smaller than source");
        }
        if self.program == Program::Mute {
            // A single END: the DSP never writes serio3, the DAC input stays at its last (constant)
            // value.
            let data = stereo_out.data();
            for i in 0..frames * 2 {
                // SAFETY: the destination span holds frames * 2 interleaved floats.
                unsafe { *data.add(i) = 0.0 };
            }
            return;
        }

        let mut silent_sends = true;
        for f in 0..frames {
            if bus_in.at_bus(f, OtisBus::DelayL) != 0.0
                || bus_in.at_bus(f, OtisBus::DelayR) != 0.0
                || bus_in.at_bus(f, OtisBus::ReverbL) != 0.0
                || bus_in.at_bus(f, OtisBus::ReverbR) != 0.0
            {
                silent_sends = false;
                break;
            }
        }

        self.dc.gain_l.set(&mut self.dc.instance.dsp, output_gain[0]);
        self.dc.gain_r.set(&mut self.dc.instance.dsp, output_gain[1]);
        let interleave = |stereo_out: MutableStereoSpan,
                          dc_out: &PlanarAudioBlock<2, K_CHUNK, f32>,
                          offset: usize,
                          n: usize| {
            for f in 0..n {
                stereo_out.set_channel(offset + f, StereoChannel::Left, dc_out.at(0, f));
                stereo_out.set_channel(offset + f, StereoChannel::Right, dc_out.at(1, f));
            }
        };

        if self.bypassed || self.program == Program::None {
            if silent_sends || self.program == Program::None {
                // Nothing audible is left in the program (or none is loaded): only the dry path is
                // heard. The DSP is not computed, but its LFOs keep rotating (see the class comment).
                self.skip_lfos(frames as u64);
                let mut offset = 0;
                while offset < frames {
                    let n = K_CHUNK.min(frames - offset);
                    self.in_.assign_scaled(&bus_in.subspan(offset, n).dry().scaled(input_gain));
                    self.compute_dc_in(n);
                    interleave(stereo_out, &self.dc_out, offset, n);
                    offset += K_CHUNK;
                }
                return;
            }
            self.bypassed = false;
            self.silent_frames = 0;
        }

        if !silent_sends {
            self.silent_frames = 0;
        } else {
            self.silent_frames += frames as u64;
        }

        let mut offset = 0;
        while offset < frames {
            let n = K_CHUNK.min(frames - offset);
            self.in_.assign_scaled(&bus_in.subspan(offset, n).scaled(input_gain));
            self.compute_program(n);
            for f in 0..n {
                let peak = self.out.at(2, f);
                self.current_bucket_peak = if self.current_bucket_peak < peak {
                    peak
                } else {
                    self.current_bucket_peak
                };
                self.bucket_frames += 1;
                if self.bucket_frames >= K_BUCKET_SIZE {
                    self.bucket_peaks[self.bucket_idx] = self.current_bucket_peak;
                    self.bucket_idx = (self.bucket_idx + 1) % K_BUCKET_COUNT;
                    self.current_bucket_peak = 0.0;
                    self.bucket_frames = 0;
                }
            }
            self.compute_dc_out(n);
            interleave(stereo_out, &self.dc_out, offset, n);
            offset += K_CHUNK;
        }

        if self.silent_frames >= K_DELAY_BUFFER_SIZE as u64 {
            let mut peak = self.current_bucket_peak;
            for bucket_peak in self.bucket_peaks {
                peak = if peak < bucket_peak { bucket_peak } else { peak };
            }
            if peak < K_THRESHOLD {
                // Everything the detector watches is under 0.02 LSB: the state is not cleared (it would
                // only drop that residue), the program is just not computed until a send arrives.
                self.bucket_peaks.fill(0.0);
                self.current_bucket_peak = 0.0;
                self.bucket_frames = 0;
                self.bypassed = true;
            }
        }
    }

    pub fn mix_block(
        &mut self,
        buses: &BusBlock,
        stereo: &mut StereoBlock,
        input_gain: [f32; 2],
        output_gain: [f32; 2],
    ) {
        stereo.resize(buses.size());
        self.mix_span(buses.span(), stereo.span_mut(), input_gain, output_gain);
    }

    pub fn mix(
        &mut self,
        buses: BusSpan,
        stereo: &mut StereoBlock,
        input_gain: [f32; 2],
        output_gain: [f32; 2],
    ) {
        stereo.resize(buses.size());
        self.mix_span(buses, stereo.span_mut(), input_gain, output_gain);
    }
}

// The C++ passes the same nine coefficients; a struct for them would be an invented abstraction.
#[allow(clippy::too_many_arguments)]
fn write_common<D: FaustDsp<T = FaustFloat>>(
    p: &mut Common<D>,
    chorus_out: f32,
    dry_send: f32,
    chorus_to_reverb: f32,
    send: f32,
    decay: f32,
    damp: f32,
    depth: f32,
    eps_c: f64,
) {
    p.chorus_out.set(&mut p.instance.dsp, chorus_out);
    p.dry_send.set(&mut p.instance.dsp, dry_send);
    p.chorus_to_reverb.set(&mut p.instance.dsp, chorus_to_reverb);
    p.send.set(&mut p.instance.dsp, send);
    p.decay.set(&mut p.instance.dsp, decay);
    p.damp.set(&mut p.instance.dsp, damp);
    p.depth.set(&mut p.instance.dsp, depth);
    p.lfo_c.set_rate(&mut p.instance.dsp, eps_c);
}

// The C++ passes the same nine coefficients; a struct for them would be an invented abstraction.
#[allow(clippy::too_many_arguments)]
fn write_tank<D: FaustDsp<T = FaustFloat>>(
    p: &mut Tank<D>,
    chorus_out: f32,
    dry_send: f32,
    chorus_to_reverb: f32,
    send: f32,
    decay: f32,
    damp: f32,
    depth: f32,
    eps_c: f64,
    tank: TankConstants,
) {
    write_common(
        &mut p.common,
        chorus_out,
        dry_send,
        chorus_to_reverb,
        send,
        decay,
        damp,
        depth,
        eps_c,
    );
    p.scale.set(&mut p.common.instance.dsp, tank.scale);
    p.leak.set(&mut p.common.instance.dsp, tank.leak);
    p.c1.set(&mut p.common.instance.dsp, tank.c1);
    p.c2.set(&mut p.common.instance.dsp, tank.c2);
    p.g.set(&mut p.common.instance.dsp, tank.g);
    p.tap_l0.set(&mut p.common.instance.dsp, tank.tap_l[0]);
    p.tap_l1.set(&mut p.common.instance.dsp, tank.tap_l[1]);
    p.tap_l2.set(&mut p.common.instance.dsp, tank.tap_l[2]);
    p.tap_r0.set(&mut p.common.instance.dsp, tank.tap_r[0]);
    p.tap_r1.set(&mut p.common.instance.dsp, tank.tap_r[1]);
    p.tap_r2.set(&mut p.common.instance.dsp, tank.tap_r[2]);
}

// The C++ passes the same nine coefficients; a struct for them would be an invented abstraction.
#[allow(clippy::too_many_arguments)]
fn write_feedback<D: FaustDsp<T = FaustFloat>>(
    p: &mut FeedbackProgram<D>,
    chorus_out: f32,
    dry_send: f32,
    chorus_to_reverb: f32,
    send: f32,
    decay: f32,
    damp: f32,
    depth: f32,
    eps_c: f64,
    tank: TankConstants,
    feedback: f32,
    sin_mix: f32,
    cos_mix: f32,
) {
    write_tank(
        &mut p.tank,
        chorus_out,
        dry_send,
        chorus_to_reverb,
        send,
        decay,
        damp,
        depth,
        eps_c,
        tank,
    );
    p.feedback.set(&mut p.tank.common.instance.dsp, feedback);
    p.sin_mix.set(&mut p.tank.common.instance.dsp, sin_mix);
    p.cos_mix.set(&mut p.tank.common.instance.dsp, cos_mix);
}
