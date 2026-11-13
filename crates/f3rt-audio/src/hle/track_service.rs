//! `audio/hle/track_service.hpp` + `track_service.cpp`: the per-track parameter records, the note
//! durations and the per-sequence volume / fade mix.
//!
//! Owns everything that is sequencing state rather than sequencer position. Parameters are only
//! written through `apply_track`; `start_sequence`, `stop_sequence` and `park_sequence` are lifecycle
//! calls from the Sequencer.

use crate::hle::events::{
    Event, NoteOffEvent, NoteOnEvent, TrackEvent, TrackEventKind, NO_PROGRAM, SONG_SLOTS, TRACK_COUNT,
};
use crate::hle::mailbox::Mailbox;
use crate::hle::note_pool::{self, NotePool, NoteSink};
use crate::hle::pulse_timer::PulseTimer;
use crate::hle::track_parameters::TrackParameters;

/// The sound CPU's channel table (c128a8): 56 physical channels, allocated 56..1. A track holds one
/// while its sequence is active; the previous-key byte outlives release, so portamento must not use
/// logical-track history.
pub const MAX_TRACK_CHANNEL: u8 = 56;

/// The parameters a sequence header supplies for one track: the 0x38-byte record the firmware keeps
/// per track, the selected program id and the program-selector bank.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct TrackConfig {
    pub parameters: TrackParameters,
    pub program: u16,
    pub bank: u8,
}

impl Default for TrackConfig {
    fn default() -> Self {
        TrackConfig { parameters: TrackParameters::default(), program: NO_PROGRAM, bank: 0 }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug, Default)]
#[repr(u8)]
pub enum TrackStatus {
    #[default]
    Inactive, // unallocated, or no stream / no program to play
    Streaming, // reading events from the ROM track stream
    Ended,     // reached the end of the stream; the track stays live for direct commands
}

/// The live sequencing state of one track of one sequence.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct Track {
    pub config: TrackConfig,
    pub sequence_volume: u8,
    pub cursor: usize,
    pub countdown: u32,
    pub head: u16,
    pub physical: u8,
    pub status: TrackStatus,
}

impl Default for Track {
    fn default() -> Self {
        Track {
            config: TrackConfig::default(),
            sequence_volume: 127,
            cursor: 0,
            countdown: 1,
            head: note_pool::NO_NOTE,
            physical: 0,
            status: TrackStatus::Inactive,
        }
    }
}

impl Track {
    pub fn is_live(&self) -> bool {
        self.status != TrackStatus::Inactive
    }

    pub fn is_streaming(&self) -> bool {
        self.status == TrackStatus::Streaming
    }
}

/// Pushes the note-off into the mailbox; the dispatcher releases the voice.
pub struct MailboxNoteSink<'a> {
    pub mailbox: &'a mut Mailbox,
}

impl NoteSink for MailboxNoteSink<'_> {
    fn note_off(&mut self, instance: u64, tick: u64) {
        self.mailbox.push(Event::NoteOff(NoteOffEvent { instance, tick }));
    }
}

// Program-selector value ranges; the extended-selector bit lives on TrackParameters.
const SELECTOR_INTERNAL_MIN: u8 = 0x64;
const SELECTOR_INTERNAL_MAX: u8 = 0x7b;
const SELECTOR_SPECIAL_7B: u8 = 0x7b;
const SELECTOR_BANK_BASE: u8 = 0x7c;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
struct Mix {
    volume: u16,
    fade_step: i16,
    fade_target: u8,
}

impl Default for Mix {
    fn default() -> Self {
        Mix { volume: 0x7fff, fade_step: 0, fade_target: 127 }
    }
}

pub struct TrackService {
    tracks: [[Track; TRACK_COUNT]; SONG_SLOTS],
    active: [bool; SONG_SLOTS],
    mix: [Mix; SONG_SLOTS],
    notes: NotePool,
    channel_used: [bool; MAX_TRACK_CHANNEL as usize + 1],
    channel_previous_key: [u8; MAX_TRACK_CHANNEL as usize + 1],
    fade: PulseTimer,
}

impl Default for TrackService {
    fn default() -> Self {
        Self::new()
    }
}

impl TrackService {
    pub fn new() -> TrackService {
        TrackService {
            tracks: [[Track::default(); TRACK_COUNT]; SONG_SLOTS],
            active: [false; SONG_SLOTS],
            mix: [Mix::default(); SONG_SLOTS],
            notes: NotePool::new(),
            channel_used: [false; MAX_TRACK_CHANNEL as usize + 1],
            channel_previous_key: [0; MAX_TRACK_CHANNEL as usize + 1],
            fade: PulseTimer::new(12, 1),
        }
    }

    pub fn reset(&mut self) {
        self.tracks = [[Track::default(); TRACK_COUNT]; SONG_SLOTS];
        self.active = [false; SONG_SLOTS];
        self.mix = [Mix::default(); SONG_SLOTS];
        self.notes.reset();
        self.channel_used.fill(false);
        self.channel_previous_key.fill(0);
        self.fade.reset();
    }

    pub fn tracks(&mut self, sequence: u8) -> &mut [Track; TRACK_COUNT] {
        &mut self.tracks[sequence as usize]
    }

    /// ---- lifecycle, called synchronously by the Sequencer before it dispatches a pulse's events.
    pub fn start_sequence(
        &mut self,
        sequence: u8,
        initial: &[TrackConfig; TRACK_COUNT],
        tick: u64,
        mailbox: &mut Mailbox,
    ) {
        let index = sequence as usize;
        let mut sink = MailboxNoteSink { mailbox };
        for t in self.tracks[index].iter_mut() {
            self.notes.release_all(&mut t.head, &mut sink, tick);
        }
        for i in 0..TRACK_COUNT {
            let physical = self.tracks[index][i].physical;
            if physical != 0 {
                self.release_channel(physical);
            }
            self.tracks[index][i].physical = 0;
        }
        self.reset_mix(sequence);
        let volume = (self.mix[index].volume >> 8) as u8;
        // `i` indexes both the track table and the caller's initial records, as in the C++.
        #[allow(clippy::needless_range_loop)]
        for i in 0..TRACK_COUNT {
            self.tracks[index][i].config = initial[i];
            if self.tracks[index][i].config.program != NO_PROGRAM {
                let physical = self.allocate_channel();
                self.tracks[index][i].physical = physical;
            }
            self.tracks[index][i].cursor = 0;
            self.tracks[index][i].countdown = 1;
            self.tracks[index][i].status = TrackStatus::Inactive;
            self.tracks[index][i].sequence_volume = volume;
        }
        self.active[sequence as usize] = true;
    }

    pub fn stop_sequence(&mut self, sequence: u8, tick: u64, mailbox: &mut Mailbox) {
        let index = sequence as usize;
        let mut sink = MailboxNoteSink { mailbox };
        for i in 0..TRACK_COUNT {
            self.notes.release_all(&mut self.tracks[index][i].head, &mut sink, tick);
            let physical = self.tracks[index][i].physical;
            if physical != 0 {
                self.release_channel(physical);
            }
            self.tracks[index][i].physical = 0;
        }
        self.active[index] = false;
    }

    pub fn park_sequence(&mut self, sequence: u8, tick: u64, mailbox: &mut Mailbox) {
        self.release_notes(sequence, tick, mailbox);
    }

    /// Note-offs only: the physical channels and the track records stay.
    pub fn release_notes(&mut self, sequence: u8, tick: u64, mailbox: &mut Mailbox) {
        let index = sequence as usize;
        let mut sink = MailboxNoteSink { mailbox };
        for i in 0..TRACK_COUNT {
            self.notes.release_all(&mut self.tracks[index][i].head, &mut sink, tick);
        }
    }

    pub fn copy_mix(&mut self, from: u8, to: u8) {
        self.mix[to as usize] = self.mix[from as usize];
        let volume = (self.mix[to as usize].volume >> 8) as u8;
        self.set_sequence_volume(to, volume);
    }

    /// The channel's last factory key: read when a sequence restarts, before any of its voices exist.
    pub fn previous_key(&self, channel: u8) -> u8 {
        self.channel_previous_key[channel as usize]
    }

    /// ---- event application, called only by the mailbox dispatcher.
    pub fn apply_note_on(&mut self, event: &NoteOnEvent) {
        let index = event.sequence as usize;
        let track = event.track as usize;
        self.notes.allocate_note(
            &mut self.tracks[index][track].head,
            event.instance,
            event.key,
            event.kernel_key,
            event.duration,
        );
    }

    pub fn apply_track(&mut self, event: &TrackEvent) {
        let t = &mut self.tracks[event.sequence as usize][event.track as usize];
        match event.kind {
            TrackEventKind::Controller => {
                if !t.config.parameters.set_controller(event.index as usize, event.value) {
                    panic!("HLE controller outside track parameter record");
                }
            }

            TrackEventKind::Program => {
                t.config.program = event.program;
                t.config.parameters.set_program_image(event.program);
            }

            TrackEventKind::ProgramSelector => {
                let parameters = &mut t.config.parameters;
                parameters.set_selector(event.value);
                if event.value >= SELECTOR_INTERNAL_MIN && event.value < SELECTOR_INTERNAL_MAX {
                } else if event.value == SELECTOR_SPECIAL_7B {
                    parameters.set_extended_selector(true);
                } else if event.value >= SELECTOR_BANK_BASE {
                    t.config.bank = event.value.wrapping_sub(SELECTOR_BANK_BASE);
                } else {
                    parameters.set_extended_selector(false);
                    t.config.program = ((t.config.bank as u16) << 14) | event.value as u16;
                    parameters.set_program_image(t.config.program);
                    // c144e2 passes the track with bit7 set: c0edda clears the bank after selection.
                    t.config.bank = 0;
                }
            }

            TrackEventKind::Volume => t.config.parameters.set_volume(event.value),

            TrackEventKind::Pan => t.config.parameters.set_pan(event.value),

            TrackEventKind::PreviousKey => {
                t.config.parameters.set_previous_key(event.value);
                self.channel_previous_key[t.physical as usize] = event.value;
            }

            TrackEventKind::SequenceVolume => {
                let mix = &mut self.mix[event.sequence as usize];
                mix.volume = ((event.value as u16) << 8) | (mix.volume as u8) as u16;
                mix.fade_step = 0;
                let volume = (mix.volume >> 8) as u8;
                self.set_sequence_volume(event.sequence, volume);
            }

            TrackEventKind::SequencePan => {
                let index = event.sequence as usize;
                for i in 0..TRACK_COUNT {
                    if self.tracks[index][i].is_live() {
                        self.tracks[index][i].config.parameters.set_pan(event.value);
                    }
                }
            }

            TrackEventKind::SequenceFade => {
                let mix = &mut self.mix[event.sequence as usize];
                mix.volume = ((event.start as u16) << 8) | (mix.volume as u8) as u16;
                mix.fade_target = event.target;
                let difference = ((event.target.wrapping_sub(event.start) as u16) << 8) as i16;
                let product = (difference as i32) * (event.rate as i32);
                mix.fade_step = ((product as u32).wrapping_mul(2) >> 16) as i16;
                let volume = (mix.volume >> 8) as u8;
                self.set_sequence_volume(event.sequence, volume);
            }

            TrackEventKind::Refresh => {}

            TrackEventKind::SequenceFadeStep => {
                let mix = &mut self.mix[event.sequence as usize];
                if mix.fade_step != 0 {
                    mix.volume = mix.volume.wrapping_add(mix.fade_step as u16);
                    let current = (mix.volume >> 8) as u8 as i8;
                    let target = mix.fade_target as i8;
                    if (mix.fade_step > 0 && current >= target)
                        || (mix.fade_step < 0 && current < target)
                    {
                        mix.volume = ((mix.fade_target as u16) << 8) | (mix.volume as u8) as u16;
                        mix.fade_step = 0;
                    }
                    let volume = (mix.volume >> 8) as u8;
                    self.set_sequence_volume(event.sequence, volume);
                }
            }
        }
    }

    /// Unlinks the track's note of `kernel_key`; returns the instance to release (0 when none).
    pub fn release_key(&mut self, sequence: u8, track: u8, kernel_key: u8, _tick: u64) -> u64 {
        self.notes.release_matching_key(
            &mut self.tracks[sequence as usize][track as usize].head,
            kernel_key,
        )
    }

    /// The sequence fade pulse; the owner calls `fade_pulse` when it fires.
    pub fn fade_timer(&mut self) -> &mut PulseTimer {
        &mut self.fade
    }

    fn reset_mix(&mut self, sequence: u8) {
        self.mix[sequence as usize] = Mix::default();
    }

    /// The sequence's BPM note pulse: steps every note duration of its tracks.
    pub fn note_pulse(&mut self, sequence: u8, tick: u64, mailbox: &mut Mailbox) {
        let index = sequence as usize;
        let mut sink = MailboxNoteSink { mailbox };
        for i in 0..TRACK_COUNT {
            self.notes.step_durations(&mut self.tracks[index][i].head, &mut sink, tick);
        }
    }

    /// The sequence fade pulse: queues one fade step per fading sequence.
    pub fn fade_pulse(&mut self, tick: u64, mailbox: &mut Mailbox) {
        for sequence in 0..SONG_SLOTS {
            if !self.active[sequence] || self.mix[sequence].fade_step == 0 {
                continue;
            }
            mailbox.push(Event::Track(TrackEvent {
                kind: TrackEventKind::SequenceFadeStep,
                tick,
                sequence: sequence as u8,
                ..TrackEvent::default()
            }));
        }
    }

    fn set_sequence_volume(&mut self, sequence: u8, volume: u8) {
        let index = sequence as usize;
        for i in 0..TRACK_COUNT {
            if self.tracks[index][i].is_live() {
                self.tracks[index][i].sequence_volume = volume;
            }
        }
    }

    fn allocate_channel(&mut self) -> u8 {
        let mut channel = MAX_TRACK_CHANNEL;
        while channel > 0 {
            if !self.channel_used[channel as usize] {
                self.channel_used[channel as usize] = true;
                return channel;
            }
            channel -= 1;
        }
        0
    }

    fn release_channel(&mut self, channel: u8) {
        if channel > 0 && channel <= MAX_TRACK_CHANNEL {
            self.channel_used[channel as usize] = false;
        }
    }
}
