//! `audio/hle/core.hpp` + `core.cpp`: the composition root of the HLE sound engine.
//!
//! It owns every service, wires the 1 kHz clock, dispatches the mailbox and renders. It is a clock
//! listener itself so that a tick's events reach the services after the sequencer's pulses and the
//! fade pulse and before the render reaches the tick; the voice ring is stepped from the render at
//! the first frame that gets there (`VoiceService::service`), as it always has been.
//!
//! The services are boxed so their addresses are stable: the clock holds listener pointers to them and
//! they hold pointers to each other, exactly as the C++ members' addresses are stable inside the
//! heap-allocated `Impl`.

use std::sync::{Arc, Mutex, PoisonError};

use crate::hle::block::{self, BusBlock, StereoBlock};
use crate::hle::clock;
use crate::hle::command::CommandPacket;
use crate::hle::effects::Effects;
use crate::hle::events::{
    EffectEventKind, Event, MAIN_CLOCK, SAMPLE_RATE, SONG_SLOTS, TrackEventKind,
};
use crate::hle::mailbox::Mailbox;
use crate::hle::program_service::{PatchList, ProgramQuery, ProgramService};
use crate::hle::pulse_timer;
use crate::hle::sequencer::Sequencer;
use crate::hle::sound_bank::SoundBank;
use crate::hle::synthesis::{VoiceRenderState, render_voices, reset_render_state};
use crate::hle::track_service::{Track, TrackService};
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
    impl_: Box<Impl>,
}

/// The composition root's state, after `audio/hle/core.cpp`'s `struct Core::Impl`.
pub struct Impl {
    pub bank: Box<SoundBank>,
    pub effects: Box<Effects>,
    pub clock: Box<clock::Clock>,
    pub mailbox: Box<Mailbox>,
    pub programs: Box<ProgramService>,
    pub tracks: Box<TrackService>,
    pub voices: Box<VoiceService>,
    pub sequencer: Box<Sequencer>,
    pub render_state: VoiceRenderState,
    pub buses: BusBlock,
    pub stereo: StereoBlock,
    pub worker_tick: u64,
    pub worker_frames: u64,
    pub gain: [f32; 2],
    pub sink: Option<SharedPcmSink>,
}

impl Impl {
    fn new(sound_rom: &'static [u8], sample_words: &'static [u16]) -> Box<Impl> {
        let mut bank = Box::new(SoundBank::new(sound_rom, sample_words));
        let bank_ptr: *mut SoundBank = &mut *bank;
        let effects = Box::new(Effects::new(sound_rom));
        let clock = Box::new(clock::Clock::new(0));
        let mut mailbox = Box::new(Mailbox::new());
        let mailbox_ptr: *mut Mailbox = &mut *mailbox;
        // SAFETY: the bank is boxed just above and outlives the service.
        let programs = Box::new(unsafe { ProgramService::new(bank_ptr) });
        let mut tracks = Box::new(TrackService::new(mailbox_ptr));
        let tracks_ptr: *mut TrackService = &mut *tracks;
        let voices = Box::new(VoiceService::new(bank_ptr));
        let sequencer = Box::new(Sequencer::new(sound_rom, mailbox_ptr, tracks_ptr));

        let mut imp = Box::new(Impl {
            bank,
            effects,
            clock,
            mailbox,
            programs,
            tracks,
            voices,
            sequencer,
            render_state: VoiceRenderState::new(),
            buses: BusBlock::new(),
            stereo: StereoBlock::new(),
            worker_tick: 0,
            worker_frames: 0,
            gain: [1.0, 1.0],
            sink: None,
        });

        // Tempo pulses, then the fade pulse, then the events they queued.
        let tracks_listener: *mut dyn pulse_timer::Listener = tracks_ptr;
        let sequencer_listener: *mut dyn pulse_timer::Listener = &mut *imp.sequencer;
        for i in 0..SONG_SLOTS {
            imp.sequencer.beats()[i].add(tracks_listener, i as u8);
            imp.sequencer.beats()[i].add(sequencer_listener, i as u8);
        }
        for i in 0..SONG_SLOTS {
            imp.clock.add(&mut imp.sequencer.beats()[i] as *mut pulse_timer::PulseTimer);
        }
        let fade: *mut pulse_timer::PulseTimer = imp.tracks.fade_timer();
        imp.clock.add(fade);
        let self_listener: *mut dyn clock::Listener = &mut *imp;
        imp.clock.add(self_listener);

        imp.sequencer.reset(0);
        imp.voices.reset(0);
        imp.tracks.reset();
        reset_render_state(&mut imp.render_state, 0);
        imp.clock.reset(0);
        imp
    }

    /// Applies one batch of events in arrival order: notes allocate, track events rewrite parameters,
    /// effects apply.
    fn dispatch_mailbox(&mut self) {
        let mailbox: *mut Mailbox = &mut *self.mailbox;
        let this: &mut Impl = self;
        // SAFETY: the mailbox is a boxed field of this owner and the drain closure does not touch it.
        unsafe {
            (*mailbox).drain(|event| match event {
                Event::NoteOn(n) => {
                    let tracks: *mut TrackService = &mut *this.tracks;
                    // SAFETY: track() returns a pointer into the tracks' table, which apply_note_on
                    // may rewrite; the reads below happen after it, as in the C++.
                    let t: *mut Track = (*tracks).track(n.sequence, n.track);
                    (*tracks).apply_note_on(&n);
                    let mut note = n;
                    // A ProgramSelector earlier in this batch is already applied: the note resolves
                    // with the program in force at this point of the event order.
                    note.program = (*t).config.program;
                    let query = ProgramQuery {
                        program: note.program,
                        key: (*t).config.parameters.transposed_key(note.kernel_key),
                        layer_mask: (*t).config.parameters.layer_mask(),
                        notes_suppressed: (*t).config.parameters.notes_suppressed(),
                    };
                    let patches: PatchList = (*this.programs).resolve(&query);
                    if patches.count != 0 {
                        (*this.voices).note_on(&note, &patches, &*t);
                    }
                }
                Event::NoteOff(n) => (*this.voices).note_off(n.instance, n.tick),
                Event::ReleaseKey(r) => {
                    let instance =
                        (*this.tracks).release_key(r.sequence, r.track, r.kernel_key, r.tick);
                    if instance != 0 {
                        (*this.voices).note_off(instance, r.tick);
                    }
                }
                Event::Track(e) => {
                    (*this.tracks).apply_track(&e);
                    match e.kind {
                        TrackEventKind::Controller
                        | TrackEventKind::Program
                        | TrackEventKind::ProgramSelector
                        | TrackEventKind::Volume
                        | TrackEventKind::Pan
                        | TrackEventKind::Refresh => {
                            (*this.voices).refresh_track(e.sequence, e.track, e.tick)
                        }
                        TrackEventKind::SequenceVolume
                        | TrackEventKind::SequencePan
                        | TrackEventKind::SequenceFade
                        | TrackEventKind::SequenceFadeStep => {
                            (*this.voices).refresh_sequence(e.sequence, e.tick)
                        }
                        TrackEventKind::PreviousKey => {}
                    }
                }
                Event::Effect(e) => {
                    if e.kind == EffectEventKind::Preset {
                        (*this.effects).select(e.preset);
                    } else {
                        (*this.effects).parameter(e.index, e.value);
                    }
                }
            });
        }
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
                        self.voices.service_queue(if timer_due { timer } else { now });
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
                    &mut self.bank,
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
            self.effects.mix_block(&self.buses, &mut self.stereo, input, output);
            if let Some(sink) = &self.sink {
                let stereo: &[f32] = &self.stereo.samples[..n * 2];
                sink.lock().unwrap_or_else(PoisonError::into_inner).write(stereo, n);
            }
            self.worker_frames += n as u64;
        }
    }

    fn advance_worker(&mut self, tick: u64) {
        let tick = tick.max(self.worker_tick);
        while self.worker_tick < tick {
            let next = tick.min(self.clock.next_tick());
            self.render_until(next);
            while self.clock.next_tick() <= next {
                self.clock.advance_one();
            }
            self.worker_tick = next;
        }
    }
}

impl clock::Listener for Impl {
    fn clock_tick(&mut self, _tick: u64) {
        self.dispatch_mailbox();
    }
}

/// `floor(tick * SAMPLE_RATE / MAIN_CLOCK)`, overflow-free.
pub fn frames_at(tick: u64) -> u64 {
    tick / u64::from(MAIN_CLOCK) * u64::from(SAMPLE_RATE)
        + tick % u64::from(MAIN_CLOCK) * u64::from(SAMPLE_RATE) / u64::from(MAIN_CLOCK)
}

impl Core {
    pub const BLOCK_FRAMES: usize = block::BLOCK_FRAMES;

    /// `sound_rom` and `sample_words` must outlive the Core. Panics when the ROM is not supported (the
    /// checks of `hle::validate()`).
    pub fn new(sound_rom: &'static [u8], sample_words: &'static [u16]) -> Core {
        Core { impl_: Impl::new(sound_rom, sample_words) }
    }

    /// None drops the rendered frames (fast-forward); the engine state evolves identically.
    pub fn set_pcm_sink(&mut self, sink: Option<SharedPcmSink>) {
        self.impl_.sink = sink;
    }

    /// Sequencer events up to and including this tick have fired.
    pub fn tick(&self) -> u64 {
        self.impl_.worker_tick
    }

    /// Frames handed to the sink so far.
    pub fn frames(&self) -> u64 {
        self.impl_.worker_frames
    }

    pub fn render_stats(&self) -> RenderStats {
        RenderStats {
            voices_started: self.impl_.voices.launched(),
            voices_active: self.impl_.voices.active_voices(),
        }
    }

    pub fn frames_at(tick: u64) -> u64 {
        frames_at(tick)
    }

    /// Each input first runs advance(tick) and then applies at tick().
    pub fn advance(&mut self, tick: u64) {
        self.impl_.advance_worker(tick);
    }

    /// Applies one parsed mailbox command packet at tick(). See `CommandPacket::parse` for the framing
    /// rules.
    pub fn command(&mut self, packet: &CommandPacket, instance: u64, tick: u64) {
        self.advance(tick);
        self.impl_.sequencer.command(packet, instance, self.impl_.worker_tick);
        self.impl_.dispatch_mailbox();
    }

    pub fn reset(&mut self, tick: u64) {
        self.advance(tick);
        let worker_tick = self.impl_.worker_tick;
        self.impl_.sequencer.reset(worker_tick);
        self.impl_.mailbox.clear();
        // Voices first: their Stop events still see the tracks they point at.
        self.impl_.voices.reset(worker_tick);
        self.impl_.tracks.reset();
        reset_render_state(&mut self.impl_.render_state, worker_tick);
        self.impl_.clock.reset(worker_tick);
        self.impl_.effects.reset();
        self.impl_.gain.fill(1.0);
        // The sound CPU's cold-boot bank, as in sequencer.reset.
        self.impl_.bank.set_bank(1);
    }

    /// `page` = the value the Audio implementation passes to `set_sound_bank`.
    pub fn set_bank(&mut self, page: u8, tick: u64) {
        self.advance(tick);
        self.impl_.bank.set_bank(page);
        let worker_tick = self.impl_.worker_tick;
        self.impl_.sequencer.set_bank(page, worker_tick);
    }

    /// `raw` = mailbox byte 0x7fa (channel 0) / 0x7fb (channel 1); only raw & 63 is used.
    pub fn set_gain(&mut self, channel: u32, raw: u8, tick: u64) {
        self.advance(tick);
        self.impl_.gain[(channel & 1) as usize] =
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
