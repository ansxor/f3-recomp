//! The F3 sound board, driven by main-CPU mailbox writes and rendered on a synth worker thread.
//! Output is interleaved stereo (Left, Right) at `sample_rate()`.

use std::sync::atomic::{AtomicU32, AtomicU64, Ordering};
use std::sync::{Arc, Mutex, MutexGuard, PoisonError};
use std::time::Instant;

use crate::hle::command::CommandPacket;
use crate::hle::core::{self, Core, SharedPcmSink};
use crate::hle::driver_layout::DriverLayout;
use crate::memory_map::{MAILBOX_GAIN_CH0, MAILBOX_GAIN_CH1, MAILBOX_PRODUCER_L, SHARED_RAM_SIZE};
use crate::pcm_ring::PcmRing;
use crate::shared_mailbox::SharedMailbox;
use crate::snapshot::AudioSnapshot;
use crate::worker_queue::{Applies, WorkerQueue};

pub const QUEUE_SIZE: usize = 4096;

/// Raw big-endian 16-bit sample ROM bytes to host words.
pub fn decode_sample_words(sample_rom: &[u8]) -> Box<[u16]> {
    sample_rom
        .chunks_exact(2)
        .map(|pair| u16::from_be_bytes([pair[0], pair[1]]))
        .collect::<Vec<u16>>()
        .into_boxed_slice()
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

#[derive(Default)]
pub struct SynthMetrics {
    pub generated: AtomicU64,
    pub render_ns: AtomicU64,
    pub voices_started: AtomicU64,
    pub voices_active: AtomicU32,
    pub tick: AtomicU64,
}

struct Synth {
    core: Core,
    metrics: Arc<SynthMetrics>,
}

impl Synth {
    fn try_new(
        sound_rom: Arc<[u8]>,
        sample_words: Box<[u16]>,
        sink: SharedPcmSink,
        layout: &'static DriverLayout,
        metrics: Arc<SynthMetrics>,
    ) -> Result<Synth, &'static str> {
        let mut core = Core::try_new(sound_rom, sample_words, layout)?;
        core.set_pcm_sink(Some(sink));
        Ok(Synth { core, metrics })
    }
}

impl Applies<CoreInput> for Synth {
    fn apply(&mut self, input: &CoreInput) {
        let started = Instant::now();
        input.apply(&mut self.core);
        let elapsed_ns = started.elapsed().as_nanos() as u64;
        let render = self.core.render_stats();
        let metrics = &self.metrics;
        metrics.render_ns.fetch_add(elapsed_ns, Ordering::Relaxed);
        metrics
            .generated
            .store(self.core.rendered_frames(), Ordering::Relaxed);
        metrics
            .voices_started
            .store(render.voices_started, Ordering::Relaxed);
        metrics
            .voices_active
            .store(render.voices_active, Ordering::Relaxed);
        metrics.tick.store(self.core.tick(), Ordering::Release);
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
    fn advance(&mut self, ticks: u32) {
        self.clock = self.clock.wrapping_add(u64::from(ticks));
    }

    fn rebase(&mut self, clock: u64, playback_epoch: u64) {
        self.clock = clock;
        self.clock_epoch = clock;
        self.submitted_tick = clock;
        self.playback_epoch = playback_epoch;
    }

    fn playback_now(&self) -> u64 {
        self.playback_epoch.wrapping_add(self.clock.wrapping_sub(self.clock_epoch))
    }

    fn post(&self, kind: CoreInputKind) {
        let tick = self.playback_now();
        self.worker.post(CoreInput { kind, tick });
    }

    fn record(&mut self, packet: CommandPacket) {
        self.command_count = self.command_count.wrapping_add(1);
        self.serial += 1;
        let instance = self.serial;
        if self.serial >> 63 != 0 {
            panic!("HLE instance IDs exhausted");
        }
        self.post(CoreInputKind::Command { packet, instance });
    }

    fn submit_clock(&mut self) {
        if self.clock == self.submitted_tick {
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
    /// `Err` names why the sound program or the samples cannot be driven.
    pub fn try_new(
        sound_rom: Arc<[u8]>,
        sample_words: Box<[u16]>,
        layout: &'static DriverLayout,
    ) -> Result<Audio, &'static str> {
        let pcm = Arc::new(Mutex::new(PcmRing::new()));
        let metrics = Arc::new(SynthMetrics::default());
        let synth = Synth::try_new(
            sound_rom,
            sample_words,
            pcm.clone(),
            layout,
            Arc::clone(&metrics),
        )?;
        Ok(Audio {
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
            mailbox: SharedMailbox::new(),
            bank: 1,
            held: true,
        })
    }

    fn pcm(&self) -> MutexGuard<'_, PcmRing> {
        self.pcm.lock().unwrap_or_else(PoisonError::into_inner)
    }

    fn post_gain(&self, ram: &[u8; SHARED_RAM_SIZE], channel: u32) {
        let raw = self.mailbox.gain(ram, channel);
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

    fn restore(&mut self, state: &AudioSnapshot) -> Result<(), &'static str> {
        self.mailbox.restore(state.consumer)?;
        self.link.flush();
        self.link.rebase(state.clock, self.metrics.tick.load(Ordering::Acquire));
        self.pcm().discard();
        self.link.command_count = state.commands;
        self.held = state.held;
        Ok(())
    }

    /// The main CPU wrote `offset` of `ram`: a producer-index write drains the mailbox, a gain write
    /// posts the gain.
    pub fn shared_write(&mut self, ram: &mut [u8; SHARED_RAM_SIZE], offset: u32) {
        if self.held {
            return;
        }
        if offset == MAILBOX_PRODUCER_L {
            for packet in self.mailbox.drain(ram) {
                self.link.record(packet);
            }
        } else if offset == MAILBOX_GAIN_CH0 || offset == MAILBOX_GAIN_CH1 {
            self.post_gain(ram, offset & 1);
        }
    }

    pub fn finish_frame(&mut self) {
        self.link.submit_clock();
        let frames = core::frames_at(self.link.playback_now());
        self.pcm().close_frame(frames);
    }

    pub fn advance(&mut self, ticks: u32) {
        self.link.advance(ticks);
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
    pub fn set_reset(&mut self, ram: &[u8; SHARED_RAM_SIZE], asserted: bool) {
        if self.held == asserted {
            return;
        }
        self.held = asserted;
        if asserted {
            self.mailbox.reset();
            self.link.post(CoreInputKind::Reset);
        } else {
            self.post_gain(ram, 0);
            self.post_gain(ram, 1);
        }
    }

    pub fn is_reset(&self) -> bool {
        self.held
    }

    /// Whole-machine/watchdog reset: hold the board and keep the continuous audio clock.
    pub fn reset_board(&mut self, ram: &[u8; SHARED_RAM_SIZE]) {
        self.set_reset(ram, true);
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
        self.restore(&state)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    struct Recorder(Arc<Mutex<Vec<CoreInput>>>);

    impl Applies<CoreInput> for Recorder {
        fn apply(&mut self, input: &CoreInput) {
            self.0.lock().unwrap().push(*input);
        }
    }

    fn link() -> (SynthLink, Arc<Mutex<Vec<CoreInput>>>) {
        let log = Arc::new(Mutex::new(Vec::new()));
        let link = SynthLink {
            worker: WorkerQueue::spawn(Recorder(Arc::clone(&log))),
            clock: 0,
            clock_epoch: 0,
            playback_epoch: 0,
            submitted_tick: 0,
            command_count: 0,
            serial: 0,
        };
        (link, log)
    }

    fn advances(link: &SynthLink, log: &Mutex<Vec<CoreInput>>) -> Vec<u64> {
        link.worker.flush();
        let log = log.lock().unwrap();
        log.iter().filter(|input| input.kind == CoreInputKind::Advance).map(|input| input.tick).collect()
    }

    #[test]
    fn a_snapshot_clock_at_the_top_of_u64_keeps_advancing_and_submitting() {
        let (mut link, log) = link();
        link.rebase(u64::MAX - 1, 7);
        link.submit_clock();
        link.advance(1);
        link.submit_clock();
        link.advance(2);
        link.submit_clock();
        assert_eq!(link.clock, 1);
        assert_eq!(advances(&link, &log), [8, 10]);
        link.submit_clock();
        assert_eq!(advances(&link, &log), [8, 10]);
    }

    #[test]
    fn recording_commands_wraps_the_command_count() {
        let (mut link, _log) = link();
        link.command_count = u64::MAX;
        link.record(CommandPacket::default());
        assert_eq!(link.command_count, 0);
    }
}
