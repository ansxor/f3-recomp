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
use crate::hle::rom_view::BOOT_BANK;
use crate::hle::sequencer::Sequencer;
use crate::hle::sound_bank::SoundBank;
use crate::hle::synthesis::VoiceRenderState;
use crate::hle::track_service::TrackService;
use crate::hle::voice_service::VoiceService;

const OUTPUT_BOOST: f32 = 12.0;
const ESP_HEADROOM: f32 = 0.18;

struct MixGains {
    pre_dsp: [f32; 2],
    post_dsp: [f32; 2],
}

/// Receives rendered stereo frames in render order, in blocks of 1..Core::BLOCK_FRAMES frames.
pub trait PcmSink: Send {
    /// Interleaved L,R in OTIS/ESP channel order (the left/right exchange of the DAC is applied by the
    /// final consumer).
    fn write(&mut self, stereo: &[f32], frames: usize);
}

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

fn grid_after(tick: u64) -> u64 {
    (tick / TIMER_TICKS as u64 + 1) * TIMER_TICKS as u64
}

/// `floor(tick * SAMPLE_RATE / MAIN_CLOCK)`, overflow-free.
pub fn frames_at(tick: u64) -> u64 {
    tick / u64::from(MAIN_CLOCK) * u64::from(SAMPLE_RATE)
        + tick % u64::from(MAIN_CLOCK) * u64::from(SAMPLE_RATE) / u64::from(MAIN_CLOCK)
}

impl Core {
    /// `Err` names why the sound program or the samples cannot be driven.
    pub fn try_new(
        sound_rom: Arc<[u8]>,
        sample_words: Box<[u16]>,
        layout: &'static DriverLayout,
    ) -> Result<Core, &'static str> {
        let bank = SoundBank::try_new(Arc::clone(&sound_rom), sample_words, layout)?;
        let sequencer = Sequencer::try_new(Arc::clone(&sound_rom), layout)?;
        let voices = VoiceService::new(&bank);
        let mut core = Core {
            bank,
            effects: Box::new(Effects::new(sound_rom, layout)),
            next_tick: grid_after(0),
            mailbox: Mailbox::new(),
            tracks: Box::new(TrackService::new()),
            voices,
            sequencer,
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
        Ok(core)
    }

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
            Event::NoteOn(note) => {
                tracks.apply_note_on(&note);
                let track = tracks.track(note.sequence, note.track);
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
                    TrackEventKind::Controller { .. }
                    | TrackEventKind::Program(_)
                    | TrackEventKind::ProgramSelector(_)
                    | TrackEventKind::Volume(_)
                    | TrackEventKind::Pan(_)
                    | TrackEventKind::Refresh => {
                        voices.refresh_track(e.sequence, e.track, bank, tracks)
                    }
                    TrackEventKind::SequenceVolume(_)
                    | TrackEventKind::SequencePan(_)
                    | TrackEventKind::SequenceFade { .. }
                    | TrackEventKind::SequenceFadeStep => {
                        voices.refresh_sequence(e.sequence, bank, tracks)
                    }
                    TrackEventKind::PreviousKey(_) => {}
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
        let gains = self.mix_gains();
        while self.worker_frames < end {
            let frames = (block::BLOCK_FRAMES as u64).min(end - self.worker_frames) as usize;
            self.render_bus_block(frames);
            self.stereo.resize(frames);
            self.effects
                .mix_block(&self.buses, &mut self.stereo, gains.pre_dsp, gains.post_dsp);
            self.write_sink(frames);
            self.worker_frames += frames as u64;
        }
    }

    fn mix_gains(&self) -> MixGains {
        let (pre_dsp, post_dsp_headroom) = if self.effects.program() == Program::Overdrive {
            (1.0, ESP_HEADROOM)
        } else {
            (ESP_HEADROOM, 1.0)
        };
        let post_dsp = self
            .gain
            .map(|channel| OUTPUT_BOOST * post_dsp_headroom * channel);
        MixGains {
            pre_dsp: [pre_dsp; 2],
            post_dsp,
        }
    }

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

    pub fn rendered_frames(&self) -> u64 {
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
        self.voices.reset(worker_tick, &self.bank);
        self.tracks.reset();
        self.render_state.reset(worker_tick);
        self.next_tick = grid_after(worker_tick);
        self.effects.reset();
        self.gain.fill(1.0);
        // The sound CPU's cold-boot bank, as in sequencer.reset.
        self.bank.set_bank(BOOT_BANK);
    }

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
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct RenderStats {
    pub voices_started: u64,
    pub voices_active: u32,
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::hle::driver_layout::STANDARD;
    use crate::hle::rom_view::SOUND_ROM_IMAGE_SIZE;
    use crate::memory_map::SOUND_ROM_SIZE;

    fn zeros(bytes: usize) -> Arc<[u8]> {
        vec![0u8; bytes].into()
    }

    fn reason(sound_rom: Arc<[u8]>, sample_words: usize) -> Option<&'static str> {
        let samples = vec![0u16; sample_words].into_boxed_slice();
        Core::try_new(sound_rom, samples, &STANDARD).err()
    }

    #[test]
    fn an_undersized_sound_rom_is_an_error() {
        let rom = zeros(SOUND_ROM_SIZE - 1);
        assert_eq!(
            reason(rom, SOUND_ROM_SIZE),
            Some("HLE requires mapped sound and power-of-two sample ROM regions")
        );
    }

    #[test]
    fn undersized_or_odd_sized_samples_are_an_error() {
        let rom = zeros(SOUND_ROM_IMAGE_SIZE as usize);
        for words in [SOUND_ROM_SIZE / 2, SOUND_ROM_SIZE + SOUND_ROM_SIZE / 2] {
            assert_eq!(
                reason(Arc::clone(&rom), words),
                Some("HLE requires mapped sound and power-of-two sample ROM regions")
            );
        }
    }

    #[test]
    fn a_blank_sound_rom_has_no_sequence_bank() {
        let rom = zeros(SOUND_ROM_IMAGE_SIZE as usize);
        assert_eq!(reason(rom, SOUND_ROM_SIZE), Some("unsupported HLE sequence bank layout"));
    }
}
