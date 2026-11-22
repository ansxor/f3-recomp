//! The composition root of the HLE sound engine.
//!
//! It owns every service, keeps the 1 kHz service grid, dispatches the mailbox and renders. Every
//! service sees the same TIMER_TICKS boundaries, fanned out in a fixed order (`clock_tick`): the
//! sequence tempo pulses, then the fade pulse, then the mailbox, so a tick's events reach the services
//! before the render reaches the tick; the voice ring is stepped from the render at the first frame
//! that gets there (`VoiceService::service`).
//!
//! Services do not hold references to each other: a call borrows the bank, the mailbox and the track
//! service for its duration. The effects and the track service stay boxed so `Core::new` stays light
//! on the stack.

use std::sync::{Arc, Mutex, PoisonError};

use crate::hle::block::{self, BusBlock, StereoBlock};
use crate::hle::command::CommandPacket;
use crate::hle::driver_layout::DriverLayout;
use crate::hle::effects::{Effects, Program};
use crate::hle::events::{
    EffectEventKind, Event, MAIN_CLOCK, SAMPLE_RATE, SONG_SLOTS, TIMER_TICKS, TrackEventKind,
};
use crate::hle::mailbox::Mailbox;
use crate::hle::patch::ProgramQuery;
use crate::hle::sequencer::Sequencer;
use crate::hle::sound_bank::SoundBank;
use crate::hle::synthesis::VoiceRenderState;
use crate::hle::track_service::TrackService;
use crate::hle::voice_service::VoiceService;

const OUTPUT_BOOST: f32 = 12.0;

/// Receives rendered stereo frames in render order, in blocks of 1..Core::BLOCK_FRAMES frames.
pub trait PcmSink: Send {
    /// Interleaved L,R in OTIS/ESP channel order (the left/right exchange of the DAC is applied by the
    /// final consumer).
    fn write(&mut self, stereo: &[f32], frames: usize);
}

/// A sink shared between the rendering thread and whoever drains it.
pub type SharedPcmSink = Arc<Mutex<dyn PcmSink>>;

pub struct Core {
    pub bank: SoundBank,
    pub effects: Box<Effects>,
    /// The next 1 kHz grid boundary to deliver.
    pub next_tick: u64,
    pub mailbox: Mailbox,
    pub tracks: Box<TrackService>,
    pub voices: VoiceService,
    pub sequencer: Sequencer,
    pub render_state: VoiceRenderState,
    pub buses: BusBlock,
    pub stereo: StereoBlock,
    pub worker_tick: u64,
    pub worker_frames: u64,
    pub gain: [f32; 2],
    pub sink: Option<SharedPcmSink>,
}

/// The first service-grid boundary after `tick`.
fn grid_after(tick: u64) -> u64 {
    (tick / TIMER_TICKS as u64 + 1) * TIMER_TICKS as u64
}

/// `floor(tick * SAMPLE_RATE / MAIN_CLOCK)`, overflow-free.
pub fn frames_at(tick: u64) -> u64 {
    tick / u64::from(MAIN_CLOCK) * u64::from(SAMPLE_RATE)
        + tick % u64::from(MAIN_CLOCK) * u64::from(SAMPLE_RATE) / u64::from(MAIN_CLOCK)
}

impl Core {
    pub fn new(
        sound_rom: &'static [u8],
        sample_words: Arc<[u16]>,
        layout: &'static DriverLayout,
    ) -> Core {
        let bank = SoundBank::new(sound_rom, sample_words, layout);
        let voices = VoiceService::new(&bank);
        let mut core = Core {
            bank,
            effects: Box::new(Effects::new(sound_rom, layout)),
            next_tick: grid_after(0),
            mailbox: Mailbox::new(),
            tracks: Box::new(TrackService::new()),
            voices,
            sequencer: Sequencer::new(sound_rom, layout),
            render_state: VoiceRenderState::new(),
            buses: BusBlock::new(),
            stereo: StereoBlock::new(),
            worker_tick: 0,
            worker_frames: 0,
            gain: [1.0, 1.0],
            sink: None,
        };

        core.sequencer.reset(0, &mut core.mailbox, &mut core.tracks);
        core.tracks.reset();
        core
    }

    /// Applies one batch of events in arrival order: notes allocate, track events rewrite parameters,
    /// effects apply.
    fn dispatch_mailbox(&mut self) {
        let Core {
            mailbox,
            tracks,
            voices,
            effects,
            bank,
            ..
        } = self;
        mailbox.drain(|event| match event {
            Event::NoteOn(n) => {
                tracks.apply_note_on(&n);
                let track = tracks.track(n.sequence, n.track);
                let mut note = n;
                // A ProgramSelector earlier in this batch is already applied: the note resolves
                // with the program in force at this point of the event order.
                note.program = track.config.program;
                let patches = ProgramQuery::for_note(&note, track).resolve(bank);
                if !patches.is_empty() {
                    voices.note_on(&note, &patches, bank, tracks);
                }
            }
            Event::NoteOff(n) => voices.note_off(n.instance, n.tick, bank, tracks),
            Event::ReleaseKey(r) => {
                let instance = tracks.release_key(r.sequence, r.track, r.kernel_key, r.tick);
                if instance != 0 {
                    voices.note_off(instance, r.tick, bank, tracks);
                }
            }
            Event::Track(e) => {
                tracks.apply_track(&e);
                match e.kind {
                    TrackEventKind::Controller
                    | TrackEventKind::Program
                    | TrackEventKind::ProgramSelector
                    | TrackEventKind::Volume
                    | TrackEventKind::Pan
                    | TrackEventKind::Refresh => {
                        voices.refresh_track(e.sequence, e.track, bank, tracks)
                    }
                    TrackEventKind::SequenceVolume
                    | TrackEventKind::SequencePan
                    | TrackEventKind::SequenceFade
                    | TrackEventKind::SequenceFadeStep => {
                        voices.refresh_sequence(e.sequence, bank, tracks)
                    }
                    TrackEventKind::PreviousKey => {}
                }
            }
            Event::Effect(e) => {
                if e.kind == EffectEventKind::Preset {
                    effects.select(e.preset);
                } else {
                    effects.parameter(e.index, e.value);
                }
            }
        });
    }

    /// One 1 kHz boundary: per sequence the note-duration pulse then the sequencer pulse, then the
    /// fade pulse, then the events they queued.
    fn clock_tick(&mut self) {
        let tick = self.next_tick;
        self.next_tick += TIMER_TICKS as u64;
        for i in 0..SONG_SLOTS {
            if self.sequencer.beats()[i].tick() {
                self.tracks.note_pulse(i as u8, tick, &mut self.mailbox);
                self.sequencer
                    .pulse(i as u8, tick, &mut self.mailbox, &mut self.tracks);
            }
        }
        if self.tracks.fade_timer().tick() {
            self.tracks.fade_pulse(tick, &mut self.mailbox);
        }
        self.dispatch_mailbox();
    }

    /// Renders the output frames up to `tick`, one bus block at a time.
    fn render_until(&mut self, tick: u64) {
        let end = frames_at(tick);
        let (link_gain, output_gain) = self.mix_gains();
        while self.worker_frames < end {
            let frames = (block::BLOCK_FRAMES as u64).min(end - self.worker_frames) as usize;
            self.render_bus_block(frames);
            self.stereo.resize(frames);
            self.effects
                .mix_block(&self.buses, &mut self.stereo, link_gain, output_gain);
            self.write_sink(frames);
            self.worker_frames += frames as u64;
        }
    }

    /// The OTIS-to-ESP link gain and the host output gain, per channel. The link is digital (gain
    /// 1.0). The overdrive's output level ignores its input, so the 0.18 must come after the ESP or
    /// it only quietens the paths around it.
    fn mix_gains(&self) -> ([f32; 2], [f32; 2]) {
        let (link_gain, host_gain) = if self.effects.program() == Program::Overdrive {
            (1.0, 0.18)
        } else {
            (0.18, 1.0)
        };
        let output = self.gain.map(|channel| OUTPUT_BOOST * host_gain * channel);
        ([link_gain; 2], output)
    }

    /// Fills the bus block with the voices' output for `frames` frames, stopping at every tick
    /// where the voice services must run.
    fn render_bus_block(&mut self, frames: usize) {
        self.render_state.fill_frame_ticks(frames);
        self.buses.resize(frames);
        let mut frame_start = 0;
        while frame_start < frames {
            self.service_due_voices(self.render_state.frame_ticks[frame_start]);
            let frame_end = self.frames_before_next_service(frame_start, frames);
            self.buses.samples[frame_start * 8..frame_end * 8].fill(0.0);
            self.render_state.render_voices(
                self.voices.voices_mut(),
                &self.bank.samples,
                &mut self.buses,
                frame_start,
                frame_end,
            );
            self.clamp_otis_buses(frame_start, frame_end);
            frame_start = frame_end;
        }
    }

    /// Runs the voice ring and every queued driver start due by `now`, in tick order.
    fn service_due_voices(&mut self, now: u64) {
        loop {
            let timer = self.voices.next_tick();
            let queued = self.voices.next_queue_tick();
            let timer_due = timer <= now;
            let queue_due = queued <= now;
            if !timer_due && !queue_due {
                return;
            }
            if queue_due && (!timer_due || queued <= timer) {
                let limit = if timer_due { timer } else { now };
                self.voices.service_queue(limit, &self.bank, &self.tracks);
            } else {
                self.voices.service(&self.bank, &self.tracks);
            }
        }
    }

    /// The end of the run of frames, starting at `frame_start`, that precede the next voice service.
    fn frames_before_next_service(&self, frame_start: usize, frames: usize) -> usize {
        let horizon = self.voices.next_tick().min(self.voices.next_queue_tick());
        let ticks = &self.render_state.frame_ticks[..frames];
        let serviced = ticks[frame_start + 1..].iter().position(|&t| t >= horizon);
        frame_start + 1 + serviced.unwrap_or(frames - frame_start - 1)
    }

    /// The OTIS output saturates at the DAC's signed range.
    fn clamp_otis_buses(&mut self, frame_start: usize, frame_end: usize) {
        let high = 1.0f32 - 1.0f32 / 524288.0f32;
        for sample in &mut self.buses.samples[frame_start * 8..frame_end * 8] {
            *sample = sample.clamp(-1.0, high);
        }
    }

    fn write_sink(&self, frames: usize) {
        let Some(sink) = &self.sink else { return };
        let stereo: &[f32] = &self.stereo.samples[..frames * 2];
        sink.lock()
            .unwrap_or_else(PoisonError::into_inner)
            .write(stereo, frames);
    }

    pub const BLOCK_FRAMES: usize = block::BLOCK_FRAMES;

    /// None drops the rendered frames (fast-forward); the engine state evolves identically.
    pub fn set_pcm_sink(&mut self, sink: Option<SharedPcmSink>) {
        self.sink = sink;
    }

    /// Sequencer events up to and including this tick have fired.
    pub fn tick(&self) -> u64 {
        self.worker_tick
    }

    /// Frames handed to the sink so far.
    pub fn frames(&self) -> u64 {
        self.worker_frames
    }

    pub fn render_stats(&self) -> RenderStats {
        RenderStats {
            voices_started: self.voices.launched(),
            voices_active: self.voices.active_voices(),
        }
    }

    pub fn frames_at(tick: u64) -> u64 {
        frames_at(tick)
    }

    /// Each input first runs advance(tick) and then applies at tick().
    pub fn advance(&mut self, tick: u64) {
        let tick = tick.max(self.worker_tick);
        while self.worker_tick < tick {
            let next = tick.min(self.next_tick);
            self.render_until(next);
            while self.next_tick <= next {
                self.clock_tick();
            }
            self.worker_tick = next;
        }
    }

    /// Applies one parsed mailbox command packet at tick(). See `CommandPacket::parse` for the framing
    /// rules.
    pub fn command(&mut self, packet: &CommandPacket, instance: u64, tick: u64) {
        self.advance(tick);
        self.sequencer.command(
            packet,
            instance,
            self.worker_tick,
            &mut self.mailbox,
            &mut self.tracks,
        );
        self.dispatch_mailbox();
    }

    pub fn reset(&mut self, tick: u64) {
        self.advance(tick);
        let worker_tick = self.worker_tick;
        self.sequencer
            .reset(worker_tick, &mut self.mailbox, &mut self.tracks);
        self.mailbox.clear();
        // Voices first: their Stop events still see the tracks they point at.
        self.voices.reset(worker_tick, &self.bank);
        self.tracks.reset();
        self.render_state.reset(worker_tick);
        self.next_tick = grid_after(worker_tick);
        self.effects.reset();
        self.gain.fill(1.0);
        // The sound CPU's cold-boot bank, as in sequencer.reset.
        self.bank.set_bank(1);
    }

    /// `page` = the value the Audio implementation passes to `set_sound_bank`.
    pub fn set_bank(&mut self, page: u8, tick: u64) {
        self.advance(tick);
        self.bank.set_bank(page);
        let worker_tick = self.worker_tick;
        self.sequencer.set_bank(page, worker_tick);
    }

    /// `raw` = mailbox byte 0x7fa (channel 0) / 0x7fb (channel 1); only raw & 63 is used.
    pub fn set_gain(&mut self, channel: u32, raw: u8, tick: u64) {
        self.advance(tick);
        self.gain[(channel & 1) as usize] =
            10.0f32.powf(-0.5f32 * f32::from(63 - (raw & 63)) / 20.0f32);
    }

    /// Checks that the ROM and samples can be driven by the HLE core. Never panics: construction
    /// failures are reported as `false`, which is why the panic hook is silenced here.
    pub fn validate(
        sound_rom: &'static [u8],
        sample_words: Arc<[u16]>,
        layout: &'static DriverLayout,
    ) -> bool {
        let hook = std::panic::take_hook();
        std::panic::set_hook(Box::new(|_| {}));
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            Core::new(sound_rom, sample_words, layout);
        }));
        std::panic::set_hook(hook);
        result.is_ok()
    }

    /// Sets flush-to-zero and denormals-are-zero on the calling thread (x86 SSE, aarch64 FPCR); no-op
    /// elsewhere, including WebAssembly. Core never calls it: the thread driving the Core does.
    pub fn enable_flush_denormals() {
        #[cfg(all(
            any(target_arch = "x86_64", target_arch = "x86"),
            not(target_os = "emscripten")
        ))]
        {
            // _MM_SET_FLUSH_ZERO_MODE(_MM_FLUSH_ZERO_ON) and _MM_SET_DENORMALS_ZERO_MODE(
            // _MM_DENORMALS_ZERO_ON) are MXCSR bit 15 (FTZ) and bit 6 (DAZ).
            const FLUSH_ZERO_MASK: u32 = 1 << 15;
            const DENORMALS_ZERO_MASK: u32 = 1 << 6;
            let mut csr: u32 = 0;
            // SAFETY: reads and writes this thread's MXCSR control word.
            unsafe {
                std::arch::asm!("stmxcsr [{}]", in(reg) &mut csr, options(nostack, preserves_flags));
                csr |= FLUSH_ZERO_MASK;
                csr |= DENORMALS_ZERO_MASK;
                std::arch::asm!("ldmxcsr [{}]", in(reg) &csr, options(nostack, preserves_flags));
            }
        }
        #[cfg(all(target_arch = "aarch64", not(target_os = "emscripten")))]
        {
            const FPCR_FZ: u64 = 1 << 24;
            // SAFETY: reads and writes the thread's FPCR control register.
            unsafe {
                let mut fpcr: u64;
                std::arch::asm!("mrs {}, fpcr", out(reg) fpcr, options(nomem, nostack));
                fpcr |= FPCR_FZ;
                std::arch::asm!("msr fpcr, {}", in(reg) fpcr, options(nomem, nostack));
            }
        }
        #[cfg(all(target_arch = "arm", not(target_os = "emscripten")))]
        {
            // FZ: flush-to-zero, the AArch32 counterpart of FPCR.FZ.
            const FPSCR_FZ: u32 = 1 << 24;
            // SAFETY: reads and writes the thread's FPSCR control register.
            unsafe {
                let mut fpscr: u32;
                std::arch::asm!("vmrs {}, fpscr", out(reg) fpscr, options(nomem, nostack));
                fpscr |= FPSCR_FZ;
                std::arch::asm!("vmsr fpscr, {}", in(reg) fpscr, options(nomem, nostack));
            }
        }
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct RenderStats {
    pub voices_started: u64,
    pub voices_active: u32,
}
