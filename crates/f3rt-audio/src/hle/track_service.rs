//! The per-track parameter records, the note durations and the per-sequence volume / fade mix.
//!
//! Owns everything that is sequencing state rather than sequencer position. Parameters are only
//! written through `apply_track`; `start_sequence`, `stop_sequence` and `park_sequence` are lifecycle
//! calls from the Sequencer.

use crate::hle::events::{
    Event, NoteOffEvent, NoteOnEvent, TrackEvent, TrackEventKind, NO_PROGRAM, SONG_SLOTS, TRACK_COUNT,
};
use crate::hle::mailbox::Mailbox;
use crate::hle::note_pool::{self, NotePool};
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
    /// Unallocated, or no stream / no program to play.
    Inactive,
    /// Reading events from the ROM track stream.
    Streaming,
    /// Reached the end of the stream; the track stays live for direct commands.
    Ended,
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

/// A note-off sink that queues the event; the mailbox dispatcher releases the voice.
fn push_note_off(mailbox: &mut Mailbox) -> impl FnMut(u64, u64) + '_ {
    |instance, tick| mailbox.push(Event::NoteOff(NoteOffEvent { instance, tick }))
}

// Program-selector value ranges; the extended-selector bit lives on TrackParameters.
const SELECTOR_INTERNAL_MIN: u8 = 0x64;
const SELECTOR_SPECIAL_7B: u8 = 0x7b;
const SELECTOR_BANK_BASE: u8 = 0x7c;

/// The sequence's volume: the high byte is the level tracks see, the low byte is carried across
/// writes of the level.
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

impl Mix {
    fn level(&self) -> u8 {
        (self.volume >> 8) as u8
    }

    fn set_level(&mut self, level: u8) {
        self.volume = (u16::from(level) << 8) | (self.volume & 0xff);
    }
}

/// The physical channel table: channels are handed out from the top, and a channel's previous
/// factory key outlives its release.
struct Channels {
    used: [bool; MAX_TRACK_CHANNEL as usize + 1],
    previous_key: [u8; MAX_TRACK_CHANNEL as usize + 1],
}

impl Channels {
    fn new() -> Channels {
        Channels {
            used: [false; MAX_TRACK_CHANNEL as usize + 1],
            previous_key: [0; MAX_TRACK_CHANNEL as usize + 1],
        }
    }

    fn reset(&mut self) {
        *self = Channels::new();
    }

    /// The highest free channel; 0 when all are taken.
    fn allocate(&mut self) -> u8 {
        let Some(channel) = (1..=MAX_TRACK_CHANNEL).rev().find(|&c| !self.used[usize::from(c)])
        else {
            return 0;
        };
        self.used[usize::from(channel)] = true;
        channel
    }

    /// Channel 0 means "none held".
    fn release(&mut self, channel: u8) {
        if channel > 0 && channel <= MAX_TRACK_CHANNEL {
            self.used[usize::from(channel)] = false;
        }
    }
}

pub struct TrackService {
    tracks: [[Track; TRACK_COUNT]; SONG_SLOTS],
    active: [bool; SONG_SLOTS],
    mix: [Mix; SONG_SLOTS],
    notes: NotePool,
    channels: Channels,
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
            channels: Channels::new(),
            fade: PulseTimer::new(12, 1),
        }
    }

    pub fn reset(&mut self) {
        self.tracks = [[Track::default(); TRACK_COUNT]; SONG_SLOTS];
        self.active = [false; SONG_SLOTS];
        self.mix = [Mix::default(); SONG_SLOTS];
        self.notes.reset();
        self.channels.reset();
        self.fade.reset();
    }

    pub fn tracks(&mut self, sequence: u8) -> &mut [Track; TRACK_COUNT] {
        &mut self.tracks[usize::from(sequence)]
    }

    pub fn track(&self, sequence: u8, track: u8) -> &Track {
        &self.tracks[usize::from(sequence)][usize::from(track)]
    }

    pub fn track_mut(&mut self, sequence: u8, track: u8) -> &mut Track {
        &mut self.tracks[usize::from(sequence)][usize::from(track)]
    }

    /// Lifecycle, called synchronously by the Sequencer before it dispatches a pulse's events.
    pub fn start_sequence(
        &mut self,
        sequence: u8,
        initial: &[TrackConfig; TRACK_COUNT],
        tick: u64,
        mailbox: &mut Mailbox,
    ) {
        let index = usize::from(sequence);
        for track in self.tracks[index].iter_mut() {
            self.notes.release_all(&mut track.head, tick, push_note_off(mailbox));
            self.channels.release(track.physical);
            track.physical = 0;
        }

        self.mix[index] = Mix::default();
        let volume = self.mix[index].level();
        for (track, config) in self.tracks[index].iter_mut().zip(initial) {
            track.config = *config;
            if config.program != NO_PROGRAM {
                track.physical = self.channels.allocate();
            }
            track.cursor = 0;
            track.countdown = 1;
            track.status = TrackStatus::Inactive;
            track.sequence_volume = volume;
        }
        self.active[index] = true;
    }

    pub fn stop_sequence(&mut self, sequence: u8, tick: u64, mailbox: &mut Mailbox) {
        let index = usize::from(sequence);
        for track in self.tracks[index].iter_mut() {
            self.notes.release_all(&mut track.head, tick, push_note_off(mailbox));
            self.channels.release(track.physical);
            track.physical = 0;
        }
        self.active[index] = false;
    }

    pub fn park_sequence(&mut self, sequence: u8, tick: u64, mailbox: &mut Mailbox) {
        self.release_notes(sequence, tick, mailbox);
    }

    /// Note-offs only: the physical channels and the track records stay.
    pub fn release_notes(&mut self, sequence: u8, tick: u64, mailbox: &mut Mailbox) {
        for track in self.tracks[usize::from(sequence)].iter_mut() {
            self.notes.release_all(&mut track.head, tick, push_note_off(mailbox));
        }
    }

    pub fn copy_mix(&mut self, from: u8, to: u8) {
        self.mix[usize::from(to)] = self.mix[usize::from(from)];
        let volume = self.mix[usize::from(to)].level();
        self.set_sequence_volume(to, volume);
    }

    /// The channel's last factory key: read when a sequence restarts, before any of its voices exist.
    pub fn previous_key(&self, channel: u8) -> u8 {
        self.channels.previous_key[usize::from(channel)]
    }

    /// Event application, called only by the mailbox dispatcher.
    pub fn apply_note_on(&mut self, event: &NoteOnEvent) {
        let track = &mut self.tracks[usize::from(event.sequence)][usize::from(event.track)];
        self.notes.allocate_note(
            &mut track.head,
            event.instance,
            event.key,
            event.kernel_key,
            event.duration,
        );
    }

    pub fn apply_track(&mut self, event: &TrackEvent) {
        let t = &mut self.tracks[usize::from(event.sequence)][usize::from(event.track)];
        match event.kind {
            TrackEventKind::Controller => {
                if !t.config.parameters.set_controller(usize::from(event.index), event.value) {
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
                match event.value {
                    // Internal selectors only record the selector byte.
                    SELECTOR_INTERNAL_MIN..SELECTOR_SPECIAL_7B => {}
                    SELECTOR_SPECIAL_7B => parameters.set_extended_selector(true),
                    SELECTOR_BANK_BASE.. => {
                        t.config.bank = event.value.wrapping_sub(SELECTOR_BANK_BASE);
                    }
                    _ => {
                        parameters.set_extended_selector(false);
                        t.config.program = (u16::from(t.config.bank) << 14) | u16::from(event.value);
                        parameters.set_program_image(t.config.program);
                        // The driver selects with the track's bit 7 set, which clears the bank
                        // after the selection.
                        t.config.bank = 0;
                    }
                }
            }

            TrackEventKind::Volume => t.config.parameters.set_volume(event.value),

            TrackEventKind::Pan => t.config.parameters.set_pan(event.value),

            TrackEventKind::PreviousKey => {
                t.config.parameters.set_previous_key(event.value);
                self.channels.previous_key[usize::from(t.physical)] = event.value;
            }

            TrackEventKind::SequenceVolume => {
                let mix = &mut self.mix[usize::from(event.sequence)];
                mix.set_level(event.value);
                mix.fade_step = 0;
                let level = mix.level();
                self.set_sequence_volume(event.sequence, level);
            }

            TrackEventKind::SequencePan => {
                for track in self.tracks[usize::from(event.sequence)].iter_mut() {
                    if track.is_live() {
                        track.config.parameters.set_pan(event.value);
                    }
                }
            }

            TrackEventKind::SequenceFade => {
                let mix = &mut self.mix[usize::from(event.sequence)];
                mix.set_level(event.start);
                mix.fade_target = event.target;
                let difference = (u16::from(event.target.wrapping_sub(event.start)) << 8) as i16;
                let product = i32::from(difference) * i32::from(event.rate);
                mix.fade_step = ((product as u32).wrapping_mul(2) >> 16) as i16;
                let level = mix.level();
                self.set_sequence_volume(event.sequence, level);
            }

            TrackEventKind::Refresh => {}

            TrackEventKind::SequenceFadeStep => {
                let mix = &mut self.mix[usize::from(event.sequence)];
                if mix.fade_step != 0 {
                    mix.volume = mix.volume.wrapping_add(mix.fade_step as u16);
                    let current = mix.level() as i8;
                    let target = mix.fade_target as i8;
                    if (mix.fade_step > 0 && current >= target)
                        || (mix.fade_step < 0 && current < target)
                    {
                        mix.set_level(mix.fade_target);
                        mix.fade_step = 0;
                    }
                    let level = mix.level();
                    self.set_sequence_volume(event.sequence, level);
                }
            }
        }
    }

    /// Unlinks the track's note of `kernel_key`; returns the instance to release (0 when none).
    pub fn release_key(&mut self, sequence: u8, track: u8, kernel_key: u8, _tick: u64) -> u64 {
        let track = &mut self.tracks[usize::from(sequence)][usize::from(track)];
        self.notes.release_matching_key(&mut track.head, kernel_key)
    }

    /// The sequence fade pulse; the owner calls `fade_pulse` when it fires.
    pub fn fade_timer(&mut self) -> &mut PulseTimer {
        &mut self.fade
    }

    /// The sequence's BPM note pulse: steps every note duration of its tracks.
    pub fn note_pulse(&mut self, sequence: u8, tick: u64, mailbox: &mut Mailbox) {
        for track in self.tracks[usize::from(sequence)].iter_mut() {
            self.notes.step_durations(&mut track.head, tick, push_note_off(mailbox));
        }
    }

    /// The sequence fade pulse: queues one fade step per fading sequence.
    pub fn fade_pulse(&mut self, tick: u64, mailbox: &mut Mailbox) {
        let fading = self.active.iter().zip(&self.mix).enumerate();
        for (sequence, (&active, mix)) in fading {
            if active && mix.fade_step != 0 {
                mailbox.push(Event::Track(TrackEvent::sequence_fade_step(tick, sequence as u8)));
            }
        }
    }

    fn set_sequence_volume(&mut self, sequence: u8, volume: u8) {
        for track in self.tracks[usize::from(sequence)].iter_mut() {
            if track.is_live() {
                track.sequence_volume = volume;
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::hle::track_parameters::at;

    fn configs(programs: &[u16]) -> [TrackConfig; TRACK_COUNT] {
        let mut initial = [TrackConfig::default(); TRACK_COUNT];
        for (config, &program) in initial.iter_mut().zip(programs) {
            config.program = program;
        }
        initial
    }

    fn started(programs: &[u16]) -> (TrackService, Mailbox) {
        let mut service = TrackService::new();
        let mut mailbox = Mailbox::new();
        service.start_sequence(3, &configs(programs), 0, &mut mailbox);
        (service, mailbox)
    }

    fn make_live(service: &mut TrackService, sequence: u8, track: u8) {
        service.track_mut(sequence, track).status = TrackStatus::Streaming;
    }

    fn sequence_event(kind: TrackEventKind) -> TrackEvent {
        TrackEvent { kind, sequence: 3, ..TrackEvent::default() }
    }

    fn drained(mailbox: &mut Mailbox) -> Vec<Event> {
        let mut events = Vec::new();
        mailbox.drain(|event| events.push(event));
        events
    }

    #[test]
    fn channels_go_to_programmed_tracks_from_the_top() {
        let (service, _) = started(&[1, NO_PROGRAM, 2]);
        assert_eq!(service.track(3, 0).physical, MAX_TRACK_CHANNEL);
        assert_eq!(service.track(3, 1).physical, 0);
        assert_eq!(service.track(3, 2).physical, MAX_TRACK_CHANNEL - 1);
    }

    #[test]
    fn stopping_frees_channels_for_the_next_sequence() {
        let (mut service, mut mailbox) = started(&[1, 2]);
        service.stop_sequence(3, 0, &mut mailbox);
        assert_eq!(service.track(3, 0).physical, 0);

        service.start_sequence(4, &configs(&[9]), 0, &mut mailbox);
        assert_eq!(service.track(4, 0).physical, MAX_TRACK_CHANNEL);
    }

    #[test]
    fn exhausted_channels_allocate_zero() {
        let mut channels = Channels::new();
        for expected in (1..=MAX_TRACK_CHANNEL).rev() {
            assert_eq!(channels.allocate(), expected);
        }
        assert_eq!(channels.allocate(), 0);
        channels.release(7);
        assert_eq!(channels.allocate(), 7);
    }

    #[test]
    fn stopping_releases_held_notes_into_the_mailbox() {
        let (mut service, mut mailbox) = started(&[1]);
        service.apply_note_on(&NoteOnEvent {
            instance: 9,
            sequence: 3,
            track: 0,
            duration: 5,
            ..NoteOnEvent::default()
        });
        service.stop_sequence(3, 77, &mut mailbox);
        assert_eq!(
            drained(&mut mailbox),
            vec![Event::NoteOff(NoteOffEvent { instance: 9, tick: 77 })]
        );
    }

    #[test]
    fn sequence_volume_reaches_only_live_tracks() {
        let (mut service, _) = started(&[1, 1]);
        make_live(&mut service, 3, 0);

        service.apply_track(&TrackEvent { value: 0x40, ..sequence_event(TrackEventKind::SequenceVolume) });

        assert_eq!(service.track(3, 0).sequence_volume, 0x40);
        assert_eq!(service.track(3, 1).sequence_volume, 127);
    }

    #[test]
    fn fade_steps_toward_the_target_and_stops_there() {
        let (mut service, mut mailbox) = started(&[1]);
        make_live(&mut service, 3, 0);

        service.apply_track(&TrackEvent {
            start: 127,
            target: 0,
            rate: 0x100,
            ..sequence_event(TrackEventKind::SequenceFade)
        });
        assert_eq!(service.track(3, 0).sequence_volume, 127);

        service.fade_pulse(5, &mut mailbox);
        assert_eq!(drained(&mut mailbox), vec![Event::Track(TrackEvent::sequence_fade_step(5, 3))]);

        // The step is -254 per pulse on a volume whose low byte starts at 0xff.
        service.apply_track(&sequence_event(TrackEventKind::SequenceFadeStep));
        assert_eq!(service.track(3, 0).sequence_volume, 0x7f);
        service.apply_track(&sequence_event(TrackEventKind::SequenceFadeStep));
        assert_eq!(service.track(3, 0).sequence_volume, 0x7e);

        for _ in 0..1000 {
            service.apply_track(&sequence_event(TrackEventKind::SequenceFadeStep));
        }
        assert_eq!(service.track(3, 0).sequence_volume, 0);
        service.fade_pulse(6, &mut mailbox);
        assert!(drained(&mut mailbox).is_empty());
    }

    #[test]
    fn selector_bank_applies_to_the_next_program_selection() {
        let (mut service, _) = started(&[1]);
        let selector = |value| TrackEvent::program_selector(0, 3, 0, value);

        service.apply_track(&selector(0x7c + 2));
        assert_eq!(service.track(3, 0).config.bank, 2);

        service.apply_track(&selector(0x05));
        let config = service.track(3, 0).config;
        assert_eq!(config.program, (2 << 14) | 0x05);
        assert_eq!(config.parameters.word(at::program_high), config.program);
        assert_eq!(config.bank, 0);
        assert_eq!(config.parameters.selector(), 0x05);
    }

    #[test]
    fn selector_7b_sets_and_a_plain_program_clears_the_extended_flag() {
        let (mut service, _) = started(&[1]);

        service.apply_track(&TrackEvent::program_selector(0, 3, 0, 0x7b));
        assert_eq!(service.track(3, 0).config.parameters.flags() & 0x40, 0x40);

        service.apply_track(&TrackEvent::program_selector(0, 3, 0, 0x10));
        assert_eq!(service.track(3, 0).config.parameters.flags() & 0x40, 0);
    }

    #[test]
    fn internal_selectors_only_record_the_selector_byte() {
        let (mut service, _) = started(&[1]);
        let before = service.track(3, 0).config;

        service.apply_track(&TrackEvent::program_selector(0, 3, 0, 0x70));

        let after = service.track(3, 0).config;
        assert_eq!((after.program, after.bank), (before.program, before.bank));
        assert_eq!(after.parameters.selector(), 0x70);
    }

    #[test]
    fn previous_key_is_remembered_per_channel() {
        let (mut service, _) = started(&[1]);
        service.apply_track(&TrackEvent::previous_key(0, 3, 0, 66));
        assert_eq!(service.previous_key(MAX_TRACK_CHANNEL), 66);
    }
}
