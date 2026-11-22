//! The F3 sound board, driven by main-CPU mailbox writes and rendered on a synth worker thread.
//! Output is interleaved stereo (Left, Right) at `sample_rate()`.

use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::time::Instant;

use crate::hle::command::CommandPacket;
use crate::hle::core::{self, Core, SharedPcmSink};
use crate::hle::driver_layout::DriverLayout;
use crate::memory_map::{MAILBOX_GAIN_CH0, MAILBOX_GAIN_CH1, MAILBOX_PRODUCER_L};
use crate::pcm_ring::PcmRing;
use crate::shared_mailbox::SharedMailbox;
use crate::snapshot::AudioSnapshot;
use crate::worker_queue::{Applies, WorkerQueue};

pub const QUEUE_SIZE: usize = 4096;

/// Raw big-endian 16-bit sample ROM bytes to host words.
pub fn decode_sample_words(sample_rom: &[u8]) -> Vec<u16> {
    sample_rom
        .chunks_exact(2)
        .map(|pair| u16::from_be_bytes([pair[0], pair[1]]))
        .collect()
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub enum CoreInputKind {
    #[default]
    Advance,
    Command {
        packet: CommandPacket,
        instance: u64,
    },
    Reset,
    Gain {
        channel: u8,
        raw: u8,
    },
    Bank {
        page: u8,
    },
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct CoreInput {
    pub kind: CoreInputKind,
    pub tick: u64,
}

impl CoreInput {
    fn apply(&self, core: &mut Core) {
        match self.kind {
            CoreInputKind::Advance => core.advance(self.tick),
            CoreInputKind::Command { packet, instance } => {
                core.command(&packet, instance, self.tick)
            }
            CoreInputKind::Reset => core.reset(self.tick),
            CoreInputKind::Gain { channel, raw } => {
                core.set_gain(u32::from(channel), raw, self.tick)
            }
            CoreInputKind::Bank { page } => core.set_bank(page, self.tick),
        }
    }
}

/// Worker-side progress the caller reads without touching the `Core`.
#[derive(Default)]
pub struct SynthMetrics {
    pub generated: AtomicU64,
    pub render_ns: AtomicU64,
    pub voices_started: AtomicU64,
    pub voices_active: AtomicU32,
    pub tick: AtomicU64,
}

struct SendCore(Core);

// SAFETY: `Core` is `!Send` only because `Voice` holds raw pointers into data the `Core` owns. The
// core is moved once onto the single synth thread and never shared or touched from another thread.
unsafe impl Send for SendCore {}

struct Synth {
    // Declared before `_samples`: the core borrows the sample buffer.
    core: SendCore,
    metrics: Arc<SynthMetrics>,
    _samples: Vec<u16>,
}

impl Synth {
    fn new(
        sound_rom: &'static [u8],
        sample_words: Vec<u16>,
        sink: SharedPcmSink,
        layout: &'static DriverLayout,
        metrics: Arc<SynthMetrics>,
    ) -> Synth {
        // SAFETY: the Vec's heap buffer does not move when the Vec does, and `Synth` keeps it alive
        // for as long as the core that borrows it.
        let samples: &'static [u16] =
            unsafe { std::slice::from_raw_parts(sample_words.as_ptr(), sample_words.len()) };
        let mut core = Core::new(sound_rom, samples, layout);
        core.set_pcm_sink(Some(sink));
        Synth {
            core: SendCore(core),
            metrics,
            _samples: sample_words,
        }
    }
}

impl Applies<CoreInput> for Synth {
    fn apply(&mut self, input: &CoreInput) {
        let started = Instant::now();
        input.apply(&mut self.core.0);
        let elapsed_ns = started.elapsed().as_nanos() as u64;
        let render = self.core.0.render_stats();
        let metrics = &self.metrics;
        metrics.render_ns.fetch_add(elapsed_ns, Ordering::Relaxed);
        metrics
            .generated
            .store(self.core.0.frames(), Ordering::Relaxed);
        metrics
            .voices_started
            .store(render.voices_started, Ordering::Relaxed);
        metrics
            .voices_active
            .store(render.voices_active, Ordering::Relaxed);
        metrics.tick.store(self.core.0.tick(), Ordering::Release);
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

/// Main-thread clock and the queue into the synth; split from `Audio` so mailbox draining can
/// record commands while the mailbox is borrowed.
struct SynthLink {
    worker: WorkerQueue<CoreInput, QUEUE_SIZE>,
    clock: u64,
    clock_epoch: u64,
    playback_epoch: u64,
    submitted_tick: u64,
    command_count: u64,
    serial: u64,
}

impl SynthLink {
    fn playback_now(&self) -> u64 {
        self.playback_epoch + (self.clock - self.clock_epoch)
    }

    fn post(&self, kind: CoreInputKind) {
        let tick = self.playback_now();
        self.worker.post(CoreInput { kind, tick });
    }

    fn record(&mut self, packet: CommandPacket) {
        self.command_count += 1;
        self.serial += 1;
        let instance = self.serial;
        if self.serial >> 63 != 0 {
            panic!("HLE instance IDs exhausted");
        }
        self.post(CoreInputKind::Command { packet, instance });
    }

    fn submit_clock(&mut self) {
        if self.clock <= self.submitted_tick {
            return;
        }
        self.post(CoreInputKind::Advance);
        self.submitted_tick = self.clock;
    }

    fn flush(&mut self) {
        self.submit_clock();
        self.worker.flush();
    }
}

pub struct Audio {
    link: SynthLink,
    pcm: Arc<Mutex<PcmRing>>,
    metrics: Arc<SynthMetrics>,
    mailbox: SharedMailbox,
    bank: u32,
    held: bool,
}

impl Audio {
    pub fn new(
        sound_rom: &'static [u8],
        sample_words: Vec<u16>,
        shared: *mut u8,
        layout: &'static DriverLayout,
    ) -> Audio {
        let pcm = Arc::new(Mutex::new(PcmRing::new()));
        let metrics = Arc::new(SynthMetrics::default());
        let synth = Synth::new(
            sound_rom,
            sample_words,
            pcm.clone(),
            layout,
            Arc::clone(&metrics),
        );
        Audio {
            link: SynthLink {
                worker: WorkerQueue::spawn(synth),
                clock: 0,
                clock_epoch: 0,
                playback_epoch: 0,
                submitted_tick: 0,
                command_count: 0,
                serial: 0,
            },
            pcm,
            metrics,
            mailbox: SharedMailbox::new(shared),
            bank: 1,
            held: true,
        }
    }

    fn pcm(&self) -> MutexGuard<'_, PcmRing> {
        self.pcm.lock().unwrap_or_else(PoisonError::into_inner)
    }

    fn post_gain(&self, channel: u32) {
        let raw = self.mailbox.gain(channel);
        self.link.post(CoreInputKind::Gain {
            channel: channel as u8,
            raw,
        });
    }

    fn snapshot(&self) -> AudioSnapshot {
        AudioSnapshot {
            clock: self.link.clock,
            commands: self.link.command_count,
            consumer: self.mailbox.consumer(),
            held: self.held,
        }
    }

    fn restore(&mut self, state: &AudioSnapshot) {
        self.link.flush();
        self.link.clock = state.clock;
        self.link.clock_epoch = state.clock;
        self.link.submitted_tick = state.clock;
        self.link.playback_epoch = self.metrics.tick.load(Ordering::Acquire);
        self.pcm().discard();
        self.link.command_count = state.commands;
        self.mailbox.restore(state.consumer);
        self.held = state.held;
    }

    /// Whether the sound program and samples can be driven; the constructor panics when this is false.
    pub fn supported(
        sound_rom: &[u8],
        sample_words: &[u16],
        layout: &'static DriverLayout,
    ) -> bool {
        // SAFETY: validate() only uses the buffers while it runs; the 'static lifetime only states that
        // the core it builds never stores them beyond the call.
        let rom: &'static [u8] =
            unsafe { std::slice::from_raw_parts(sound_rom.as_ptr(), sound_rom.len()) };
        let samples: &'static [u16] =
            unsafe { std::slice::from_raw_parts(sample_words.as_ptr(), sample_words.len()) };
        Core::validate(rom, samples, layout)
    }

    pub fn shared_write(&mut self, offset: u32) {
        if self.held {
            return;
        }
        if offset == MAILBOX_PRODUCER_L {
            for packet in self.mailbox.drain() {
                self.link.record(packet);
            }
        } else if offset == MAILBOX_GAIN_CH0 || offset == MAILBOX_GAIN_CH1 {
            self.post_gain(offset & 1);
        }
    }

    pub fn finish_frame(&mut self) {
        self.link.submit_clock();
        let frames = core::frames_at(self.link.playback_now());
        self.pcm().close_frame(frames);
    }

    pub fn advance(&mut self, ticks: u32) {
        self.link.clock += u64::from(ticks);
    }

    /// Continuous effective device time, not the caller's main-block endpoint.
    pub fn clock_ticks(&self) -> u64 {
        self.link.clock
    }

    pub fn generated_frames(&self) -> u64 {
        self.metrics.generated.load(Ordering::Relaxed)
    }

    pub fn performance_stats(&self) -> AudioPerformanceStats {
        AudioPerformanceStats {
            commands: self.link.command_count,
            rendered_frames: self.generated_frames(),
            voices_started: self.metrics.voices_started.load(Ordering::Relaxed),
            voices_active: self.metrics.voices_active.load(Ordering::Relaxed),
            render_ns: self.metrics.render_ns.load(Ordering::Relaxed),
        }
    }

    /// Reset control (active-high asserted = CPU held in reset).
    pub fn set_reset(&mut self, asserted: bool) {
        if self.held == asserted {
            return;
        }
        self.held = asserted;
        if asserted {
            self.mailbox.reset();
            self.link.post(CoreInputKind::Reset);
        } else {
            self.post_gain(0);
            self.post_gain(1);
        }
    }

    pub fn is_reset(&self) -> bool {
        self.held
    }

    /// Whole-machine/watchdog reset: hold the board and keep the continuous audio clock.
    pub fn reset_board(&mut self) {
        self.set_reset(true);
    }

    /// Sound CPU program ROM banking (0xc20000 - 0xc3ffff window, default page 1).
    pub fn set_sound_bank(&mut self, page: u32) {
        self.bank = page;
        self.link.post(CoreInputKind::Bank { page: page as u8 });
    }

    pub fn available_frames(&mut self) -> usize {
        self.link.flush();
        self.pcm().available()
    }

    pub fn render(&mut self, interleaved_stereo: &mut [i16], max_frames: usize) -> usize {
        self.link.flush();
        self.pcm().drain_i16(interleaved_stereo, max_frames)
    }

    /// Float variant of `render`: the same ring on the -1..1 scale.
    pub fn render_f32(&mut self, interleaved_stereo: &mut [f32], max_frames: usize) -> usize {
        self.link.flush();
        self.pcm().drain(interleaved_stereo, max_frames)
    }

    /// Non-blocking drain: returns only PCM already produced without waiting. It holds back the audio of
    /// the video frame that `finish_frame()` just closed, so each call returns the frame before it
    /// whether or not the worker is quick: the amount does not race with it. Callers needing a complete,
    /// deterministic stream must finish with the blocking render.
    pub fn render_ready(&mut self, interleaved_stereo: &mut [i16], max_frames: usize) -> usize {
        self.pcm().drain_released(interleaved_stereo, max_frames)
    }

    pub fn save_state(&self, dst: &mut [u8]) {
        self.snapshot().write(dst);
    }

    pub fn load_state(&mut self, src: &[u8]) -> Result<(), &'static str> {
        let state = AudioSnapshot::parse(src)?;
        self.restore(&state);
        Ok(())
    }
}
