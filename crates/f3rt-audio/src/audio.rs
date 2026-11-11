//! `f3rt/audio.hpp` + `audio.cpp`: the F3 sound board, driven by main-CPU mailbox writes and rendered
//! on a synth worker thread. Output is interleaved stereo (Left, Right) at `sample_rate()`.

use std::sync::atomic::{AtomicU64, Ordering};
use std::time::Instant;

use crate::hle::command::CommandPacket;
use crate::hle::core::{self, Core, PcmSink, RenderStats};
use crate::hle::events::SAMPLE_RATE;
use crate::memory_map::{MAILBOX_GAIN_CH0, MAILBOX_GAIN_CH1, MAILBOX_PRODUCER_L};
use crate::pcm_ring::PcmRing;
use crate::shared_mailbox::SharedMailbox;
use crate::snapshot::AudioSnapshot;
use crate::worker_queue::{Applies, WorkerQueue};

pub const QUEUE_SIZE: usize = 4096;

/// Raw big-endian 16-bit sample ROM bytes to host words.
pub fn decode_sample_words(sample_rom: &[u8]) -> Vec<u16> {
    let mut words = vec![0u16; sample_rom.len() / 2];
    for (i, word) in words.iter_mut().enumerate() {
        *word = u16::from_be_bytes([sample_rom[i * 2], sample_rom[i * 2 + 1]]);
    }
    words
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Advance;

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Command {
    pub packet: CommandPacket,
    pub instance: u64,
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Reset;

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Gain {
    pub channel: u8,
    pub raw: u8,
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct Bank {
    pub page: u8,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum CoreInputKind {
    Advance(Advance),
    Command(Command),
    Reset(Reset),
    Gain(Gain),
    Bank(Bank),
}

impl Default for CoreInputKind {
    fn default() -> Self {
        CoreInputKind::Advance(Advance)
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct CoreInput {
    pub kind: CoreInputKind,
    pub tick: u64,
}

impl CoreInput {
    fn apply(&self, core: &mut Core) {
        match self.kind {
            CoreInputKind::Advance(_) => core.advance(self.tick),
            CoreInputKind::Command(command) => {
                core.command(&command.packet, command.instance, self.tick)
            }
            CoreInputKind::Reset(_) => core.reset(self.tick),
            CoreInputKind::Gain(gain) => core.set_gain(u32::from(gain.channel), gain.raw, self.tick),
            CoreInputKind::Bank(bank) => core.set_bank(bank.page, self.tick),
        }
    }
}

pub struct Synth {
    pub rom: Vec<u8>,
    pub samples: Vec<u16>,
    pub core: Core,
    pub generated: AtomicU64,
    pub render_ns: AtomicU64,
}

impl Synth {
    fn new(sound_rom: Vec<u8>, sample_words: Vec<u16>, sink: *mut dyn PcmSink) -> Synth {
        // SAFETY: the decoder owns both buffers for as long as this Synth lives, and a Vec's buffer does
        // not move when the Synth does, so the slices stay valid for the core's lifetime.
        let rom: &'static [u8] =
            unsafe { std::slice::from_raw_parts(sound_rom.as_ptr(), sound_rom.len()) };
        let samples: &'static [u16] =
            unsafe { std::slice::from_raw_parts(sample_words.as_ptr(), sample_words.len()) };
        let mut core = Core::new(rom, samples);
        core.set_pcm_sink(Some(sink));
        Synth {
            rom: sound_rom,
            samples: sample_words,
            core,
            generated: AtomicU64::new(0),
            render_ns: AtomicU64::new(0),
        }
    }
}

impl Applies<CoreInput> for Synth {
    fn apply(&mut self, input: &CoreInput) {
        let started = Instant::now();
        input.apply(&mut self.core);
        let elapsed = started.elapsed();
        self.render_ns.fetch_add(elapsed.as_nanos() as u64, Ordering::Relaxed);
        self.generated.store(self.core.frames(), Ordering::Relaxed);
    }
}

pub struct Audio {
    impl_: Box<Impl>,
}

pub struct Impl {
    /// Declared first so it is dropped (and its thread joined) before the services it points at.
    pub worker: WorkerQueue<CoreInput, Synth, QUEUE_SIZE>,
    pub pcm: Box<PcmRing>,
    pub synth: Box<Synth>,
    pub mailbox: SharedMailbox,
    pub clock: u64,
    pub command_count: u64,
    pub serial: u64,
    pub submitted_tick: u64,
    pub clock_epoch: u64,
    pub playback_epoch: u64,
    pub bank: u32,
    pub held: bool,
}

impl Impl {
    fn new(sound_rom: Vec<u8>, sample_words: Vec<u16>, shared: *mut u8) -> Impl {
        let mut pcm = Box::new(PcmRing::new());
        let sink_ptr: *mut PcmRing = &mut *pcm;
        let sink: *mut dyn PcmSink = sink_ptr;
        let mut synth = Box::new(Synth::new(sound_rom, sample_words, sink));
        let worker = WorkerQueue::new(&mut *synth as *mut Synth);
        Impl {
            worker,
            pcm,
            synth,
            mailbox: SharedMailbox::new(shared),
            clock: 0,
            command_count: 0,
            serial: 0,
            submitted_tick: 0,
            clock_epoch: 0,
            playback_epoch: 0,
            bank: 1,
            held: true,
        }
    }

    fn playback_now(&self) -> u64 {
        self.playback_epoch + (self.clock - self.clock_epoch)
    }

    fn post(&mut self, kind: CoreInputKind) {
        let tick = self.playback_now();
        self.worker.post(CoreInput { kind, tick });
    }

    fn post_gain(&mut self, channel: u32) {
        self.post(CoreInputKind::Gain(Gain {
            channel: channel as u8,
            raw: self.mailbox.gain(channel),
        }));
    }

    fn record(&mut self, packet: CommandPacket) {
        self.command_count += 1;
        self.serial += 1;
        let instance = self.serial;
        if self.serial >> 63 != 0 {
            panic!("HLE instance IDs exhausted");
        }
        self.post(CoreInputKind::Command(Command { packet, instance }));
    }

    fn submit_clock(&mut self) {
        if self.clock <= self.submitted_tick {
            return;
        }
        self.post(CoreInputKind::Advance(Advance));
        self.submitted_tick = self.clock;
    }

    fn flush(&mut self) {
        self.submit_clock();
        self.worker.flush();
    }

    fn snapshot(&self) -> AudioSnapshot {
        AudioSnapshot {
            clock: self.clock,
            commands: self.command_count,
            consumer: self.mailbox.consumer(),
            held: self.held,
        }
    }

    fn restore(&mut self, state: &AudioSnapshot) {
        self.flush();
        self.clock = state.clock;
        self.clock_epoch = state.clock;
        self.submitted_tick = state.clock;
        self.playback_epoch = self.synth.core.tick();
        self.pcm.discard();
        self.command_count = state.commands;
        self.mailbox.restore(state.consumer);
        self.held = state.held;
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct AudioPerformanceStats {
    pub commands: u64,
    pub rendered_frames: u64,
    pub voices_started: u64,
    pub voices_active: u32,
    pub render_ns: u64,
}

impl Audio {
    /// Whether the sound program and samples can be driven; the constructor panics when this is false.
    pub fn supported(sound_rom: &[u8], sample_words: &[u16]) -> bool {
        // SAFETY: validate() only uses the buffers while it runs; the 'static lifetime only states that
        // the core it builds never stores them beyond the call.
        let rom: &'static [u8] =
            unsafe { std::slice::from_raw_parts(sound_rom.as_ptr(), sound_rom.len()) };
        let samples: &'static [u16] =
            unsafe { std::slice::from_raw_parts(sample_words.as_ptr(), sample_words.len()) };
        Core::validate(rom, samples)
    }

    /// `sound_rom`: 0x80000+ bytes 68000 program code (interleaved high/low byte pairs), owned.
    /// `sample_words`: decoded sample ROM, owned. `shared_ram`: 0x800-byte dual-port RAM owned by the
    /// main CPU bus; must outlive the Audio.
    pub fn new(sound_rom: Vec<u8>, sample_words: Vec<u16>, shared_ram: *mut u8) -> Audio {
        Audio { impl_: Box::new(Impl::new(sound_rom, sample_words, shared_ram)) }
    }

    pub fn shared_write(&mut self, offset: u32) {
        if self.impl_.held {
            return;
        }
        if offset == MAILBOX_PRODUCER_L {
            let p: *mut Impl = &mut *self.impl_;
            // SAFETY: record() does not touch the mailbox the drain is iterating.
            unsafe { (*p).mailbox.drain(|packet| (*p).record(packet)) };
        } else if offset == MAILBOX_GAIN_CH0 || offset == MAILBOX_GAIN_CH1 {
            self.impl_.post_gain(offset & 1);
        }
    }

    pub fn finish_frame(&mut self) {
        let p = &mut *self.impl_;
        p.submit_clock();
        let frames = core::frames_at(p.playback_now());
        p.pcm.close_frame(frames);
    }

    pub fn advance(&mut self, main_cycles: u32) {
        self.impl_.clock += u64::from(main_cycles);
    }

    /// Continuous effective device time, not the caller's main-block endpoint.
    pub fn clock_ticks(&self) -> u64 {
        self.impl_.clock
    }

    pub fn generated_frames(&self) -> u64 {
        self.impl_.synth.generated.load(Ordering::Relaxed)
    }

    pub fn performance_stats(&self) -> AudioPerformanceStats {
        let render: RenderStats = self.impl_.synth.core.render_stats();
        AudioPerformanceStats {
            commands: self.impl_.command_count,
            rendered_frames: self.generated_frames(),
            voices_started: render.voices_started,
            voices_active: render.voices_active,
            render_ns: self.impl_.synth.render_ns.load(Ordering::Relaxed),
        }
    }

    /// Reset control (active-high asserted = CPU held in reset).
    pub fn set_reset(&mut self, asserted: bool) {
        let p = &mut *self.impl_;
        if p.held == asserted {
            return;
        }
        p.held = asserted;
        if asserted {
            p.mailbox.reset();
            p.post(CoreInputKind::Reset(Reset));
        } else {
            p.post_gain(0);
            p.post_gain(1);
        }
    }

    pub fn is_reset(&self) -> bool {
        self.impl_.held
    }

    /// Whole-machine/watchdog reset: hold the board and keep the continuous audio clock.
    pub fn reset_board(&mut self) {
        self.set_reset(true);
    }

    /// Sound CPU program ROM banking (0xc20000 - 0xc3ffff window, default page 1).
    pub fn set_sound_bank(&mut self, page: u32) {
        self.impl_.bank = page;
        self.impl_.post(CoreInputKind::Bank(Bank { page: page as u8 }));
    }

    pub fn sound_bank(&self) -> u32 {
        self.impl_.bank
    }

    pub fn sample_rate(&self) -> u32 {
        SAMPLE_RATE
    }

    pub fn available_frames(&mut self) -> usize {
        self.impl_.flush();
        self.impl_.pcm.available()
    }

    pub fn render(&mut self, interleaved_stereo: &mut [i16], max_frames: usize) -> usize {
        self.impl_.flush();
        self.impl_.pcm.drain_i16(interleaved_stereo, max_frames)
    }

    /// The float overload: the same ring, converted to the -1..1 scale the C++ passes on.
    pub fn render_f32(&mut self, interleaved_stereo: &mut [f32], max_frames: usize) -> usize {
        self.impl_.flush();
        self.impl_.pcm.drain(interleaved_stereo, max_frames)
    }

    /// Non-blocking drain: returns only PCM already produced without waiting. It holds back the audio of
    /// the video frame that `finish_frame()` just closed, so each call returns the frame before it
    /// whether or not the worker is quick: the amount does not race with it. Callers needing a complete,
    /// deterministic stream must finish with the blocking render.
    pub fn render_ready(&mut self, interleaved_stereo: &mut [i16], max_frames: usize) -> usize {
        self.impl_.pcm.drain_released(interleaved_stereo, max_frames)
    }

    pub fn state_size(&self) -> usize {
        crate::snapshot::BYTES
    }

    pub fn save_state(&self, dst: &mut [u8]) {
        self.impl_.snapshot().write(dst);
    }

    pub fn load_state(&mut self, src: &[u8]) -> Result<(), &'static str> {
        let state = AudioSnapshot::parse(src)?;
        self.impl_.restore(&state);
        Ok(())
    }
}
