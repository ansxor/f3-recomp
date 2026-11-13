//! `audio/hle/sequencer.hpp` + `sequencer.cpp`: the ROM sequence streams to mailbox events.
//!
//! It owns only sequencing position and tempo: the track records live in TrackService, and the
//! mailbox carries every note and parameter write to the services.

use crate::hle::command::{
    Command, CommandPacket, EffectParameter, EffectPreset, SequenceRef,
};
use crate::hle::events::{
    EffectEvent, EffectEventKind, Event, NoteOnEvent, ReleaseKeyEvent, TrackEvent, TrackEventKind,
    NO_PROGRAM, SONG_SLOTS, TRACK_COUNT,
};
use crate::hle::mailbox::Mailbox;
use crate::hle::pulse_timer::PulseTimer;
use crate::hle::rom_view::{RomView, SOUND_ROM_IMAGE_SIZE};
use crate::hle::sound_rom_5a7e9117;
use crate::hle::track_parameters::at;
use crate::hle::track_service::{TrackConfig, TrackService, TrackStatus};
use crate::memory_map::{SOUND_ROM_BASE, SOUND_ROM_SIZE};

const ROM_BASE: u32 = SOUND_ROM_BASE;
const STREAM_BASE: u32 = sound_rom_5a7e9117::SEQ_STREAM_BASE;

// Song track stream layout
const CONDUCTOR_STREAM_INDEX: usize = 0;
const FIRST_MUSIC_STREAM_INDEX: usize = 1;
const ARRANGEMENT_STREAM_INDEX: usize = 10;
const TOTAL_SONG_STREAMS: usize = 11;

// Sequence track event opcodes
const EV_NOTE_SHORT_MAX: u8 = 0x58;
const EV_NOTE_LONG_MAX: u8 = 0xb0;
const EV_NOTE_KEY_OFFSET: u8 = 0x58;
const EV_CONTROLLER_MIN: u8 = 0xb0;
const EV_CONTROLLER_MAX: u8 = 0xd9;
const EV_PROGRAM_SELECTOR: u8 = 0xd9;
const EV_SPECIAL_TRACK_DA: u8 = 0xda;
const EV_SPECIAL_TRACK_DB: u8 = 0xdb;
const EV_RELATIVE_NOTE: u8 = 0xdc;
const EV_DELAY: u8 = 0xe6;
const EV_ARRANGEMENT: u8 = 0xe7;
const EV_NOTIFICATION: u8 = 0xe8;
const EV_END: u8 = 0xe9;

// Arrangement marker
const ARRANGEMENT_LOOP_MARKER: u16 = 100;

// Header flags (+1) bit 1: a standalone song restarts at its last bar.
const SEQ_FLAG_LOOP: u8 = 0x02;

// The sequence-addressed commands share the alias walk and the active-sequence gate; these express
// that over the CommandPacket alternatives.

fn sequence_ref(command: &Command) -> SequenceRef {
    match command {
        Command::SequenceStart(c) => c.sequence,
        Command::SequenceStartRepeat(c) => c.sequence,
        Command::SequenceStop(c) => c.sequence,
        Command::SequencePause(c) => c.sequence,
        Command::SequenceResume(c) => c.sequence,
        Command::SequenceSeek(c) => c.sequence,
        Command::SequenceVolume(c) => c.sequence,
        Command::SequencePan(c) => c.sequence,
        Command::SequenceRate(c) => c.sequence,
        Command::SequenceFade(c) => c.sequence,
        Command::TrackVolume(c) => c.sequence,
        Command::TrackPan(c) => c.sequence,
        Command::TrackController(c) => c.sequence,
        Command::TrackProgram(c) => c.sequence,
        Command::DirectNoteOn(c) => c.sequence,
        Command::DirectNoteOff(c) => c.sequence,
        Command::Nop(c) => c.sequence,
        // The dispatcher only asks the sequence-addressed alternatives.
        _ => SequenceRef::default(),
    }
}

fn track_of(command: &Command) -> u8 {
    match command {
        Command::TrackVolume(c) => c.track,
        Command::TrackPan(c) => c.track,
        Command::TrackController(c) => c.track,
        Command::TrackProgram(c) => c.track,
        Command::DirectNoteOn(c) => c.track,
        Command::DirectNoteOff(c) => c.track,
        Command::Nop(c) => c.track,
        // Only the track-addressed alternatives reach the track command.
        _ => 0,
    }
}

fn is_start(command: &Command) -> bool {
    matches!(command, Command::SequenceStart(_) | Command::SequenceStartRepeat(_))
}

fn cleared_selection(command: Command) -> Command {
    match command {
        Command::SequenceStart(mut c) => {
            c.sequence.selection = false;
            Command::SequenceStart(c)
        }
        Command::SequenceStartRepeat(mut c) => {
            c.sequence.selection = false;
            Command::SequenceStartRepeat(c)
        }
        Command::SequenceStop(mut c) => {
            c.sequence.selection = false;
            Command::SequenceStop(c)
        }
        Command::SequencePause(mut c) => {
            c.sequence.selection = false;
            Command::SequencePause(c)
        }
        Command::SequenceResume(mut c) => {
            c.sequence.selection = false;
            Command::SequenceResume(c)
        }
        Command::SequenceSeek(mut c) => {
            c.sequence.selection = false;
            Command::SequenceSeek(c)
        }
        Command::SequenceVolume(mut c) => {
            c.sequence.selection = false;
            Command::SequenceVolume(c)
        }
        Command::SequencePan(mut c) => {
            c.sequence.selection = false;
            Command::SequencePan(c)
        }
        Command::SequenceRate(mut c) => {
            c.sequence.selection = false;
            Command::SequenceRate(c)
        }
        Command::SequenceFade(mut c) => {
            c.sequence.selection = false;
            Command::SequenceFade(c)
        }
        Command::TrackVolume(mut c) => {
            c.sequence.selection = false;
            Command::TrackVolume(c)
        }
        Command::TrackPan(mut c) => {
            c.sequence.selection = false;
            Command::TrackPan(c)
        }
        Command::TrackController(mut c) => {
            c.sequence.selection = false;
            Command::TrackController(c)
        }
        Command::TrackProgram(mut c) => {
            c.sequence.selection = false;
            Command::TrackProgram(c)
        }
        Command::DirectNoteOn(mut c) => {
            c.sequence.selection = false;
            Command::DirectNoteOn(c)
        }
        Command::DirectNoteOff(mut c) => {
            c.sequence.selection = false;
            Command::DirectNoteOff(c)
        }
        Command::Nop(mut c) => {
            c.sequence.selection = false;
            Command::Nop(c)
        }
        other => other,
    }
}

// =============================================================================
// Pure Bit-Decoding Helpers
// =============================================================================

const fn decode_short_note_velocity(operand: u16) -> u8 {
    let velocity = ((operand >> 8) & 0x7c) as u8;
    if velocity != 0 { velocity | (velocity >> 5) } else { 1 }
}

const fn decode_long_note_velocity(operand: u16) -> u8 {
    let velocity = (operand & 0x7f) as u8;
    if velocity != 0 { velocity } else { 1 }
}

const fn decode_arrangement_transpose(word3: u16) -> i16 {
    (word3.wrapping_mul(2) as i16) >> 11
}

// =============================================================================
// Static ROM Data Structures & Parser
// =============================================================================

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
struct StreamEvent {
    /// The stream address the C++ records; nothing reads it.
    address: u32,
    delay: u32,
    operand: u16,
    duration: u16,
    opcode: u8,
    key: u8,
    velocity: u8,
    arrangement: [u16; 4],
}

#[derive(Default)]
struct TrackData {
    events: Vec<StreamEvent>,
}

struct SongData {
    present: bool,
    flags: u8,
    rate: u8,
    numerator: u8,
    exponent: u8,
    bars: u16,
    ticks_per_bar: u16,
    loop_start: u32,
    loop_end: u32,
    initial: [TrackConfig; TRACK_COUNT],
    tracks: [TrackData; TOTAL_SONG_STREAMS],
    linked: u8,
}

impl Default for SongData {
    fn default() -> SongData {
        SongData {
            present: false,
            flags: 0,
            rate: 0,
            numerator: 0,
            exponent: 0,
            bars: 0,
            ticks_per_bar: 0,
            loop_start: 0,
            loop_end: 0,
            initial: [TrackConfig::default(); TRACK_COUNT],
            tracks: core::array::from_fn(|_| TrackData::default()),
            linked: 0,
        }
    }
}

fn parse_track_stream(rom: &RomView, mut p: u32, end: u32) -> TrackData {
    fn read_operand(rom: &RomView, p: &mut u32, end: u32) -> u16 {
        if p.wrapping_add(2) > end {
            panic!("truncated HLE event operand");
        }
        let word = rom.read_u16(*p);
        *p += 2;
        word
    }

    let mut out = TrackData::default();
    let mut finished = false;

    while p.wrapping_add(2) <= end {
        let mut e = StreamEvent { address: p, ..StreamEvent::default() };
        let token = rom.read_u16(p);
        p += 2;

        if token & 0x8000 == 0 {
            continue;
        }

        e.opcode = token as u8;
        e.delay = u32::from((token >> 8) & 0x7f);

        if e.opcode < EV_NOTE_SHORT_MAX {
            e.key = e.opcode;
            e.operand = read_operand(rom, &mut p, end);
            e.duration = e.operand & 0x3ff;
            if e.duration == 0 {
                e.duration = read_operand(rom, &mut p, end);
            }
            e.velocity = decode_short_note_velocity(e.operand);
        } else if e.opcode < EV_NOTE_LONG_MAX {
            e.key = e.opcode - EV_NOTE_KEY_OFFSET;
            e.operand = read_operand(rom, &mut p, end);
            e.duration = e.operand & 0x3ff;
            if e.duration == 0 {
                e.duration = 1;
            }
            e.velocity = decode_long_note_velocity(e.operand);
        } else if e.opcode < EV_RELATIVE_NOTE {
            e.operand = read_operand(rom, &mut p, end);
        } else if e.opcode == EV_RELATIVE_NOTE {
            panic!("relative-note references are absent from supported ROM bank");
        } else if e.opcode == EV_DELAY {
            e.operand = read_operand(rom, &mut p, end);
            e.delay = u32::from(e.operand | ((u32::from(token) << 1) as u16 & 0x8000));
        } else if e.opcode == EV_ARRANGEMENT {
            for word in e.arrangement.iter_mut() {
                *word = read_operand(rom, &mut p, end);
            }
            e.delay = 0;
        } else if e.opcode == EV_NOTIFICATION || e.opcode == EV_END {
            e.delay = 0;
        } else {
            panic!("unsupported HLE sequence opcode");
        }

        out.events.push(e);
        if e.opcode == EV_END {
            finished = true;
            break;
        }
    }

    if !finished {
        panic!("HLE track missing end event");
    }
    out
}

fn parse_initial_track(rom: &RomView, header_addr: u32, track_index: usize) -> TrackConfig {
    let mut config = TrackConfig::default();

    // c13d1e/c13ca4 restore even-offset controller defaults from c13556.
    for index in (0..=0x14u32).step_by(2) {
        config.parameters.set_byte(
            at::controllers + index as usize,
            rom.read_byte(sound_rom_5a7e9117::CONTROLLER_DEFAULTS_TABLE + index),
        );
    }

    let record = header_addr
        .wrapping_add(sound_rom_5a7e9117::SEQ_HEADER_TRACKS_OFFSET)
        .wrapping_add((track_index as u32).wrapping_mul(sound_rom_5a7e9117::SEQ_TRACK_RECORD_SIZE as u32));

    for n in 0..sound_rom_5a7e9117::SEQ_TRACK_RECORD_SIZE as u32 {
        config
            .parameters
            .set_byte(at::program_record + n as usize, rom.read_byte(record.wrapping_add(n)));
    }

    config.program = rom.read_u16(record);
    config.parameters.set_program_image(config.program);
    config.parameters.copy_record_to_live();

    config
}

fn parse_sound_bank(rom: &RomView) -> [SongData; SONG_SLOTS] {
    let mut bank_data: [SongData; SONG_SLOTS] = core::array::from_fn(|_| SongData::default());

    let stream_limit = ROM_BASE.wrapping_add(SOUND_ROM_IMAGE_SIZE);
    let header_rel = rom.read_u32(STREAM_BASE);
    if header_rel < 8 + SONG_SLOTS as u32 * 4
        || header_rel & 1 != 0
        || STREAM_BASE.wrapping_add(header_rel) >= stream_limit
    {
        panic!("unsupported HLE sequence bank layout");
    }

    let header = STREAM_BASE.wrapping_add(header_rel);
    let header_size = rom.read_u32(header);
    if header_size < 8 + SONG_SLOTS as u32 * 2 + sound_rom_5a7e9117::SEQ_HEADER_SIZE
        || header.wrapping_add(header_size) > stream_limit
    {
        panic!("unsupported HLE sequence bank layout");
    }

    // The index is the sequence id: it addresses the bank table and the ROM's id-indexed tables.
    #[allow(clippy::needless_range_loop)]
    for id in 0..SONG_SLOTS {
        let relative = rom.read_u32(STREAM_BASE.wrapping_add(8).wrapping_add(id as u32 * 4));
        if relative == 0 {
            continue;
        }

        let start = STREAM_BASE.wrapping_add(relative);
        let end = start.wrapping_add(rom.read_u32(start));
        if relative < 408 || relative & 1 != 0 || end > header || end < start.wrapping_add(48) {
            panic!("invalid HLE sequence extent");
        }

        let header_offset = rom.read_u16(header.wrapping_add(8).wrapping_add(id as u32 * 2));
        if u32::from(header_offset) < 208
            || u32::from(header_offset).wrapping_add(sound_rom_5a7e9117::SEQ_HEADER_SIZE)
                > rom.read_u32(header)
        {
            panic!("invalid HLE sequence header");
        }

        let h = header.wrapping_add(u32::from(header_offset));
        let song = &mut bank_data[id];
        song.present = true;
        song.flags = rom.read_byte(h.wrapping_add(1));
        song.rate = rom.read_byte(h.wrapping_add(sound_rom_5a7e9117::SEQ_HEADER_RATE_OFFSET));
        song.bars = rom.read_u16(h.wrapping_add(sound_rom_5a7e9117::SEQ_HEADER_BARS_OFFSET));

        let exponent = rom.read_byte(h.wrapping_add(sound_rom_5a7e9117::SEQ_HEADER_TIME_SIG_EXP_OFFSET));
        if exponent > 8 {
            panic!("unsupported HLE time signature");
        }
        song.exponent = exponent;
        song.numerator =
            rom.read_byte(h.wrapping_add(sound_rom_5a7e9117::SEQ_HEADER_TIME_SIG_FACTOR_OFFSET));
        song.ticks_per_bar =
            ((384u32 >> exponent).wrapping_mul(u32::from(song.numerator))) as u16;
        song.loop_start =
            rom.read_u32(h.wrapping_add(sound_rom_5a7e9117::SEQ_HEADER_LOOP_START_OFFSET));
        song.loop_end =
            rom.read_u32(h.wrapping_add(sound_rom_5a7e9117::SEQ_HEADER_LOOP_END_OFFSET));
        song.linked = rom.read_byte(h.wrapping_add(sound_rom_5a7e9117::SEQ_HEADER_LINKED_OFFSET));

        for t in 0..TRACK_COUNT {
            song.initial[t] = parse_initial_track(rom, h, t);
        }

        let mut offsets = [0u32; TOTAL_SONG_STREAMS];
        for t in 0..TOTAL_SONG_STREAMS {
            offsets[t] =
                rom.read_u32(start.wrapping_add(4).wrapping_add((t as u32).wrapping_mul(4)));
        }

        // The index is the stream id: it addresses the offset table and the song's stream slot.
        #[allow(clippy::needless_range_loop)]
        for t in 0..TOTAL_SONG_STREAMS {
            if offsets[t] == 0 {
                continue;
            }
            if offsets[t] < 48
                || offsets[t] & 1 != 0
                || start.wrapping_add(offsets[t]).wrapping_add(4) > end
            {
                panic!("invalid HLE track offset");
            }
            let mut bound = end;
            for offset in offsets {
                if offset > offsets[t] {
                    bound = if bound < start.wrapping_add(offset) {
                        bound
                    } else {
                        start.wrapping_add(offset)
                    };
                }
            }
            song.tracks[t] = parse_track_stream(
                rom,
                start.wrapping_add(offsets[t]).wrapping_add(4),
                bound,
            );
        }
    }

    bank_data
}

// =============================================================================
// Sequence State Machine
// =============================================================================

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
#[repr(u8)]
enum SequenceStatus {
    #[default]
    Stopped, // completely inactive; no physical tracks held
    Playing, // active and advancing the tempo pulse (when rate > 0)
    Paused,  // halted via SequencePause; retains tracks and resumes via SequenceResume
    Parked,  // halted at natural end or after SequenceSeek; retains tracks for direct commands
}

// Sequencing position and tempo. The track records, the note bookkeeping and the volume mix live in TrackService.
#[derive(Clone, Copy, PartialEq, Eq, Debug)]
struct SongState {
    status: SequenceStatus,
    rate_override: bool,
    rate: u16,
    saved_rate: u16,
    position: u32,

    // Conductor track (Track 0) state
    conductor: usize,
    conductor_countdown: u32,
    conductor_ended: bool,

    // Arrangement & transposition state
    selected: u8,
    phase: u8,
    arrangement_owner: u8,
    arrangement_repeat: u16,
    transpose: i16,
    transpose_mask: u16,
}

impl Default for SongState {
    fn default() -> SongState {
        SongState {
            status: SequenceStatus::Stopped,
            rate_override: false,
            rate: 0,
            saved_rate: 0,
            position: 0,
            conductor: 0,
            conductor_countdown: 1,
            conductor_ended: false,
            selected: 0,
            phase: 0,
            arrangement_owner: 0,
            arrangement_repeat: 1,
            transpose: 0,
            transpose_mask: 0,
        }
    }
}

impl SongState {
    fn is_active(&self) -> bool {
        self.status != SequenceStatus::Stopped
    }

    // The C++ defines it as part of the state machine; nothing calls it.
    #[allow(dead_code)]
    fn is_running(&self) -> bool {
        self.is_active() && self.rate > 0
    }

    fn pause_playback(&mut self) {
        self.status = SequenceStatus::Paused;
        self.rate = 0;
        self.rate_override = true;
    }

    fn resume_playback(&mut self) {
        self.status = SequenceStatus::Playing;
        self.rate = self.saved_rate;
    }

    fn park_playback(&mut self) {
        self.status = SequenceStatus::Parked;
        self.rate = 0;
    }

    fn override_rate(&mut self, new_rate: u8) {
        self.status = SequenceStatus::Playing;
        self.rate = u16::from(new_rate);
        self.saved_rate = u16::from(new_rate);
        self.rate_override = true;
    }
}

// =============================================================================
// Sequencer Orchestrator Implementation
// =============================================================================

struct Impl {
    rom: &'static [u8],
    data: [SongData; SONG_SLOTS],
    state: [SongState; SONG_SLOTS],
    beats: [PulseTimer; SONG_SLOTS],
    serial: u64,
    current_bank: u8,
}

/// One sequencer call: the sequencer's own state split into disjoint borrows, plus the services it
/// writes to, borrowed for that call only.
struct Pass<'a> {
    rom: &'static [u8],
    current_bank: u8,
    data: &'a [SongData; SONG_SLOTS],
    state: &'a mut [SongState; SONG_SLOTS],
    beats: &'a mut [PulseTimer; SONG_SLOTS],
    serial: &'a mut u64,
    mailbox: &'a mut Mailbox,
    tracks: &'a mut TrackService,
}

impl Impl {
    // -------------------------------------------------------------------------
    // 1. Lifecycle & Public Entry Points
    // -------------------------------------------------------------------------

    fn new(image: &'static [u8]) -> Impl {
        if image.len() < SOUND_ROM_SIZE {
            panic!("HLE requires at least a 512 KiB padded sound ROM");
        }
        let mut impl_ = Impl {
            rom: image,
            data: core::array::from_fn(|_| SongData::default()),
            state: [SongState::default(); SONG_SLOTS],
            beats: core::array::from_fn(|_| PulseTimer::new(625, 0)),
            serial: 0,
            current_bank: 1,
        };
        impl_.load_bank(1);
        impl_
    }

    fn load_bank(&mut self, bank: u8) {
        self.current_bank = bank;
        let rom = RomView::with_image(self.rom, self.current_bank);
        self.data = parse_sound_bank(&rom);
    }

    fn pass<'a>(&'a mut self, mailbox: &'a mut Mailbox, tracks: &'a mut TrackService) -> Pass<'a> {
        Pass {
            rom: self.rom,
            current_bank: self.current_bank,
            data: &self.data,
            state: &mut self.state,
            beats: &mut self.beats,
            serial: &mut self.serial,
            mailbox,
            tracks,
        }
    }
}

impl Pass<'_> {
    fn dispatch_command(&mut self, packet: &CommandPacket, instance: u64, tick: u64) {
        // The packet's opcode selects the payload; `Ignored` is the rejected default branch.
        let command = *packet.command();
        match command {
            Command::EffectPreset(c) => self.dispatch_effect_preset(&c, tick),
            Command::EffectParameter(c) => self.dispatch_effect_parameter(&c, tick),
            Command::SequenceStart(_)
            | Command::SequenceStartRepeat(_)
            | Command::SequenceStop(_)
            | Command::SequencePause(_)
            | Command::SequenceResume(_)
            | Command::SequenceSeek(_)
            | Command::SequenceVolume(_)
            | Command::SequencePan(_)
            | Command::SequenceRate(_)
            | Command::SequenceFade(_)
            | Command::TrackVolume(_)
            | Command::TrackPan(_)
            | Command::TrackController(_)
            | Command::TrackProgram(_)
            | Command::DirectNoteOn(_)
            | Command::DirectNoteOff(_)
            | Command::Nop(_) => self.dispatch_addressed_command(command, instance, tick),
            // Ignored: an unknown opcode or a payload length the driver rejects.
            Command::Ignored(_) => {}
        }
    }

    fn dispatch_effect_preset(&mut self, command: &EffectPreset, tick: u64) {
        if command.preset < 13 {
            self.mailbox.push(Event::Effect(EffectEvent {
                kind: EffectEventKind::Preset,
                tick,
                preset: command.preset,
                index: 0,
                value: 0,
                }));
        }
    }

    fn dispatch_effect_parameter(&mut self, command: &EffectParameter, tick: u64) {
        self.mailbox.push(Event::Effect(EffectEvent {
            kind: EffectEventKind::Parameter,
            tick,
            preset: 0,
            index: command.index,
            value: command.value,
            }));
    }

    fn dispatch_addressed_command(&mut self, command: Command, instance: u64, tick: u64) {
        let mut id = sequence_ref(&command).id;
        if usize::from(id) >= SONG_SLOTS {
            return;
        }

        // c12de6 applies a command to this sequence and follows linked aliases.
        let mut visited = [false; SONG_SLOTS];
        loop {
            if visited[usize::from(id)] {
                panic!("cyclic HLE linked sequence alias");
            }
            visited[usize::from(id)] = true;
            self.execute_sequence_command(command, id, instance, tick);
            if !self.data[usize::from(id)].present || self.data[usize::from(id)].linked == 0 {
                break;
            }
            id = self.data[usize::from(id)].linked;
            if usize::from(id) >= SONG_SLOTS {
                panic!("invalid HLE linked sequence alias");
            }
        }
    }

    // -------------------------------------------------------------------------
    // 2. Mailbox Command Execution
    // -------------------------------------------------------------------------

    fn execute_sequence_command(&mut self, command: Command, id: u8, instance: u64, tick: u64) {
        if sequence_ref(&command).selection && self.is_arranged(id) {
            if is_start(&command) {
                self.stop_selected_child(id, tick);
                self.advance_arrangement(id, tick, core::ptr::null());
                return;
            } else if matches!(command, Command::SequenceStop(_)) {
                self.stop_selected_child(id, tick);
                return;
            } else {
                let child = self.selected_child(id);
                if child != 0 {
                    self.execute_sequence_command(
                        cleared_selection(command),
                        child,
                        instance,
                        tick,
                    );
                }
                return;
            }
        }

        if is_start(&command) {
            self.start_sequence(id, tick);
            return;
        }
        if matches!(command, Command::SequenceStop(_)) {
            self.stop_sequence(id, tick);
            return;
        }
        if !matches!(command, Command::SequenceSeek(_))
            && !self.state[usize::from(id)].is_active()
        {
            return;
        }

        match command {
            Command::SequencePause(_) => {
                self.state[usize::from(id)].pause_playback();
                self.beats[usize::from(id)].set_step(0);
                self.tracks.release_notes(id, tick, self.mailbox);
            }
            Command::SequenceResume(_) => {
                self.state[usize::from(id)].resume_playback();
                self.beats[usize::from(id)]
                    .set_step(u32::from(self.state[usize::from(id)].rate));
            }
            Command::SequenceSeek(c) => {
                if !self.state[usize::from(id)].is_active() {
                    self.start_sequence(id, tick);
                }
                self.initialize_tracks(id, id, tick);
                let mut n = 0u64;
                while n < u64::from(c.position) && self.state[usize::from(id)].is_active() {
                    self.tick_sequence(id, tick, true);
                    n += 1;
                }
                self.finish_sequence(id, tick);
            }
            Command::SequenceVolume(c) => {
                self.mailbox.push(Event::Track(TrackEvent {
                    kind: TrackEventKind::SequenceVolume,
                    tick,
                    sequence: id,
                    track: 0,
                    index: 0,
                    value: c.volume,
                    program: 0,
                    start: 0,
                    target: 0,
                    rate: 0,
                    }));
            }
            Command::SequencePan(c) => {
                self.mailbox.push(Event::Track(TrackEvent {
                    kind: TrackEventKind::SequencePan,
                    tick,
                    sequence: id,
                    track: 0,
                    index: 0,
                    value: c.value,
                    program: 0,
                    start: 0,
                    target: 0,
                    rate: 0,
                    }));
            }
            Command::SequenceRate(c) => {
                self.state[usize::from(id)].override_rate(c.rate);
                self.beats[usize::from(id)]
                    .set_step(u32::from(self.state[usize::from(id)].rate));
            }
            Command::SequenceFade(c) => {
                let rom_view = RomView::with_image(self.rom, self.current_bank);
                let multiplier = rom_view.read_u16(
                    sound_rom_5a7e9117::RATE_TABLE + u32::from(c.rate_index) * 2,
                ) as i16;
                self.mailbox.push(Event::Track(TrackEvent {
                    kind: TrackEventKind::SequenceFade,
                    tick,
                    sequence: id,
                    track: 0,
                    index: 0,
                    value: 0,
                    program: 0,
                    start: c.start,
                    target: c.target,
                    rate: multiplier,
                    }));
            }
            // The track-addressed commands; starts and stops returned above.
            Command::TrackVolume(_)
            | Command::TrackPan(_)
            | Command::TrackController(_)
            | Command::TrackProgram(_)
            | Command::DirectNoteOn(_)
            | Command::DirectNoteOff(_)
            | Command::Nop(_) => self.execute_track_command(command, id, instance, tick),
            _ => {}
        }
    }

    fn execute_track_command(&mut self, command: Command, id: u8, instance: u64, tick: u64) {
        let logical = track_of(&command);
        if logical == 0 || usize::from(logical) > TRACK_COUNT {
            return;
        }
        let t = logical - 1;
        if !self.tracks.tracks(id)[usize::from(t)].is_live() {
            return;
        }

        match command {
            Command::TrackVolume(c) => {
                self.mailbox.push(Event::Track(TrackEvent {
                    kind: TrackEventKind::Volume,
                    tick,
                    sequence: id,
                    track: t,
                    index: 0,
                    value: c.value,
                    program: 0,
                    start: 0,
                    target: 0,
                    rate: 0,
                    }));
            }
            Command::TrackPan(c) => {
                self.mailbox.push(Event::Track(TrackEvent {
                    kind: TrackEventKind::Pan,
                    tick,
                    sequence: id,
                    track: t,
                    index: 0,
                    value: c.value,
                    program: 0,
                    start: 0,
                    target: 0,
                    rate: 0,
                    }));
            }
            Command::TrackController(c) => {
                self.mailbox.push(Event::Track(TrackEvent {
                    kind: TrackEventKind::Controller,
                    tick,
                    sequence: id,
                    track: t,
                    index: c.index,
                    value: c.value,
                    program: 0,
                    start: 0,
                    target: 0,
                    rate: 0,
                    }));
            }
            Command::TrackProgram(c) => {
                self.mailbox.push(Event::Track(TrackEvent {
                    kind: TrackEventKind::Program,
                    tick,
                    sequence: id,
                    track: t,
                    index: 0,
                    value: 0,
                    program: c.program,
                    start: 0,
                    target: 0,
                    rate: 0,
                    }));
            }
            Command::DirectNoteOn(c) => {
                if instance & (1u64 << 63) != 0 {
                    panic!("direct-note instance uses reserved music ID bit");
                }
                self.trigger_note(
                    id,
                    usize::from(t),
                    c.key,
                    c.velocity,
                    0x7fff,
                    instance,
                    tick,
                    false,
                );
            }
            Command::DirectNoteOff(c) => {
                self.mailbox.push(Event::ReleaseKey(ReleaseKeyEvent {
                    tick,
                    sequence: id,
                    track: t,
                    kernel_key: c.key.wrapping_add(21),
                    }));
            }
            Command::Nop(_) => {
                // c1310e retains return-PC c13148 in A3; c13148 writes 6(A3)
                // = ROM c1314e, not note RAM. The board ignores this ROM write.
            }
            _ => {}
        }
    }

    // -------------------------------------------------------------------------
    // 3. Sequence Transport (Start / Stop / Finish / Init)
    // -------------------------------------------------------------------------

    fn start_sequence(&mut self, id: u8, tick: u64) {
        if !self.data[usize::from(id)].present {
            return;
        }
        let phase = self.state[usize::from(id)].phase;
        self.stop_sequence(id, tick);

        let rate = u16::from(self.data[usize::from(id)].rate);
        let s = &mut self.state[usize::from(id)];
        *s = SongState::default();
        s.status = SequenceStatus::Playing;
        s.rate = rate;
        s.saved_rate = rate;
        s.phase = phase;
        // An ordinary start never interprets the selection list in track 10.
        self.tracks.start_sequence(id, &self.data[usize::from(id)].initial, tick, self.mailbox);
        self.beats[usize::from(id)].reset();
        self.beats[usize::from(id)].set_step(u32::from(rate));
        self.initialize_tracks(id, id, tick);
    }

    fn initialize_tracks(&mut self, owner: u8, selected: u8, tick: u64) {
        if !self.data[usize::from(selected)].present {
            panic!("arrangement selects absent HLE sequence");
        }

        let s = &mut self.state[usize::from(owner)];
        s.selected = selected;
        s.position = 0;
        s.conductor = 0;
        s.conductor_countdown = 1;
        s.conductor_ended =
            self.data[usize::from(selected)].tracks[CONDUCTOR_STREAM_INDEX].events.is_empty();

        // Note-offs only: the physical tracks survive a repeat.
        self.tracks.release_notes(owner, tick, self.mailbox);
        for t in 0..TRACK_COUNT {
            let physical = self.tracks.tracks(owner)[t].physical;
            let previous_key = self.tracks.previous_key(physical);
            let track = &mut self.tracks.tracks(owner)[t];
            track.config = self.data[usize::from(selected)].initial[t];
            track.config.parameters.set_previous_key(previous_key);
            track.cursor = 0;
            track.countdown = 1;

            let can_stream = track.physical != 0
                && !self.data[usize::from(selected)].tracks[FIRST_MUSIC_STREAM_INDEX + t]
                    .events
                    .is_empty()
                && track.config.program != NO_PROGRAM;
            track.status = if can_stream { TrackStatus::Streaming } else { TrackStatus::Inactive };
            // c13ef6 re-sends the track record: voices of this track still sounding pick it up before the
            // pulse's own events are applied.
            if track.is_live() {
                self.mailbox.push(Event::Track(TrackEvent {
                    kind: TrackEventKind::Refresh,
                    tick,
                    sequence: owner,
                    track: t as u8,
                    index: 0,
                    value: 0,
                    program: 0,
                    start: 0,
                    target: 0,
                    rate: 0,
                }));
            }
        }

        if !s.rate_override {
            s.saved_rate = u16::from(self.data[usize::from(selected)].rate);
            s.rate = s.saved_rate;
            self.beats[usize::from(owner)].set_step(u32::from(s.rate));
        }

        self.process_due_events(owner, tick, false, true);
    }

    fn stop_sequence(&mut self, id: u8, tick: u64) {
        self.tracks.stop_sequence(id, tick, self.mailbox);
        self.beats[usize::from(id)].set_step(0);
        self.state[usize::from(id)].status = SequenceStatus::Stopped;
    }

    fn finish_sequence(&mut self, id: u8, tick: u64) {
        // Natural end parks the sequencer but retains allocated track records. c13072 still accepts direct 8d/8e
        // commands on those tracks. Explicit 82 instead removes the active sequence through c12c30.
        self.tracks.park_sequence(id, tick, self.mailbox);
        self.state[usize::from(id)].park_playback();
        self.beats[usize::from(id)].set_step(0);
    }

    // -------------------------------------------------------------------------
    // 4. Arrangement Playlist Control (Track 10)
    // -------------------------------------------------------------------------

    fn is_arranged(&self, id: u8) -> bool {
        let arr_events = &self.data[usize::from(id)].tracks[ARRANGEMENT_STREAM_INDEX].events;
        !arr_events.is_empty() && arr_events[0].opcode == EV_ARRANGEMENT
    }

    fn selected_child(&self, id: u8) -> u8 {
        let s = &self.state[usize::from(id)];
        let arr_events = &self.data[usize::from(id)].tracks[ARRANGEMENT_STREAM_INDEX].events;
        if s.phase == 0 || usize::from(s.phase) > arr_events.len() {
            return 0;
        }
        let e = &arr_events[usize::from(s.phase) - 1];
        if e.opcode == EV_ARRANGEMENT && e.arrangement[3] != ARRANGEMENT_LOOP_MARKER {
            e.arrangement[0] as u8
        } else {
            0
        }
    }

    fn stop_selected_child(&mut self, id: u8, tick: u64) {
        let child = self.selected_child(id);
        if child != 0 {
            self.stop_sequence(child, tick);
        }
        self.state[usize::from(id)].phase = 0;
    }

    fn advance_arrangement(&mut self, id: u8, tick: u64, previous: *const SongState) -> u8 {
        let list_len = self.data[usize::from(id)].tracks[ARRANGEMENT_STREAM_INDEX].events.len();

        // c12bea/c12d0e: 100 is a loop-selection marker, not a playable phase.
        // At the terminator, return to the phase following the last marker.
        let mut attempt = 0usize;
        while attempt <= list_len {
            attempt += 1;
            let s = &mut self.state[usize::from(id)];
            s.phase = s.phase.wrapping_add(1);
            let phase = self.state[usize::from(id)].phase;
            if usize::from(phase) > list_len
                || self.data[usize::from(id)].tracks[ARRANGEMENT_STREAM_INDEX].events
                    [usize::from(phase) - 1]
                    .opcode
                    == EV_END
            {
                self.state[usize::from(id)].phase = 0;
                for n in 0..list_len {
                    let e = self.data[usize::from(id)].tracks[ARRANGEMENT_STREAM_INDEX].events[n];
                    if e.opcode == EV_ARRANGEMENT && e.arrangement[3] == ARRANGEMENT_LOOP_MARKER {
                        self.state[usize::from(id)].phase = (n + 1) as u8;
                    }
                }
                if self.state[usize::from(id)].phase == 0 {
                    return 0;
                }
                continue;
            }

            let e =
                self.data[usize::from(id)].tracks[ARRANGEMENT_STREAM_INDEX].events
                    [usize::from(phase) - 1];
            if e.arrangement[3] == ARRANGEMENT_LOOP_MARKER {
                continue;
            }

            let child = e.arrangement[0] as u8;
            if child == 0
                || usize::from(e.arrangement[0]) >= SONG_SLOTS
                || !self.data[usize::from(child)].present
            {
                panic!("invalid HLE arrangement selection");
            }

            self.state[usize::from(id)].arrangement_repeat = e.arrangement[3] & 0x7f;
            if self.state[usize::from(child)].is_active() {
                return child;
            }

            self.start_sequence(child, tick);
            let next = &mut self.state[usize::from(child)];
            next.arrangement_owner = id;
            next.transpose_mask = e.arrangement[2];
            next.transpose = decode_arrangement_transpose(e.arrangement[3]);

            if !previous.is_null() {
                // SAFETY: `previous` is the finishing sequence's state, which outlives this call.
                self.tracks.copy_mix(unsafe { (*previous).selected }, child);
                // SAFETY: as above.
                if unsafe { (*previous).rate_override } {
                    // SAFETY: as above.
                    let saved = unsafe { (*previous).saved_rate };
                    let next = &mut self.state[usize::from(child)];
                    next.rate_override = true;
                    next.rate = saved;
                    next.saved_rate = saved;
                    self.beats[usize::from(child)].set_step(u32::from(saved));
                }
            }
            return child;
        }
        0
    }

    // -------------------------------------------------------------------------
    // 5. Musical Tick & Track Event Processing
    // -------------------------------------------------------------------------

    fn tick_sequence(&mut self, id: u8, tick: u64, silent: bool) {
        if !self.state[usize::from(id)].is_active() {
            return;
        }

        self.state[usize::from(id)].position =
            self.state[usize::from(id)].position.wrapping_add(1);
        let selected = self.state[usize::from(id)].selected;

        self.process_due_events(id, tick, silent, false);

        // c14cc0..c14d12 performs the header's bar-boundary transition even
        // when an individual track has one final tick beyond that boundary.
        let boundary = u32::from(self.data[usize::from(selected)].bars)
            .wrapping_mul(u32::from(self.data[usize::from(selected)].ticks_per_bar));
        let mut finished = if boundary != 0 {
            self.state[usize::from(id)].position >= boundary
        } else {
            self.state[usize::from(id)].conductor_ended
        };
        if boundary == 0 {
            for track in self.tracks.tracks(id).iter() {
                if track.is_streaming() {
                    finished = false;
                }
            }
        }
        if !finished {
            return;
        }

        if self.state[usize::from(id)].arrangement_owner != 0 {
            let mut repeat_done = false;
            let owner = usize::from(self.state[usize::from(id)].arrangement_owner);
            if self.state[owner].arrangement_repeat != 0 {
                self.state[owner].arrangement_repeat =
                    self.state[owner].arrangement_repeat.wrapping_sub(1);
                repeat_done = self.state[owner].arrangement_repeat == 0;
            }
            if repeat_done {
                let previous: *const SongState = &self.state[usize::from(id)];
                let child =
                    self.advance_arrangement(self.state[usize::from(id)].arrangement_owner, tick, previous);
                if child != id {
                    self.stop_sequence(id, tick);
                    return;
                }
            }
            self.initialize_tracks(id, id, tick);
        } else if self.data[usize::from(selected)].flags & SEQ_FLAG_LOOP != 0 {
            self.initialize_tracks(id, id, tick);
        } else {
            self.finish_sequence(id, tick);
        }
    }

    fn process_due_events(&mut self, id: u8, tick: u64, silent: bool, initial: bool) {
        let selected = self.state[usize::from(id)].selected;

        let mut conductor_due = initial;
        if !self.state[usize::from(id)].conductor_ended
            && !conductor_due
            && self.state[usize::from(id)].conductor_countdown != 0
        {
            self.state[usize::from(id)].conductor_countdown =
                self.state[usize::from(id)].conductor_countdown.wrapping_sub(1);
            conductor_due = self.state[usize::from(id)].conductor_countdown == 0;
        }
        if !self.state[usize::from(id)].conductor_ended && conductor_due {
            let stream_len =
                self.data[usize::from(selected)].tracks[CONDUCTOR_STREAM_INDEX].events.len();
            loop {
                let conductor = self.state[usize::from(id)].conductor;
                if conductor >= stream_len {
                    break;
                }
                let event =
                    self.data[usize::from(selected)].tracks[CONDUCTOR_STREAM_INDEX].events
                        [conductor];
                self.state[usize::from(id)].conductor = conductor + 1;
                if event.opcode == EV_END {
                    self.state[usize::from(id)].conductor_ended = true;
                    break;
                }
                if event.opcode != EV_DELAY {
                    panic!("unsupported HLE conductor control");
                }
                self.state[usize::from(id)].conductor_countdown = event.delay;
                if event.delay != 0 {
                    break;
                }
            }
        }

        // The stream is borrowed from the sequence data, not from `self`, so the note and control
        // calls below can take `&mut self`; the track record is re-indexed around them.
        let data = self.data;
        for t in 0..TRACK_COUNT {
            let track = &mut self.tracks.tracks(id)[t];
            if !track.is_streaming() {
                continue;
            }
            if !initial {
                if track.countdown == 0 {
                    continue;
                }
                track.countdown = track.countdown.wrapping_sub(1);
                if track.countdown != 0 {
                    continue;
                }
            }

            let events = &data[usize::from(selected)].tracks[FIRST_MUSIC_STREAM_INDEX + t].events;
            loop {
                let cursor = self.tracks.tracks(id)[t].cursor;
                if cursor >= events.len() {
                    break;
                }
                let event = events[cursor];
                self.tracks.tracks(id)[t].cursor = cursor + 1;
                if event.opcode == EV_END {
                    self.tracks.tracks(id)[t].status = TrackStatus::Ended;
                    break;
                }
                if event.opcode < EV_NOTE_LONG_MAX {
                    if !silent {
                        *self.serial = self.serial.wrapping_add(1);
                        self.trigger_note(
                            id,
                            t,
                            event.key,
                            event.velocity,
                            event.duration,
                            (1u64 << 63) | *self.serial,
                            tick,
                            true,
                        );
                    }
                } else {
                    self.apply_track_control(id, t, &event, tick, silent);
                }
                self.tracks.tracks(id)[t].countdown = event.delay;
                if event.delay != 0 {
                    break;
                }
            }
        }
    }

    fn apply_track_control(
        &mut self,
        id: u8,
        track_id: usize,
        event: &StreamEvent,
        tick: u64,
        silent: bool,
    ) {
        let op = event.opcode;
        let value = (event.operand & 0x7f) as u8;
        let t = track_id as u8;

        if (EV_CONTROLLER_MIN..EV_CONTROLLER_MAX).contains(&op) {
            if !silent {
                self.mailbox.push(Event::Track(TrackEvent {
                    kind: TrackEventKind::Controller,
                    tick,
                    sequence: id,
                    track: t,
                    index: op - EV_CONTROLLER_MIN,
                    value,
                    program: 0,
                    start: 0,
                    target: 0,
                    rate: 0,
                    }));
            }
        } else if op == EV_PROGRAM_SELECTOR {
            if !silent {
                self.mailbox.push(Event::Track(TrackEvent {
                    kind: TrackEventKind::ProgramSelector,
                    tick,
                    sequence: id,
                    track: t,
                    index: 0,
                    value,
                    program: 0,
                    start: 0,
                    target: 0,
                    rate: 0,
                    }));
            }
        } else if op == EV_SPECIAL_TRACK_DA || op == EV_SPECIAL_TRACK_DB {
            panic!("HLE special-track control absent from supplied data");
        } else if op == EV_NOTIFICATION {
            panic!("HLE notification absent from supplied data");
        } else if op != EV_DELAY && op != EV_END {
            panic!("unsupported HLE track control");
        }
    }

    // -------------------------------------------------------------------------
    // 6. Note Triggering
    // -------------------------------------------------------------------------

    // The C++ takes the same nine parameters.
    #[allow(clippy::too_many_arguments)]
    fn trigger_note(
        &mut self,
        id: u8,
        track_id: usize,
        key: u8,
        velocity: u8,
        duration: u16,
        instance: u64,
        tick: u64,
        transpose: bool,
    ) {
        let t = track_id as u8;
        let track = &self.tracks.tracks(id)[track_id];
        if !track.is_live() || track.config.program == NO_PROGRAM {
            return;
        }

        let mut effective = i32::from(key);
        if transpose && self.state[usize::from(id)].transpose_mask & (1u16 << track_id) != 0 {
            effective += i32::from(self.state[usize::from(id)].transpose);
            if effective < 0 || effective >= i32::from(EV_NOTE_SHORT_MAX) {
                return; // c141b6/c141bc.
            }
        }

        let kernel_key = (effective + 21) as u8;
        let note = NoteOnEvent {
            instance,
            tick,
            duration,
            program: track.config.program,
            sequence: id,
            track: t,
            key,
            kernel_key,
            velocity,
            previous_key: track.config.parameters.previous_key(),
        };
        self.mailbox.push(Event::NoteOn(note));

        // c172c2 consumes the previous key; c1723e stores the new factory key
        // only after all instrument layers have received their onset context.
        self.mailbox.push(Event::Track(TrackEvent {
            kind: TrackEventKind::PreviousKey,
            tick,
            sequence: id,
            track: t,
            index: 0,
            value: kernel_key,
            program: 0,
            start: 0,
            target: 0,
            rate: 0,
        }));
    }
}

// =============================================================================
// Public Sequencer Facade
// =============================================================================

/// Reads the ROM sequence streams and turns them into mailbox events.
pub struct Sequencer {
    impl_: Box<Impl>,
}

impl Sequencer {
    pub fn new(rom: &'static [u8]) -> Sequencer {
        Sequencer { impl_: Box::new(Impl::new(rom)) }
    }

    pub fn command(
        &mut self,
        packet: &CommandPacket,
        instance: u64,
        tick: u64,
        mailbox: &mut Mailbox,
        tracks: &mut TrackService,
    ) {
        self.impl_.pass(mailbox, tracks).dispatch_command(packet, instance, tick);
    }

    /// One tempo pulse of `sequence`, after its beat timer fired.
    pub fn pulse(
        &mut self,
        sequence: u8,
        tick: u64,
        mailbox: &mut Mailbox,
        tracks: &mut TrackService,
    ) {
        self.impl_.pass(mailbox, tracks).tick_sequence(sequence, tick, false);
    }

    pub fn reset(&mut self, tick: u64, mailbox: &mut Mailbox, tracks: &mut TrackService) {
        let mut pass = self.impl_.pass(mailbox, tracks);
        for id in 0..SONG_SLOTS {
            pass.stop_sequence(id as u8, tick);
        }
        self.impl_.state = [SongState::default(); SONG_SLOTS];
        for beat in self.impl_.beats.iter_mut() {
            beat.reset();
            beat.set_step(0);
        }
        if self.impl_.current_bank != 1 {
            self.impl_.load_bank(1);
        }
    }

    pub fn set_bank(&mut self, bank: u8, _tick: u64) {
        if self.impl_.current_bank == bank {
            return;
        }
        self.impl_.load_bank(bank);
    }

    /// The per-sequence tempo pulses. Step 0 while the sequence is not playing.
    pub fn beats(&mut self) -> &mut [PulseTimer; SONG_SLOTS] {
        &mut self.impl_.beats
    }

    pub fn validate(sound_rom: &'static [u8]) -> bool {
        if sound_rom.len() < SOUND_ROM_SIZE {
            return false;
        }
        // The C++ catches every exception the default bank's parser throws.
        let hook = std::panic::take_hook();
        std::panic::set_hook(Box::new(|_| {}));
        let result = std::panic::catch_unwind(|| {
            Sequencer::new(sound_rom);
        });
        std::panic::set_hook(hook);
        result.is_ok()
    }
}
