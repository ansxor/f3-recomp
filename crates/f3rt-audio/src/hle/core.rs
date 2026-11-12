//! `audio/hle/core.hpp` + `core.cpp`: the composition root of the HLE sound engine.
//!
//! It owns every service, keeps the 1 kHz service grid, dispatches the mailbox and renders. Every
//! service sees the same TIMER_TICKS boundaries, fanned out in a fixed order (`clock_tick`): the
//! sequence tempo pulses, then the fade pulse, then the mailbox, so a tick's events reach the services
//! before the render reaches the tick; the voice ring is stepped from the render at the first frame
//! that gets there (`VoiceService::service`).
//!
//! Services do not hold pointers to each other: a call borrows the mailbox and the track service for
//! its duration. The bank and the track service stay boxed because the program and voice services
//! keep pointers into them.

use std::sync::{Arc, Mutex, PoisonError};

use crate::hle::block::{self, BusBlock, StereoBlock};
use crate::hle::command::CommandPacket;
use crate::hle::effects::Effects;
use crate::hle::events::{
    EffectEventKind, Event, MAIN_CLOCK, SAMPLE_RATE, SONG_SLOTS, TIMER_TICKS, TrackEventKind,
};
use crate::hle::mailbox::Mailbox;
use crate::hle::program_service::{PatchList, ProgramQuery, ProgramService};
use crate::hle::sequencer::Sequencer;
use crate::hle::sound_bank::SoundBank;
use crate::hle::synthesis::{VoiceRenderState, render_voices, reset_render_state};
use crate::hle::track_service::TrackService;
use crate::hle::voice_service::VoiceService;
use crate::output_gain::OUTPUT_BOOST;

/// Receives rendered stereo frames in render order, in blocks of 1..Core::BLOCK_FRAMES frames.
pub trait PcmSink: Send {
    /// Interleaved L,R in OTIS/ESP channel order (the left/right exchange of the DAC is applied by the
    /// final consumer).
    fn write(&mut self, stereo: &[f32], frames: usize);
}

/// A sink shared between the rendering thread and whoever drains it.
pub type SharedPcmSink = Arc<Mutex<dyn PcmSink>>;

pub struct Core {
    pub bank: Arc<SoundBank>,
    pub effects: Box<Effects>,
    /// The next 1 kHz grid boundary to deliver.
    pub next_tick: u64,
    pub mailbox: Mailbox,
    pub programs: Box<ProgramService>,
    pub tracks: Box<TrackService>,
    pub voices: Box<VoiceService>,
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
    pub fn new(sound_rom: &'static [u8], sample_words: &'static [u16]) -> Core {
        let bank = Arc::new(SoundBank::new(sound_rom, sample_words));
        let effects = Box::new(Effects::new(sound_rom));
        // SAFETY: the bank is boxed just above and outlives the service.
        let programs = Box::new(ProgramService::new(Arc::clone(&bank)));
        let voices = Box::new(VoiceService::new(Arc::clone(&bank)));

        let mut core = Core {
            bank,
            effects,
            next_tick: grid_after(0),
            mailbox: Mailbox::new(),
            programs,
            tracks: Box::new(TrackService::new()),
            voices,
            sequencer: Sequencer::new(sound_rom),
            render_state: VoiceRenderState::new(),
            buses: BusBlock::new(),
            stereo: StereoBlock::new(),
            worker_tick: 0,
            worker_frames: 0,
            gain: [1.0, 1.0],
            sink: None,
        };

        core.sequencer.reset(0, &mut core.mailbox, &mut core.tracks);
        core.voices.reset(0);
        core.tracks.reset();
        reset_render_state(&mut core.render_state, 0);
        core
    }

    /// Applies one batch of events in arrival order: notes allocate, track events rewrite parameters,
    /// effects apply.
    fn dispatch_mailbox(&mut self) {
        let Core {
            mailbox,
            tracks,
            programs,
            voices,
            effects,
            ..
        } = self;
        mailbox.drain(|event| match event {
            Event::NoteOn(n) => {
                tracks.apply_note_on(&n);
                // The track record is read after apply_note_on, as in the C++.
                let t = &tracks.tracks(n.sequence)[usize::from(n.track)];
                let mut note = n;
                // A ProgramSelector earlier in this batch is already applied: the note resolves
                // with the program in force at this point of the event order.
                note.program = t.config.program;
                let query = ProgramQuery {
                    program: note.program,
                    key: t.config.parameters.transposed_key(note.kernel_key),
                    layer_mask: t.config.parameters.layer_mask(),
                    notes_suppressed: t.config.parameters.notes_suppressed(),
                };
                let patches: PatchList = programs.resolve(&query);
                if patches.count != 0 {
                    voices.note_on(&note, &patches, t);
                }
            }
            Event::NoteOff(n) => voices.note_off(n.instance, n.tick),
            Event::ReleaseKey(r) => {
                let instance = tracks.release_key(r.sequence, r.track, r.kernel_key, r.tick);
                if instance != 0 {
                    voices.note_off(instance, r.tick);
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
                    | TrackEventKind::Refresh => voices.refresh_track(e.sequence, e.track, e.tick),
                    TrackEventKind::SequenceVolume
                    | TrackEventKind::SequencePan
                    | TrackEventKind::SequenceFade
                    | TrackEventKind::SequenceFadeStep => {
                        voices.refresh_sequence(e.sequence, e.tick)
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

    fn render_until(&mut self, tick: u64) {
        let end = frames_at(tick);
        let mut input = [0.0f32; 2];
        let mut output = [0.0f32; 2];
        for ch in 0..2 {
            input[ch] = 0.18;
            output[ch] = OUTPUT_BOOST * self.gain[ch];
        }
        while self.worker_frames < end {
            let n = (block::BLOCK_FRAMES as u64).min(end - self.worker_frames) as usize;
            if self.render_state.frame_ticks.len() < n {
                self.render_state.frame_ticks.resize(n, 0);
            }
            let mut frame_tick = self.render_state.tick;
            let mut remainder = self.render_state.tick_remainder;
            for f in 0..n {
                self.render_state.frame_ticks[f] = frame_tick;
                remainder += MAIN_CLOCK;
                frame_tick += u64::from(remainder / SAMPLE_RATE);
                remainder %= SAMPLE_RATE;
            }
            self.buses.resize(n);
            let mut frame_start = 0;
            while frame_start < n {
                let now = self.render_state.frame_ticks[frame_start];
                // The voice ring and every queued driver start due by this frame run before it is
                // rendered.
                loop {
                    let timer = self.voices.next_tick();
                    let timer_due = timer <= now;
                    let queued = self.voices.next_queue_tick();
                    let queue_due = queued <= now;
                    if !timer_due && !queue_due {
                        break;
                    }
                    if queue_due && (!timer_due || queued <= timer) {
                        self.voices
                            .service_queue(if timer_due { timer } else { now });
                    } else {
                        self.voices.service();
                    }
                }
                let mut frame_end = frame_start + 1;
                let horizon = self.voices.next_tick().min(self.voices.next_queue_tick());
                while frame_end < n && self.render_state.frame_ticks[frame_end] < horizon {
                    frame_end += 1;
                }
                self.buses.samples[frame_start * 8..frame_end * 8].fill(0.0);
                self.voices.begin_render();
                render_voices(
                    &mut self.voices,
                    Arc::clone(&self.bank),
                    &mut self.render_state,
                    self.buses.span_mut(),
                    frame_start,
                    frame_end,
                );
                self.voices.finish_render();
                let clamped_high = 1.0f32 - 1.0f32 / 524288.0f32;
                for sample in &mut self.buses.samples[frame_start * 8..frame_end * 8] {
                    *sample = if *sample < -1.0f32 {
                        -1.0f32
                    } else if clamped_high < *sample {
                        clamped_high
                    } else {
                        *sample
                    };
                }
                frame_start = frame_end;
            }
            self.render_state.tick = frame_tick;
            self.render_state.tick_remainder = remainder;
            self.stereo.resize(n);
            self.effects
                .mix_block(&self.buses, &mut self.stereo, input, output);
            if let Some(sink) = &self.sink {
                let stereo: &[f32] = &self.stereo.samples[..n * 2];
                sink.lock()
                    .unwrap_or_else(PoisonError::into_inner)
                    .write(stereo, n);
            }
            self.worker_frames += n as u64;
        }
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
        self.voices.reset(worker_tick);
        self.tracks.reset();
        reset_render_state(&mut self.render_state, worker_tick);
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

    /// Checks that the ROM and samples can be driven by the HLE core. Throws nothing: the C++ builds a
    /// Core inside try/catch and drops the message, which is why the panic hook is silenced here.
    pub fn validate(sound_rom: &'static [u8], sample_words: &'static [u16]) -> bool {
        let hook = std::panic::take_hook();
        std::panic::set_hook(Box::new(|_| {}));
        let result = std::panic::catch_unwind(std::panic::AssertUnwindSafe(|| {
            Core::new(sound_rom, sample_words);
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
