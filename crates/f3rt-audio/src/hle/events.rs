//! The sample-rate/clock constants and the mailbox event records.

/// The voice and effects stages run on the ES5505 output grid (clock rate 15238090 Hz, 32 voices:
/// 15238090 / (16 * 32) = 29761 + 229/256 = 29761.89453125 Hz, rounded to 29762 Hz per 12's PCB
/// observation over MAME's integer-truncation bug) and the engine outputs at that rate.
pub const SAMPLE_RATE: u32 = 29762;
pub const MAIN_CLOCK: u32 = 16_000_000;
/// ROM sequencer's observed 1 kHz service.
pub const TIMER_TICKS: u32 = 16000;
/// Sound CPU cycles (16 MHz) the real driver spends per started voice while a sequencer pulse starts
/// several notes; see DriverClock in note_timing.
pub const DRIVER_KEYON_CYCLES: u32 = 20408;

/// Sequence slots the ROM can start, and the logical tracks each sequence holds.
pub const SONG_SLOTS: usize = 100;
pub const TRACK_COUNT: usize = 8;
pub const NO_PROGRAM: u16 = 0xffff;

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct NoteOnEvent {
    pub instance: u64,
    pub tick: u64,
    pub duration: u16,
    pub program: u16,
    pub sequence: u8,
    pub track: u8,
    pub key: u8,
    pub kernel_key: u8,
    pub velocity: u8,
    pub previous_key: u8,
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct NoteOffEvent {
    pub instance: u64,
    pub tick: u64,
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct ReleaseKeyEvent {
    pub tick: u64,
    pub sequence: u8,
    pub track: u8,
    pub kernel_key: u8,
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum TrackEventKind {
    #[default]
    Controller,
    Program,
    ProgramSelector,
    Volume,
    Pan,
    PreviousKey,
    SequenceVolume,
    SequencePan,
    SequenceFade,
    SequenceFadeStep,
    /// The track record was rewritten as a whole: re-read it for live voices, no field changes.
    Refresh,
}

/// One parameter write on a track, or one mix change on a sequence. Applied by TrackService in
/// arrival order.
#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct TrackEvent {
    pub kind: TrackEventKind,
    pub tick: u64,
    pub sequence: u8,
    pub track: u8,
    /// Controller / Volume / Pan / PreviousKey.
    pub index: u8,
    pub value: u8,
    /// Program.
    pub program: u16,
    /// SequenceFade.
    pub start: u8,
    pub target: u8,
    /// SequenceFade rate multiplier (RATE_TABLE).
    pub rate: i16,
}

impl TrackEvent {
    fn at(kind: TrackEventKind, tick: u64, sequence: u8, track: u8) -> TrackEvent {
        TrackEvent { kind, tick, sequence, track, ..TrackEvent::default() }
    }

    pub fn controller(tick: u64, sequence: u8, track: u8, index: u8, value: u8) -> TrackEvent {
        TrackEvent { index, value, ..Self::at(TrackEventKind::Controller, tick, sequence, track) }
    }

    pub fn program(tick: u64, sequence: u8, track: u8, program: u16) -> TrackEvent {
        TrackEvent { program, ..Self::at(TrackEventKind::Program, tick, sequence, track) }
    }

    pub fn program_selector(tick: u64, sequence: u8, track: u8, value: u8) -> TrackEvent {
        TrackEvent { value, ..Self::at(TrackEventKind::ProgramSelector, tick, sequence, track) }
    }

    pub fn volume(tick: u64, sequence: u8, track: u8, value: u8) -> TrackEvent {
        TrackEvent { value, ..Self::at(TrackEventKind::Volume, tick, sequence, track) }
    }

    pub fn pan(tick: u64, sequence: u8, track: u8, value: u8) -> TrackEvent {
        TrackEvent { value, ..Self::at(TrackEventKind::Pan, tick, sequence, track) }
    }

    pub fn previous_key(tick: u64, sequence: u8, track: u8, key: u8) -> TrackEvent {
        TrackEvent { value: key, ..Self::at(TrackEventKind::PreviousKey, tick, sequence, track) }
    }

    pub fn sequence_volume(tick: u64, sequence: u8, value: u8) -> TrackEvent {
        TrackEvent { value, ..Self::at(TrackEventKind::SequenceVolume, tick, sequence, 0) }
    }

    pub fn sequence_pan(tick: u64, sequence: u8, value: u8) -> TrackEvent {
        TrackEvent { value, ..Self::at(TrackEventKind::SequencePan, tick, sequence, 0) }
    }

    pub fn sequence_fade(tick: u64, sequence: u8, start: u8, target: u8, rate: i16) -> TrackEvent {
        TrackEvent {
            start,
            target,
            rate,
            ..Self::at(TrackEventKind::SequenceFade, tick, sequence, 0)
        }
    }

    pub fn sequence_fade_step(tick: u64, sequence: u8) -> TrackEvent {
        Self::at(TrackEventKind::SequenceFadeStep, tick, sequence, 0)
    }

    pub fn refresh(tick: u64, sequence: u8, track: u8) -> TrackEvent {
        Self::at(TrackEventKind::Refresh, tick, sequence, track)
    }
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
#[repr(u8)]
pub enum EffectEventKind {
    #[default]
    Preset,
    Parameter,
}

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
pub struct EffectEvent {
    pub kind: EffectEventKind,
    pub tick: u64,
    pub preset: u8,
    pub index: u8,
    pub value: u8,
}

impl EffectEvent {
    pub fn preset(tick: u64, preset: u8) -> EffectEvent {
        EffectEvent { kind: EffectEventKind::Preset, tick, preset, ..EffectEvent::default() }
    }

    pub fn parameter(tick: u64, index: u8, value: u8) -> EffectEvent {
        EffectEvent { kind: EffectEventKind::Parameter, tick, index, value, ..EffectEvent::default() }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum Event {
    NoteOn(NoteOnEvent),
    NoteOff(NoteOffEvent),
    ReleaseKey(ReleaseKeyEvent),
    Track(TrackEvent),
    Effect(EffectEvent),
}

impl Default for Event {
    fn default() -> Self {
        Event::NoteOn(NoteOnEvent::default())
    }
}
