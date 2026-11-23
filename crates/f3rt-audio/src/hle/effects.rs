//! The substitute for the ES5510 effects programs, its preset/program
//! selection, the OTIS bus routing, the auto-bypass and the LFO phase of bypassed frames.
//!
//! The standard driver has a program pointer table at 0xc1c3a2 and a preset table at 0xc1c2d2 (
//! 16 bytes per preset); byte 5 of a preset row indexes the pointer table and bytes 6..15 are the ten
//! parameters p0..p9 in the order the sound commands set them. A preset loads the program its pointer
//! names; programs are identified here by that pointer, so the selection does not depend on the game.
//! Six programs are decoded, including the preset 12 overdrive in Riding Fight and Land Maker.
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
//! echo.dsp / hall.dsp / overdrive.dsp / dc_blocker.dsp are the programs, each with a signal-flow diagram and
//! descriptive signal-flow names rather than decoded register names. This module keeps what is not
//! signal processing: reading the preset table and the program pointer, mapping p0..p9 to the
//! programs' controls (`EffectDsp::apply`), routing the OTIS buses, the auto-bypass and the LFO phase of
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
//! whole program every frame whatever the input is (once per frame), so an LFO phase keeps advancing through silence. Here
//! too: every LFO advances by its rate on every frame, including the frames the auto-bypass does not
//! compute; the phase restarts at (1, 0) only when a program is loaded (select() of a different
//! program) and on reset().

use crate::hle::block::{BusBlock, OtisBus, PlanarAudioBlock, StereoBlock, StereoChannel};
use crate::hle::driver_layout::DriverLayout;
use crate::hle::events::SAMPLE_RATE;
use crate::hle::faust::generated::chorus::ChorusDsp;
use crate::hle::faust::generated::dc_blocker::DcBlockerDsp;
use crate::hle::faust::generated::echo::EchoDsp;
use crate::hle::faust::generated::hall::HallDsp;
use crate::hle::faust::generated::overdrive::OverdriveDsp;
use crate::hle::faust::generated::reverb::ReverbDsp;
use crate::hle::faust::support::{
    Controls, FaustDsp, FaustFloat, OVERDRIVE_CURVE_WORDS, ParamIndex, init_overdrive_rom,
};

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
    Overdrive,
}

// ROM pointers of the decoded programs in the standard driver (the same pointer table in all three
// sound ROMs).
pub const K_PTR_REVERB: u32 = 0xc1c836;
pub const K_PTR_HALL: u32 = 0xc1c4e2;
pub const K_PTR_MUTE: u32 = 0xc1ce1b;
pub const K_PTR_CHORUS: u32 = 0xc1d171;
pub const K_PTR_ECHO: u32 = 0xc1d4d4;
pub const K_PTR_OVERDRIVE: u32 = 0xc1e10b;

const K_DELAY_BUFFER_SIZE: usize = 96000 * SAMPLE_RATE as usize / 48000;
const K_BUCKET_COUNT: usize = 16;
// 16 * K_BUCKET_SIZE ~ K_DELAY_BUFFER_SIZE
const K_BUCKET_SIZE: usize = 6000 * SAMPLE_RATE as usize / 48000;
// Frames per FAUST compute() call (the caller renders at most 256 at a time).
const K_CHUNK: usize = 256;

// Silence threshold: maximum output gain is Audio::output_boost (12.0) * route (3.2) = 38.4 and the
// wet path gain is bounded by ~3.0, so a line peak below 1e-8 is under 0.02 int16 LSB at the output.
const K_THRESHOLD: f32 = 1e-8;

const Q23: f64 = 8388608.0;

/// What a program reads to turn the preset parameters p0..p9 into its controls.
struct EffectContext<'a> {
    rom: &'a [u8],
    layout: &'a DriverLayout,
    parameters: [u8; 10],
}

impl EffectContext<'_> {
    /// Volume/pan table entry as a gain; the index saturates at 127.
    fn table(&self, index: u32) -> f32 {
        let p = self.layout.volume_pan_table_offset as usize + index.min(127) as usize * 2;
        (((self.rom[p] as u16) << 8 | self.rom[p + 1] as u16) as f32) / 32768.0
    }
}

/// The messages every ES5510 program understands.
trait EffectDsp {
    /// Maps the preset parameters onto the program's controls. Every mapping was read by patching the
    /// preset row in a ROM copy and diffing the ES5510 GPRs the driver writes (section 9 of the note);
    /// GPR formulas are exact unless marked.
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
    fn apply(&mut self, ctx: &EffectContext);

    /// Planar inputs a0l a0r a1l a1r a2l a2r, outputs DAC L, DAC R, peak.
    fn compute(&mut self, frames: usize, inputs: &[&[f32]; 6], outputs: &mut [&mut [f32]; 3]);

    /// Program load and reset: clears the delay memory and state and restarts the LFOs at (1, 0).
    fn clear(&mut self);

    /// Frames the program was not computed (auto-bypass): every LFO keeps rotating.
    fn skip_lfos(&mut self, frames: u64);
}

/// A FAUST control. The Rust backend hands out a parameter index instead of a `FAUSTFLOAT*` zone, so
/// the write goes through the owning DSP.
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

/// Stack for building the generated DSPs: they are constructed by value, which in a debug build
/// reserves more than the 2 MiB of a default thread (the echo alone is about 2.3 MiB).
const DSP_BUILD_STACK_BYTES: usize = 8 * 1024 * 1024;

/// Runs `build` on a scoped thread with `DSP_BUILD_STACK_BYTES` of stack, so its result does not
/// depend on how deep the caller's own stack is.
fn build_on_large_stack<T: Send>(build: impl FnOnce() -> T + Send) -> T {
    std::thread::scope(|scope| {
        std::thread::Builder::new()
            .stack_size(DSP_BUILD_STACK_BYTES)
            .spawn_scoped(scope, build)
            .expect("spawn the effects build thread")
            .join()
            .unwrap_or_else(|payload| std::panic::resume_unwind(payload))
    })
}

/// A heap-allocated, initialised DSP with its control registry. Call it only from
/// `build_on_large_stack`, or for a DSP small enough for the caller's stack.
fn new_dsp<D: FaustDsp<T = FaustFloat>>() -> (Box<D>, Controls) {
    let mut dsp = Box::new(D::new());
    let controls = Controls::new(&*dsp);
    dsp.init(SAMPLE_RATE as i32);
    (dsp, controls)
}

fn lfo_rate(p2: u8, slope: f64) -> f64 {
    let value = slope * (f64::from(p2) - 15.0);
    let limited = if 0.0 < value { value } else { 0.0 };
    limited / Q23
}

fn loop_decay(p0: u8) -> f32 {
    let value = p0 as f32 / 128.0;
    if value < 0.9 { value } else { 0.9 }
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

/// Parameters every ES5510 program has (p8 / p9 gains, loop decay and damping, LFO C depth and rate).
#[derive(Clone, Copy, PartialEq, Debug)]
struct CommonParams {
    chorus_out: f32,
    dry_send: f32,
    chorus_to_reverb: f32,
    send: f32,
    decay: f32,
    damp: f32,
    depth: f32,
    eps_c: f64,
}

impl CommonParams {
    fn new(ctx: &EffectContext) -> CommonParams {
        let p = &ctx.parameters;
        CommonParams {
            chorus_out: ctx.table(127u32.wrapping_sub(p[8] as u32)),
            dry_send: ctx.table(127u32.wrapping_sub(p[9] as u32)),
            chorus_to_reverb: ctx.table(p[8] as u32),
            send: ctx.table(p[9] as u32),
            decay: loop_decay(p[0]),
            damp: (p[6] >> 4) as f32 / 16.0,
            depth: 8.0,
            eps_c: 4864.0 / Q23,
        }
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
    // The tank constants are the decoded GPR values, not mathematical constants: 0.7071 must stay
    // that exact float, so the approximate-constant lint does not apply.
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

impl TankConstants {
    fn reverb(ctx: &EffectContext) -> TankConstants {
        let p = &ctx.parameters;
        let g108 = p[1] as f32 / 128.0;
        TankConstants {
            scale: 0.5,
            leak: 0.0703125,
            c1: g108 / 8.0,
            c2: g108 * 0.875,
            g: p[7] as f32 / 128.0,
            tap_l: [0.125, 0.625, 0.4375],
            tap_r: [0.125, 0.625, 0.625],
        }
    }
}

/// Chorus/Echo cross-fed feedback and the p6 low nibble mix angle.
#[derive(Clone, Copy, PartialEq, Debug)]
struct FeedbackParams {
    feedback: f32,
    sin_mix: f32,
    cos_mix: f32,
}

impl FeedbackParams {
    fn new(ctx: &EffectContext) -> FeedbackParams {
        let p = &ctx.parameters;
        let angle = (8 * (p[6] as u32 & 15) + 4).min(127);
        let feedback = (p[1] as i8) as f32 / 128.0;
        FeedbackParams {
            feedback: feedback.clamp(-0.95, 0.95),
            sin_mix: ctx.table(angle),
            cos_mix: ctx.table(127 - angle),
        }
    }
}

/// Zones every program has, and the LFO C they share.
struct CommonControls {
    chorus_out: Zone,
    dry_send: Zone,
    chorus_to_reverb: Zone,
    send: Zone,
    decay: Zone,
    damp: Zone,
    depth: Zone,
    lfo_c: Lfo,
}

impl CommonControls {
    fn new(controls: &Controls) -> CommonControls {
        CommonControls {
            chorus_out: Zone::new(controls, "chorus_out"),
            dry_send: Zone::new(controls, "dry_send"),
            chorus_to_reverb: Zone::new(controls, "chorus_to_reverb"),
            send: Zone::new(controls, "send"),
            decay: Zone::new(controls, "decay"),
            damp: Zone::new(controls, "damp"),
            depth: Zone::new(controls, "depth"),
            lfo_c: Lfo::new(controls, "c"),
        }
    }

    fn apply<D: FaustDsp<T = FaustFloat>>(&mut self, dsp: &mut D, params: &CommonParams) {
        self.chorus_out.set(dsp, params.chorus_out);
        self.dry_send.set(dsp, params.dry_send);
        self.chorus_to_reverb.set(dsp, params.chorus_to_reverb);
        self.send.set(dsp, params.send);
        self.decay.set(dsp, params.decay);
        self.damp.set(dsp, params.damp);
        self.depth.set(dsp, params.depth);
        self.lfo_c.set_rate(dsp, params.eps_c);
    }
}

/// The Reverb, Chorus and Echo programs share the reverb tank and its constants.
struct TankControls {
    scale: Zone,
    leak: Zone,
    c1: Zone,
    c2: Zone,
    g: Zone,
    tap_l: [Zone; 3],
    tap_r: [Zone; 3],
}

impl TankControls {
    fn new(controls: &Controls) -> TankControls {
        TankControls {
            scale: Zone::new(controls, "scale"),
            leak: Zone::new(controls, "leak"),
            c1: Zone::new(controls, "c1"),
            c2: Zone::new(controls, "c2"),
            g: Zone::new(controls, "g"),
            tap_l: [0, 1, 2].map(|i| Zone::new(controls, &format!("tap_l{i}"))),
            tap_r: [0, 1, 2].map(|i| Zone::new(controls, &format!("tap_r{i}"))),
        }
    }

    fn apply<D: FaustDsp<T = FaustFloat>>(&self, dsp: &mut D, tank: &TankConstants) {
        self.scale.set(dsp, tank.scale);
        self.leak.set(dsp, tank.leak);
        self.c1.set(dsp, tank.c1);
        self.c2.set(dsp, tank.c2);
        self.g.set(dsp, tank.g);
        for (zone, value) in self.tap_l.iter().zip(tank.tap_l) {
            zone.set(dsp, value);
        }
        for (zone, value) in self.tap_r.iter().zip(tank.tap_r) {
            zone.set(dsp, value);
        }
    }
}

struct FeedbackControls {
    feedback: Zone,
    sin_mix: Zone,
    cos_mix: Zone,
}

impl FeedbackControls {
    fn new(controls: &Controls) -> FeedbackControls {
        FeedbackControls {
            feedback: Zone::new(controls, "feedback"),
            sin_mix: Zone::new(controls, "sin_mix"),
            cos_mix: Zone::new(controls, "cos_mix"),
        }
    }

    fn apply<D: FaustDsp<T = FaustFloat>>(&self, dsp: &mut D, params: &FeedbackParams) {
        self.feedback.set(dsp, params.feedback);
        self.sin_mix.set(dsp, params.sin_mix);
        self.cos_mix.set(dsp, params.cos_mix);
    }
}

struct ReverbProgram {
    dsp: Box<ReverbDsp>,
    common: CommonControls,
    tank: TankControls,
}

impl ReverbProgram {
    fn new() -> ReverbProgram {
        let (dsp, controls) = new_dsp();
        ReverbProgram {
            dsp,
            common: CommonControls::new(&controls),
            tank: TankControls::new(&controls),
        }
    }
}

impl EffectDsp for ReverbProgram {
    fn apply(&mut self, ctx: &EffectContext) {
        let p = &ctx.parameters;
        let common = CommonParams {
            eps_c: lfo_rate(p[2], 196.608),
            depth: p[3] as f32 / 2.0,
            ..CommonParams::new(ctx)
        };
        self.common.apply(&mut *self.dsp, &common);
        self.tank.apply(&mut *self.dsp, &TankConstants::reverb(ctx));
    }

    fn compute(&mut self, frames: usize, inputs: &[&[f32]; 6], outputs: &mut [&mut [f32]; 3]) {
        self.dsp.compute(frames, inputs, outputs);
    }

    fn clear(&mut self) {
        self.dsp.instance_clear();
        self.common.lfo_c.restart(&mut *self.dsp);
    }

    fn skip_lfos(&mut self, frames: u64) {
        self.common.lfo_c.skip(&mut *self.dsp, frames);
    }
}

struct ChorusProgram {
    dsp: Box<ChorusDsp>,
    common: CommonControls,
    tank: TankControls,
    feedback: FeedbackControls,
    lfo_a: Lfo,
    lfo_b: Lfo,
}

impl ChorusProgram {
    fn new() -> ChorusProgram {
        let (dsp, controls) = new_dsp();
        ChorusProgram {
            dsp,
            common: CommonControls::new(&controls),
            tank: TankControls::new(&controls),
            feedback: FeedbackControls::new(&controls),
            lfo_a: Lfo::new(&controls, "a"),
            lfo_b: Lfo::new(&controls, "b"),
        }
    }
}

impl EffectDsp for ChorusProgram {
    fn apply(&mut self, ctx: &EffectContext) {
        let p2 = ctx.parameters[2];
        self.common.apply(&mut *self.dsp, &CommonParams::new(ctx));
        self.tank.apply(&mut *self.dsp, &TankConstants::default());
        self.feedback.apply(&mut *self.dsp, &FeedbackParams::new(ctx));
        self.lfo_a.set_rate(&mut *self.dsp, lfo_rate(p2, 196.608));
        self.lfo_b.set_rate(&mut *self.dsp, lfo_rate(p2, 296.4));
    }

    fn compute(&mut self, frames: usize, inputs: &[&[f32]; 6], outputs: &mut [&mut [f32]; 3]) {
        self.dsp.compute(frames, inputs, outputs);
    }

    fn clear(&mut self) {
        self.dsp.instance_clear();
        self.lfo_a.restart(&mut *self.dsp);
        self.lfo_b.restart(&mut *self.dsp);
        self.common.lfo_c.restart(&mut *self.dsp);
    }

    fn skip_lfos(&mut self, frames: u64) {
        self.lfo_a.skip(&mut *self.dsp, frames);
        self.lfo_b.skip(&mut *self.dsp, frames);
        self.common.lfo_c.skip(&mut *self.dsp, frames);
    }
}

struct EchoProgram {
    dsp: Box<EchoDsp>,
    common: CommonControls,
    tank: TankControls,
    feedback: FeedbackControls,
    echo: Zone,
    mod_center: Zone,
    enhance: Zone,
}

impl EchoProgram {
    fn new() -> EchoProgram {
        let (dsp, controls) = new_dsp();
        EchoProgram {
            dsp,
            common: CommonControls::new(&controls),
            tank: TankControls::new(&controls),
            feedback: FeedbackControls::new(&controls),
            echo: Zone::new(&controls, "echo"),
            mod_center: Zone::new(&controls, "mod_center"),
            enhance: Zone::new(&controls, "enhance"),
        }
    }
}

impl EffectDsp for EchoProgram {
    fn apply(&mut self, ctx: &EffectContext) {
        let p = &ctx.parameters;
        let common = CommonParams { decay: 0.25, damp: 0.3125, ..CommonParams::new(ctx) };
        self.common.apply(&mut *self.dsp, &common);
        self.tank.apply(&mut *self.dsp, &TankConstants::default());
        self.feedback.apply(&mut *self.dsp, &FeedbackParams::new(ctx));
        let nibble = (p[6] >> 4) as u32;
        let tap = ((p[1] as i8) as f32).clamp(0.0, 127.0);
        self.echo.set(&mut *self.dsp, p[2] as f32 * 112.0);
        self.mod_center.set(&mut *self.dsp, tap * 256.0 + 0.49);
        self.enhance.set(&mut *self.dsp, ((nibble * nibble + 3) as f32) / 1024.0);
    }

    fn compute(&mut self, frames: usize, inputs: &[&[f32]; 6], outputs: &mut [&mut [f32]; 3]) {
        self.dsp.compute(frames, inputs, outputs);
    }

    fn clear(&mut self) {
        self.dsp.instance_clear();
        self.common.lfo_c.restart(&mut *self.dsp);
    }

    fn skip_lfos(&mut self, frames: u64) {
        self.common.lfo_c.skip(&mut *self.dsp, frames);
    }
}

struct HallProgram {
    dsp: Box<HallDsp>,
    common: CommonControls,
    a: Zone,
    g: Zone,
    g2: Zone,
    g108: Zone,
}

impl HallProgram {
    fn new() -> HallProgram {
        let (dsp, controls) = new_dsp();
        HallProgram {
            dsp,
            common: CommonControls::new(&controls),
            a: Zone::new(&controls, "hall_a"),
            g: Zone::new(&controls, "hall_g"),
            g2: Zone::new(&controls, "hall_g2"),
            g108: Zone::new(&controls, "hall_g108"),
        }
    }
}

impl EffectDsp for HallProgram {
    fn apply(&mut self, ctx: &EffectContext) {
        let p = &ctx.parameters;
        let common = CommonParams {
            eps_c: lfo_rate(p[2], 196.608),
            depth: p[3] as f32 / 2.0,
            ..CommonParams::new(ctx)
        };
        let g = p[7] as f32 / 128.0;
        self.common.apply(&mut *self.dsp, &common);
        self.a.set(&mut *self.dsp, (u32::from(p[0]) + 7) as f32 / 128.0);
        self.g.set(&mut *self.dsp, g);
        self.g2.set(&mut *self.dsp, 0.796875 * g);
        self.g108.set(&mut *self.dsp, p[1] as f32 / 128.0);
    }

    fn compute(&mut self, frames: usize, inputs: &[&[f32]; 6], outputs: &mut [&mut [f32]; 3]) {
        self.dsp.compute(frames, inputs, outputs);
    }

    fn clear(&mut self) {
        self.dsp.instance_clear();
        self.common.lfo_c.restart(&mut *self.dsp);
    }

    fn skip_lfos(&mut self, frames: u64) {
        self.common.lfo_c.skip(&mut *self.dsp, frames);
    }
}

struct OverdriveProgram {
    dsp: Box<OverdriveDsp>,
    controls: Controls,
    lfo: Lfo,
}

impl OverdriveProgram {
    /// Builds on a large stack: see `build_on_large_stack`.
    fn new(rom: &[u8], layout: &DriverLayout) -> Box<Self> {
        build_on_large_stack(|| Box::new(Self::build(rom, layout)))
    }

    fn build(rom: &[u8], layout: &DriverLayout) -> Self {
        // The length-prefixed ROM curves follow the pointer table and its trailer.
        // FAUST's array follows DSP address order, with B immediately before A.
        let first = layout.esp_program_table_offset as usize + 0x38;
        let second = first + 2 + 2 * OVERDRIVE_CURVE_WORDS;
        let mut words = [0; 2 * OVERDRIVE_CURVE_WORDS + 1];
        for (offset, range) in [
            (second, 0..OVERDRIVE_CURVE_WORDS),
            (first, OVERDRIVE_CURVE_WORDS..2 * OVERDRIVE_CURVE_WORDS),
        ] {
            let data = &rom[offset + 2..offset + 2 + 2 * OVERDRIVE_CURVE_WORDS];
            for (word, &bytes) in words[range].iter_mut().zip(data.as_chunks::<2>().0) {
                *word = i16::from_be_bytes(bytes);
            }
        }
        let mut dsp = Box::new(OverdriveDsp::new());
        init_overdrive_rom(&mut *dsp, words, SAMPLE_RATE as i32);
        let controls = Controls::new(&*dsp);
        let lfo = Lfo::new(&controls, "c");
        Self { dsp, controls, lfo }
    }
}

impl EffectDsp for OverdriveProgram {
    fn apply(&mut self, ctx: &EffectContext) {
        let p = ctx.parameters;
        let word = |offset: usize| u16::from_be_bytes([ctx.rom[offset], ctx.rom[offset + 1]]);
        let nonnegative = |value: u8| i32::from((value as i8).max(0));
        let pan = |index: i32| {
            f32::from(word(ctx.layout.volume_pan_table_offset as usize + 2 * index as usize))
                / 32768.0
        };
        let square = |value: u8| {
            let n = i32::from(value & 15);
            // The register contains a signed 24-bit coefficient.
            ((n * n + 6) << 24) >> 8
        };
        let decay = nonnegative(p[0]);
        let allpass = (decay - 6).clamp(25, 89);
        let effects = nonnegative(p[8]);
        let reverb = nonnegative(p[9]);
        let rate = word((ctx.layout.phase_rate_table - 0xc00000) as usize + 2 * usize::from(p[2]));
        self.lfo.set_rate(&mut *self.dsp, f64::from(rate) / 8388608.0);
        for (label, value) in [
            ("tank_decay", decay << 16),
            ("allpass", allpass << 16),
            ("allpass_complement", (0x3fffffff - (allpass << 8).pow(2)) >> 7),
            ("short_feedback", i32::from(p[1] as i8) << 16),
            ("envelope_limit", i32::from(p[3] >> 4) << 19),
            ("address_gain", square(p[3])),
            ("tank_damp", i32::from(p[6] >> 4) << 19),
            ("curve_gain", square(p[6])),
            ("filter_pole", i32::from(p[7] >> 4) << 19),
            ("diffusion_pole", i32::from(p[7] & 15) << 19),
        ] {
            self.controls.set(&mut *self.dsp, label, value as f32);
        }
        for (label, value) in [
            ("chorus_out", pan(127 - effects)),
            ("chorus_to_reverb", pan(effects)),
            ("dry_send", pan(127 - reverb)),
            ("send", pan(reverb)),
        ] {
            self.controls.set(&mut *self.dsp, label, value);
        }
    }

    fn compute(&mut self, frames: usize, inputs: &[&[f32]; 6], outputs: &mut [&mut [f32]; 3]) {
        self.dsp.compute(frames, inputs, outputs);
    }

    fn clear(&mut self) {
        self.dsp.instance_clear();
        self.lfo.restart(&mut *self.dsp);
    }

    fn skip_lfos(&mut self, frames: u64) {
        self.lfo.skip(&mut *self.dsp, frames);
    }
}

/// The decoded programs. The generated classes hold their delay memory inline (about 900 KB for all
/// four), so each lives in its own heap allocation and never moves. Overdrive is built on first load because its
/// curves come from the ROM.
struct Programs {
    reverb: ReverbProgram,
    chorus: ChorusProgram,
    echo: EchoProgram,
    hall: HallProgram,
    overdrive: Option<Box<OverdriveProgram>>,
}

impl Programs {
    /// Builds on a large stack: see `build_on_large_stack`.
    fn new() -> Programs {
        build_on_large_stack(|| Programs {
            reverb: ReverbProgram::new(),
            chorus: ChorusProgram::new(),
            echo: EchoProgram::new(),
            hall: HallProgram::new(),
            overdrive: None,
        })
    }

    fn dsp_mut(&mut self, program: Program) -> Option<&mut dyn EffectDsp> {
        match program {
            Program::Reverb => Some(&mut self.reverb),
            Program::Chorus => Some(&mut self.chorus),
            Program::Echo => Some(&mut self.echo),
            Program::Hall => Some(&mut self.hall),
            Program::Overdrive => self.overdrive.as_deref_mut().map(|p| p as &mut dyn EffectDsp),
            Program::Mute | Program::None => None,
        }
    }
}

/// Output DC blocker: one pole, 20 Hz.
struct DcBlocker {
    dsp: Box<DcBlockerDsp>,
    gain_l: Zone,
    gain_r: Zone,
}

impl DcBlocker {
    fn new() -> DcBlocker {
        let (mut dsp, controls) = new_dsp::<DcBlockerDsp>();
        let pole = (-2.0f32 * std::f32::consts::PI * 20.0f32 / SAMPLE_RATE as f32).exp();
        Zone::new(&controls, "r").set(&mut *dsp, pole);
        DcBlocker {
            dsp,
            gain_l: Zone::new(&controls, "gain_l"),
            gain_r: Zone::new(&controls, "gain_r"),
        }
    }

    fn set_gains(&mut self, gain: [f32; 2]) {
        self.gain_l.set(&mut *self.dsp, gain[0]);
        self.gain_r.set(&mut *self.dsp, gain[1]);
    }

    /// Reads the first two channels of `inputs`, writes both channels of `out`.
    fn compute(
        &mut self,
        inputs: &[&[f32]],
        out: &mut PlanarAudioBlock<2, K_CHUNK, f32>,
        frames: usize,
    ) {
        let [left, right] = &mut out.storage;
        let mut outputs: [&mut [f32]; 2] = [&mut left[..], &mut right[..]];
        self.dsp.compute(frames, inputs, &mut outputs);
    }
}

/// Auto-bypass: after a delay-memory length of silent sends, a program whose peak output (line 2 of
/// its output block) stayed below the silence threshold is not computed until a send arrives.
struct BypassDetector {
    bucket_peaks: [f32; K_BUCKET_COUNT],
    bucket_frames: usize,
    bucket_idx: usize,
    current_bucket_peak: f32,
    silent_frames: u64,
    bypassed: bool,
}

impl BypassDetector {
    fn new() -> BypassDetector {
        BypassDetector {
            bucket_peaks: [0.0; K_BUCKET_COUNT],
            bucket_frames: 0,
            bucket_idx: 0,
            current_bucket_peak: 0.0,
            silent_frames: K_DELAY_BUFFER_SIZE as u64,
            bypassed: true,
        }
    }

    fn reset(&mut self) {
        *self = BypassDetector::new();
    }

    fn wake(&mut self) {
        self.bypassed = false;
        self.silent_frames = 0;
    }

    fn is_bypassed(&self) -> bool {
        self.bypassed
    }

    fn record_sends(&mut self, frames: usize, silent: bool) {
        if silent {
            self.silent_frames += frames as u64;
        } else {
            self.silent_frames = 0;
        }
    }

    fn observe_peaks(&mut self, peaks: &[f32]) {
        for &peak in peaks {
            self.current_bucket_peak =
                if self.current_bucket_peak < peak { peak } else { self.current_bucket_peak };
            self.bucket_frames += 1;
            if self.bucket_frames >= K_BUCKET_SIZE {
                self.bucket_peaks[self.bucket_idx] = self.current_bucket_peak;
                self.bucket_idx = (self.bucket_idx + 1) % K_BUCKET_COUNT;
                self.current_bucket_peak = 0.0;
                self.bucket_frames = 0;
            }
        }
    }

    fn update_bypass(&mut self) {
        if self.silent_frames < K_DELAY_BUFFER_SIZE as u64 {
            return;
        }
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

fn sends_silent(buses: &BusBlock, frames: usize) -> bool {
    (0..frames).all(|f| {
        buses.at_bus(f, OtisBus::DelayL) == 0.0
            && buses.at_bus(f, OtisBus::DelayR) == 0.0
            && buses.at_bus(f, OtisBus::ReverbL) == 0.0
            && buses.at_bus(f, OtisBus::ReverbR) == 0.0
    })
}

fn channels<const C: usize>(block: &PlanarAudioBlock<C, K_CHUNK, f32>) -> [&[f32]; C] {
    std::array::from_fn(|c| block.channel(c))
}

pub struct Effects {
    programs: Programs,
    dc: Box<DcBlocker>,
    program: Program,
    bypass: BypassDetector,

    // The sound image is owned by the Audio implementation and outlives the Effects.
    rom: &'static [u8],
    layout: &'static DriverLayout,
    parameters: [u8; 10],
    preset: u8,

    in_: PlanarAudioBlock<6, K_CHUNK, f32>,
    out: PlanarAudioBlock<3, K_CHUNK, f32>,
    dc_out: PlanarAudioBlock<2, K_CHUNK, f32>,
}

impl Effects {
    pub fn new(rom: &'static [u8], layout: &'static DriverLayout) -> Effects {
        let mut effects = Effects {
            programs: Programs::new(),
            dc: Box::new(DcBlocker::new()),
            program: Program::None,
            bypass: BypassDetector::new(),
            rom,
            layout,
            parameters: [0; 10],
            preset: 0,
            in_: PlanarAudioBlock::new(),
            out: PlanarAudioBlock::new(),
            dc_out: PlanarAudioBlock::new(),
        };
        effects.reset();
        effects
    }

    /// Loads a preset and its parameters.
    pub fn select(&mut self, preset: u8) {
        if preset >= 13 {
            return;
        }
        let row = self.layout.preset_table_offset as usize + preset as usize * 16;
        let index = self.rom[row + 5] as usize;
        let pointer = self.rom_u32(self.layout.esp_program_table_offset as usize + index * 4);
        let program = match pointer.wrapping_add(self.layout.esp_pointer_delta) {
            K_PTR_REVERB => Program::Reverb,
            K_PTR_HALL => Program::Hall,
            K_PTR_CHORUS => Program::Chorus,
            K_PTR_ECHO => Program::Echo,
            K_PTR_MUTE => Program::Mute,
            K_PTR_OVERDRIVE => Program::Overdrive,
            _ => panic!(
                "ES5510 program at {} (preset {}) is not decoded",
                pointer, preset
            ),
        };
        // Row bytes 6..15 are p0..p9 in the order the sound commands set them (confirmed by diffing the
        // DSP registers after patching row bytes).
        self.parameters.copy_from_slice(&self.rom[row + 6..row + 16]);
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
        self.bypass.is_bypassed()
    }

    pub fn reset(&mut self) {
        for program in [Program::Reverb, Program::Chorus, Program::Echo, Program::Hall] {
            self.clear_program(program);
        }
        self.programs.overdrive = None;
        self.bypass.reset();
        self.dc.dsp.instance_clear();
        self.program = Program::None;
        self.parameters.fill(0);
        self.update();
    }

    /// Mixes one block of OTIS bus frames into interleaved stereo.
    pub fn mix_block(
        &mut self,
        buses: &BusBlock,
        stereo: &mut StereoBlock,
        input_gain: [f32; 2],
        output_gain: [f32; 2],
    ) {
        let frames = buses.size();
        stereo.resize(frames);
        if frames == 0 {
            return;
        }
        if self.program == Program::Mute {
            // A single END: the DSP never writes serio3, so the DAC input stays silent.
            stereo.active_samples_mut().fill(0.0);
            return;
        }

        let silent_sends = sends_silent(buses, frames);
        self.dc.set_gains(output_gain);

        if self.program == Program::None || (self.bypass.is_bypassed() && silent_sends) {
            self.mix_dry(buses, stereo, input_gain);
            return;
        }
        if self.bypass.is_bypassed() {
            self.bypass.wake();
        }
        self.bypass.record_sends(frames, silent_sends);

        let mut offset = 0;
        while offset < frames {
            let n = K_CHUNK.min(frames - offset);
            self.in_.load_buses(buses, offset, n, input_gain, false);
            self.compute_program(n);
            self.bypass.observe_peaks(&self.out.channel(2)[..n]);
            self.dc.compute(&channels(&self.out), &mut self.dc_out, n);
            self.interleave(stereo, offset, n);
            offset += K_CHUNK;
        }
        self.bypass.update_bypass();
    }

    /// Nothing audible is left in the program (or none is loaded): only the dry path is heard. The DSP
    /// is not computed, but its LFOs keep rotating (see the module comment).
    fn mix_dry(&mut self, buses: &BusBlock, stereo: &mut StereoBlock, input_gain: [f32; 2]) {
        let frames = buses.size();
        self.skip_lfos(frames as u64);
        let mut offset = 0;
        while offset < frames {
            let n = K_CHUNK.min(frames - offset);
            self.in_.load_buses(buses, offset, n, input_gain, true);
            self.dc.compute(&channels(&self.in_), &mut self.dc_out, n);
            self.interleave(stereo, offset, n);
            offset += K_CHUNK;
        }
    }

    fn interleave(&self, stereo: &mut StereoBlock, offset: usize, n: usize) {
        for f in 0..n {
            stereo.set_channel(offset + f, StereoChannel::Left, self.dc_out.at(0, f));
            stereo.set_channel(offset + f, StereoChannel::Right, self.dc_out.at(1, f));
        }
    }

    fn context(&self) -> EffectContext<'static> {
        EffectContext { rom: self.rom, layout: self.layout, parameters: self.parameters }
    }

    fn active_dsp_mut(&mut self) -> Option<&mut dyn EffectDsp> {
        self.programs.dsp_mut(self.program)
    }

    fn update(&mut self) {
        // Curve A can drive the tank with silent sends. Let the level detector
        // observe this state before allowing the DSP to fall asleep again.
        if self.program == Program::Overdrive && self.bypass.is_bypassed() {
            self.bypass.wake();
        }
        let ctx = self.context();
        if let Some(dsp) = self.active_dsp_mut() {
            dsp.apply(&ctx);
        }
    }

    fn compute_program(&mut self, frames: usize) {
        let inputs = channels(&self.in_);
        let [out0, out1, out2] = &mut self.out.storage;
        let mut outputs: [&mut [f32]; 3] = [&mut out0[..], &mut out1[..], &mut out2[..]];
        if let Some(dsp) = self.programs.dsp_mut(self.program) {
            dsp.compute(frames, &inputs, &mut outputs);
        }
    }

    fn clear_program(&mut self, program: Program) {
        if program == Program::Overdrive {
            let (rom, layout) = (self.rom, self.layout);
            self.programs
                .overdrive
                .get_or_insert_with(|| OverdriveProgram::new(rom, layout));
        }
        if let Some(dsp) = self.programs.dsp_mut(program) {
            dsp.clear();
        }
    }

    fn skip_lfos(&mut self, frames: u64) {
        if let Some(dsp) = self.active_dsp_mut() {
            dsp.skip_lfos(frames);
        }
    }

    fn rom_u32(&self, offset: usize) -> u32 {
        u32::from_be_bytes([
            self.rom[offset],
            self.rom[offset + 1],
            self.rom[offset + 2],
            self.rom[offset + 3],
        ])
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::hle::driver_layout::{EARLY, STANDARD};

    fn preset_12_fixture(layout: &DriverLayout) -> &'static [u8] {
        let mut rom = vec![0; 0x20000];
        let presets = layout.preset_table_offset as usize;
        let pointer_table = layout.esp_program_table_offset as usize;
        let row = presets + 12 * 16;
        rom[row + 5] = 10;
        rom[row + 6..row + 16].copy_from_slice(&[77, 252, 1, 183, 37, 0, 83, 10, 64, 20]);
        let pointer = K_PTR_OVERDRIVE - layout.esp_pointer_delta;
        rom[pointer_table + 40..pointer_table + 44].copy_from_slice(&pointer.to_be_bytes());

        let mut set_word = |offset, value: i16| {
            rom[offset..offset + 2].copy_from_slice(&value.to_be_bytes());
        };
        let pan = layout.volume_pan_table_offset as usize;
        for (index, gain) in [(20, 0x1f57), (107, 0x7c1a), (64, 0x5b11), (63, 0x59f2)] {
            set_word(pan + 2 * index, gain);
        }
        set_word((layout.phase_rate_table - 0xc00000) as usize + 2, 41);
        let table = pointer_table + 0x38;
        set_word(table, 130);
        set_word(table + 132, 130);
        for j in 0..65 {
            // Synthetic curves, with the same lengths, origins and controls as the ROM curves.
            set_word(table + 2 + 2 * j, (900 * j as i32 - 29000) as i16);
            set_word(table + 134 + 2 * j, 32767 - 311 * j as i16);
        }
        Box::leak(rom.into_boxed_slice())
    }

    #[test]
    fn preset_12_impulse_matches_reference_in_both_layouts() {
        for layout in [&EARLY, &STANDARD] {
            let mut effects = Effects::new(preset_12_fixture(layout), layout);
            for (warmup, bus, expected) in [
                (0, 2, 0xf0e9cde274c1fd5d),
                (32768, 2, 0xd44c0320d0983c03),
                (0, 4, 0x091d3b5c91a22f9d),
            ] {
                effects.reset();
                effects.select(12);
                assert_eq!(effects.program(), Program::Overdrive);
                assert!(!effects.bypassed());
                for _ in 0..warmup / K_CHUNK {
                    effects.compute_program(K_CHUNK);
                }
                effects.in_.set(bus, 0, 4096.0 / 32768.0);
                // FNV-1a over 8,192 interleaved, little-endian DAC samples from the
                // historical interpreter seeded with the same synthetic tables.
                let mut hash = 0xcbf29ce484222325_u64;
                for _ in 0..8192 / K_CHUNK {
                    effects.compute_program(K_CHUNK);
                    effects.in_.set(bus, 0, 0.0);
                    for frame in 0..K_CHUNK {
                        for ch in 0..2 {
                            let sample = (effects.out.at(ch, frame) * 32768.0) as i16;
                            for byte in sample.to_le_bytes() {
                                hash = (hash ^ u64::from(byte)).wrapping_mul(0x100000001b3);
                            }
                        }
                    }
                }
                assert_eq!(hash, expected, "warmup={warmup}, bus={bus}");
            }
        }
    }

    fn awake() -> BypassDetector {
        let mut detector = BypassDetector::new();
        detector.wake();
        detector
    }

    #[test]
    fn detector_stays_active_while_sends_arrive() {
        let mut detector = awake();
        detector.record_sends(K_DELAY_BUFFER_SIZE, false);
        detector.update_bypass();
        assert!(!detector.is_bypassed());
    }

    #[test]
    fn detector_bypasses_after_a_delay_length_of_silence() {
        let mut detector = awake();
        detector.record_sends(K_DELAY_BUFFER_SIZE - 1, true);
        detector.update_bypass();
        assert!(!detector.is_bypassed());
        detector.record_sends(1, true);
        detector.update_bypass();
        assert!(detector.is_bypassed());
    }

    #[test]
    fn detector_stays_active_while_residue_is_audible() {
        let mut detector = awake();
        detector.observe_peaks(&[K_THRESHOLD]);
        detector.record_sends(K_DELAY_BUFFER_SIZE, true);
        detector.update_bypass();
        assert!(!detector.is_bypassed());

        let mut quiet = awake();
        quiet.observe_peaks(&[K_THRESHOLD / 2.0]);
        quiet.record_sends(K_DELAY_BUFFER_SIZE, true);
        quiet.update_bypass();
        assert!(quiet.is_bypassed());
    }

    #[test]
    fn detector_wakes_on_new_send() {
        let mut detector = BypassDetector::new();
        assert!(detector.is_bypassed());
        detector.wake();
        assert!(!detector.is_bypassed());
        detector.update_bypass();
        assert!(!detector.is_bypassed());
    }

    /// A ROM whose preset 0 loads the reverb.
    fn reverb_fixture(layout: &DriverLayout) -> &'static [u8] {
        let mut rom = vec![0; 0x20000];
        let row = layout.preset_table_offset as usize;
        rom[row + 5] = 0;
        rom[row + 6..row + 16].copy_from_slice(&[0, 0, 15, 0, 0, 0, 0, 0, 0, 0]);
        let pointer = K_PTR_REVERB - layout.esp_pointer_delta;
        let table = layout.esp_program_table_offset as usize;
        rom[table..table + 4].copy_from_slice(&pointer.to_be_bytes());
        // A flat volume/pan table: every send is routed to the DAC at half gain.
        let pan = layout.volume_pan_table_offset as usize;
        for index in 0..128 {
            rom[pan + 2 * index..pan + 2 * index + 2].copy_from_slice(&0x4000u16.to_be_bytes());
        }
        Box::leak(rom.into_boxed_slice())
    }

    fn bus_block(send: f32) -> BusBlock {
        let mut buses = BusBlock::new();
        buses.resize(K_CHUNK);
        buses.set(0, OtisBus::ReverbL.index(), send);
        buses.set(0, OtisBus::ReverbR.index(), send);
        buses
    }

    fn mixed(effects: &mut Effects, buses: &BusBlock) -> StereoBlock {
        let mut stereo = StereoBlock::new();
        effects.mix_block(buses, &mut stereo, [1.0; 2], [1.0; 2]);
        stereo
    }

    fn loudest(stereo: &StereoBlock) -> f32 {
        stereo.samples[..stereo.size() * 2].iter().fold(0.0, |peak, &s| peak.max(s.abs()))
    }

    // Runs on the default test thread: building the effects must not need a larger caller stack.
    #[test]
    fn effects_sleep_after_a_silent_delay_length_and_wake_on_a_send() {
        let mut effects = Effects::new(reverb_fixture(&STANDARD), &STANDARD);
        effects.select(0);
        assert_eq!(effects.program(), Program::Reverb);
        assert!(effects.bypassed());

        let stereo = mixed(&mut effects, &bus_block(0.25));
        assert!(!effects.bypassed());
        assert!(loudest(&stereo) > 0.0);
        assert!(stereo.samples[..K_CHUNK * 2].iter().all(|s| s.is_finite()));

        let silence = bus_block(0.0);
        let mut silent_frames = 0;
        while !effects.bypassed() {
            assert!(silent_frames < 8 * K_DELAY_BUFFER_SIZE, "the reverb never fell asleep");
            let stereo = mixed(&mut effects, &silence);
            assert!(stereo.samples[..K_CHUNK * 2].iter().all(|s| s.is_finite()));
            silent_frames += K_CHUNK;
        }
        assert!(silent_frames >= K_DELAY_BUFFER_SIZE);

        // Asleep: only the DC blocker's residue is left.
        assert!(loudest(&mixed(&mut effects, &silence)) < K_THRESHOLD);
        assert!(effects.bypassed());

        let stereo = mixed(&mut effects, &bus_block(0.25));
        assert!(!effects.bypassed());
        assert!(loudest(&stereo) > 0.0);
        assert!(stereo.samples[..K_CHUNK * 2].iter().all(|s| s.is_finite()));
    }

    #[test]
    fn dry_bus_reaches_the_output_while_the_program_sleeps() {
        let mut effects = Effects::new(reverb_fixture(&STANDARD), &STANDARD);
        effects.select(0);
        let mut buses = BusBlock::new();
        buses.resize(K_CHUNK);
        for frame in 0..K_CHUNK {
            buses.set(frame, OtisBus::DryL.index(), 0.5);
        }
        let stereo = mixed(&mut effects, &buses);
        assert!(effects.bypassed());
        assert!(loudest(&stereo) > 0.0);
    }
}
