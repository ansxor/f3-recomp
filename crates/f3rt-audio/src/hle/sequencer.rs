use std::sync::Arc;

use crate::hle::command::{Command, CommandPacket};
use crate::hle::driver_layout::DriverLayout;
use crate::hle::events::{
    EffectEvent, Event, NoteOnEvent, ReleaseKeyEvent, TrackEvent, NO_PROGRAM, SONG_SLOTS,
    TRACK_COUNT,
};
use crate::hle::mailbox::Mailbox;
use crate::hle::pulse_timer::PulseTimer;
use crate::hle::rom_view::{BOOT_BANK, RomView};
use crate::hle::sequence_bank::{
    SequenceBank, StreamEvent, EV_CONTROLLER_MAX, EV_CONTROLLER_MIN, EV_DELAY, EV_END,
    EV_NOTE_LONG_MAX, EV_NOTE_SHORT_MAX, EV_NOTIFICATION, EV_PROGRAM_SELECTOR, EV_SPECIAL_TRACK_DA,
    EV_SPECIAL_TRACK_DB, MAX_ARRANGEMENT_PHASES, SEQ_FLAG_LOOP,
};
use crate::hle::track_parameters::TrackParameters;
use crate::hle::track_service::{Track, TrackService, TrackStatus};
use crate::memory_map::SOUND_ROM_SIZE;

/// Effect presets at or above this value are not driver presets.
const EFFECT_PRESET_COUNT: u8 = 13;

const DIRECT_NOTE_PULSE_LIMIT: u16 = 0x7fff;

const MUSIC_INSTANCE_BIT: u64 = 1 << 63;

#[derive(Clone, Copy, Default, PartialEq, Eq, Debug)]
#[repr(u8)]
enum SequenceStatus {
    /// Completely inactive; no physical tracks held.
    #[default]
    Stopped,
    /// Active and advancing the tempo pulse (when rate > 0).
    Playing,
    /// Halted via SequencePause; retains tracks and resumes via SequenceResume.
    Paused,
    /// Halted at natural end or after SequenceSeek; retains tracks for direct commands.
    Parked,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
struct ArrangementCarry {
    selected: u8,
    rate_override: Option<u16>,
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
struct SongState {
    status: SequenceStatus,
    rate_override: bool,
    rate: u16,
    saved_rate: u16,
    position: u32,

    conductor: usize,
    conductor_countdown: u32,
    conductor_ended: bool,

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
    fn started(rate: u16, phase: u8) -> SongState {
        SongState {
            status: SequenceStatus::Playing,
            rate,
            saved_rate: rate,
            phase,
            ..SongState::default()
        }
    }

    fn is_active(&self) -> bool {
        self.status != SequenceStatus::Stopped
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

    fn carry(&self) -> ArrangementCarry {
        ArrangementCarry {
            selected: self.selected,
            rate_override: self.rate_override.then_some(self.saved_rate),
        }
    }

    fn count_arrangement_repeat(&mut self) -> bool {
        if self.arrangement_repeat == 0 {
            return false;
        }
        self.arrangement_repeat = self.arrangement_repeat.wrapping_sub(1);
        self.arrangement_repeat == 0
    }

    fn kernel_key(&self, track: u8, key: u8, transposed: bool) -> Option<u8> {
        let mut effective = i32::from(key);
        if transposed && self.transpose_mask & (1u16 << track) != 0 {
            effective += i32::from(self.transpose);
            if !(0..i32::from(EV_NOTE_SHORT_MAX)).contains(&effective) {
                return None;
            }
        }
        Some((effective + 21) as u8)
    }

    fn advance_conductor(&mut self, stream: &[StreamEvent], initial: bool) {
        let mut due = initial;
        if !self.conductor_ended && !due && self.conductor_countdown != 0 {
            self.conductor_countdown = self.conductor_countdown.wrapping_sub(1);
            due = self.conductor_countdown == 0;
        }
        if self.conductor_ended || !due {
            return;
        }
        while let Some(event) = stream.get(self.conductor) {
            self.conductor += 1;
            if event.opcode == EV_END {
                self.conductor_ended = true;
                break;
            }
            if event.opcode != EV_DELAY {
                panic!("unsupported HLE conductor control");
            }
            self.conductor_countdown = event.delay;
            if event.delay != 0 {
                break;
            }
        }
    }
}

struct Note {
    key: u8,
    velocity: u8,
    duration: u16,
    instance: u64,
}

struct Context<'a> {
    tick: u64,
    mailbox: &'a mut Mailbox,
    tracks: &'a mut TrackService,
}

impl Context<'_> {
    fn push_track(&mut self, event: TrackEvent) {
        self.mailbox.push(Event::Track(event));
    }

    fn release_notes(&mut self, sequence: u8) {
        self.tracks.release_notes(sequence, self.tick, self.mailbox);
    }

    fn note_on(&mut self, state: &SongState, sequence: u8, track: u8, note: Note, transposed: bool) {
        let t = self.tracks.track(sequence, track);
        if !t.is_live() || t.config.program == NO_PROGRAM {
            return;
        }
        let Some(kernel_key) = state.kernel_key(track, note.key, transposed) else {
            return;
        };
        self.enqueue_note_transition(sequence, track, note, kernel_key);
    }

    fn enqueue_note_transition(&mut self, sequence: u8, track: u8, note: Note, kernel_key: u8) {
        let previous_key = self.tracks.track(sequence, track).config.parameters.previous_key();
        let event = NoteOnEvent {
            instance: note.instance,
            tick: self.tick,
            duration: note.duration,
            sequence,
            track,
            key: note.key,
            kernel_key,
            velocity: note.velocity,
            previous_key,
        };
        self.mailbox.push(Event::NoteOn(event));
        self.push_track(TrackEvent::previous_key(self.tick, sequence, track, kernel_key));
    }

    fn stream_control(&mut self, sequence: u8, track: u8, event: &StreamEvent, silent: bool) {
        let op = event.opcode;
        let value = (event.operand & 0x7f) as u8;

        if (EV_CONTROLLER_MIN..EV_CONTROLLER_MAX).contains(&op) {
            if !silent {
                let index = op - EV_CONTROLLER_MIN;
                self.push_track(TrackEvent::controller(self.tick, sequence, track, index, value));
            }
        } else if op == EV_PROGRAM_SELECTOR {
            if !silent {
                self.push_track(TrackEvent::program_selector(self.tick, sequence, track, value));
            }
        } else if op == EV_SPECIAL_TRACK_DA || op == EV_SPECIAL_TRACK_DB {
            panic!("HLE special-track control absent from supplied data");
        } else if op == EV_NOTIFICATION {
            panic!("HLE notification absent from supplied data");
        } else if op != EV_DELAY && op != EV_END {
            panic!("unsupported HLE track control");
        }
    }
}

pub struct Sequencer {
    rom: Arc<[u8]>,
    layout: &'static DriverLayout,
    bank: SequenceBank,
    state: [SongState; SONG_SLOTS],
    beats: [PulseTimer; SONG_SLOTS],
    serial: u64,
    current_bank: u8,
}

impl Sequencer {
    /// `Err` names why `rom` cannot be sequenced: too small, or a malformed boot bank.
    pub fn try_new(
        rom: Arc<[u8]>,
        layout: &'static DriverLayout,
    ) -> Result<Sequencer, &'static str> {
        if rom.len() < SOUND_ROM_SIZE {
            return Err("HLE requires at least a 512 KiB padded sound ROM");
        }
        let bank = Self::parse_bank(&rom, layout, BOOT_BANK)?;
        Ok(Sequencer {
            rom,
            layout,
            bank,
            state: [SongState::default(); SONG_SLOTS],
            beats: core::array::from_fn(|_| PulseTimer::new(625, 0)),
            serial: 0,
            current_bank: BOOT_BANK,
        })
    }

    fn parse_bank(
        rom: &Arc<[u8]>,
        layout: &DriverLayout,
        bank: u8,
    ) -> Result<SequenceBank, &'static str> {
        SequenceBank::try_parse(&RomView::with_image(Arc::clone(rom), bank), layout)
    }

    /// A bank switch after construction: the boot bank already parsed, so a bank the game selects
    /// that does not is a fault of the running game.
    fn load_bank(&mut self, bank: u8) {
        self.current_bank = bank;
        self.bank = Self::parse_bank(&self.rom, self.layout, bank)
            .unwrap_or_else(|reason| panic!("{reason}"));
    }

    pub fn command(
        &mut self,
        packet: &CommandPacket,
        instance: u64,
        tick: u64,
        mailbox: &mut Mailbox,
        tracks: &mut TrackService,
    ) {
        let mut cx = Context { tick, mailbox, tracks };
        match *packet.command() {
            Command::EffectPreset(c) => {
                if c.preset < EFFECT_PRESET_COUNT {
                    cx.mailbox.push(Event::Effect(EffectEvent::preset(tick, c.preset)));
                }
            }
            Command::EffectParameter(c) => {
                cx.mailbox.push(Event::Effect(EffectEvent::parameter(tick, c.index, c.value)));
            }
            Command::Ignored(_) => {}
            addressed => self.dispatch_addressed(addressed, instance, &mut cx),
        }
    }

    /// One tempo pulse of `sequence`, after its beat timer fired.
    pub fn pulse(
        &mut self,
        sequence: u8,
        tick: u64,
        mailbox: &mut Mailbox,
        tracks: &mut TrackService,
    ) {
        self.tick_sequence(sequence, false, &mut Context { tick, mailbox, tracks });
    }

    pub fn reset(&mut self, tick: u64, mailbox: &mut Mailbox, tracks: &mut TrackService) {
        let mut cx = Context { tick, mailbox, tracks };
        for id in 0..SONG_SLOTS as u8 {
            self.stop_sequence(id, &mut cx);
        }
        self.state = [SongState::default(); SONG_SLOTS];
        for beat in self.beats.iter_mut() {
            beat.reset();
            beat.set_step(0);
        }
        if self.current_bank != BOOT_BANK {
            self.load_bank(BOOT_BANK);
        }
    }

    pub fn set_bank(&mut self, bank: u8, _tick: u64) {
        if self.current_bank != bank {
            self.load_bank(bank);
        }
    }

    /// The per-sequence tempo pulses. Step 0 while the sequence is not playing.
    pub fn beats(&mut self) -> &mut [PulseTimer; SONG_SLOTS] {
        &mut self.beats
    }

    /// Applies a sequence-addressed command to its sequence and then to each linked alias.
    fn dispatch_addressed(&mut self, command: Command, instance: u64, cx: &mut Context) {
        let Some(sequence) = command.sequence_ref() else {
            return;
        };
        let mut id = sequence.id;
        if usize::from(id) >= SONG_SLOTS {
            return;
        }

        let mut visited = [false; SONG_SLOTS];
        loop {
            if visited[usize::from(id)] {
                panic!("cyclic HLE linked sequence alias");
            }
            visited[usize::from(id)] = true;
            self.execute_sequence_command(command, id, instance, cx);

            let song = self.bank.song(id);
            if !song.present || song.linked == 0 {
                break;
            }
            id = song.linked;
            if usize::from(id) >= SONG_SLOTS {
                panic!("invalid HLE linked sequence alias");
            }
        }
    }

    fn execute_sequence_command(&mut self, command: Command, id: u8, instance: u64, cx: &mut Context) {
        let selection = command.sequence_ref().is_some_and(|sequence| sequence.selection);
        if selection && self.bank.song(id).is_arranged() {
            if command.is_start() {
                self.stop_selected_child(id, cx);
                self.advance_arrangement(id, None, cx);
            } else if matches!(command, Command::SequenceStop(_)) {
                self.stop_selected_child(id, cx);
            } else {
                let child = self.selected_child(id);
                if child != 0 {
                    self.execute_sequence_command(command.without_selection(), child, instance, cx);
                }
            }
            return;
        }

        if command.is_start() {
            self.start_sequence(id, cx);
            return;
        }
        if matches!(command, Command::SequenceStop(_)) {
            self.stop_sequence(id, cx);
            return;
        }
        let is_seek = matches!(command, Command::SequenceSeek(_));
        if !is_seek && !self.state[usize::from(id)].is_active() {
            return;
        }

        match command {
            Command::SequencePause(_) => {
                self.state[usize::from(id)].pause_playback();
                self.beats[usize::from(id)].set_step(0);
                cx.release_notes(id);
            }
            Command::SequenceResume(_) => {
                self.state[usize::from(id)].resume_playback();
                self.set_beat_to_rate(id);
            }
            Command::SequenceSeek(c) => {
                if !self.state[usize::from(id)].is_active() {
                    self.start_sequence(id, cx);
                }
                if !self.state[usize::from(id)].is_active() {
                    return;
                }
                self.initialize_tracks(id, id, cx);
                let mut ticks = 0u64;
                while ticks < u64::from(c.position) && self.state[usize::from(id)].is_active() {
                    self.tick_sequence(id, true, cx);
                    ticks += 1;
                }
                self.finish_sequence(id, cx);
            }
            Command::SequenceVolume(c) => {
                cx.push_track(TrackEvent::sequence_volume(cx.tick, id, c.volume));
            }
            Command::SequencePan(c) => {
                cx.push_track(TrackEvent::sequence_pan(cx.tick, id, c.value));
            }
            Command::SequenceRate(c) => {
                self.state[usize::from(id)].override_rate(c.rate);
                self.set_beat_to_rate(id);
            }
            Command::SequenceFade(c) => {
                let rom = RomView::with_image(Arc::clone(&self.rom), self.current_bank);
                let multiplier =
                    rom.read_u16(self.layout.rate_table + u32::from(c.rate_index) * 2) as i16;
                cx.push_track(TrackEvent::sequence_fade(cx.tick, id, c.start, c.target, multiplier));
            }
            _ => self.execute_track_command(command, id, instance, cx),
        }
    }

    fn execute_track_command(&mut self, command: Command, id: u8, instance: u64, cx: &mut Context) {
        let Some(logical) = command.track() else {
            return;
        };
        if logical == 0 || usize::from(logical) > TRACK_COUNT {
            return;
        }
        let track = logical - 1;
        if !cx.tracks.track(id, track).is_live() {
            return;
        }

        match command {
            Command::TrackVolume(c) => {
                cx.push_track(TrackEvent::volume(cx.tick, id, track, c.value));
            }
            Command::TrackPan(c) => cx.push_track(TrackEvent::pan(cx.tick, id, track, c.value)),
            Command::TrackController(c) => {
                if TrackParameters::valid_controller(usize::from(c.index)) {
                    cx.push_track(TrackEvent::controller(cx.tick, id, track, c.index, c.value));
                }
            }
            Command::TrackProgram(c) => {
                cx.push_track(TrackEvent::program(cx.tick, id, track, c.program));
            }
            Command::DirectNoteOn(c) => {
                if instance & MUSIC_INSTANCE_BIT != 0 {
                    panic!("direct-note instance uses reserved music ID bit");
                }
                let note = Note {
                    key: c.key,
                    velocity: c.velocity,
                    duration: DIRECT_NOTE_PULSE_LIMIT,
                    instance,
                };
                cx.note_on(&self.state[usize::from(id)], id, track, note, false);
            }
            Command::DirectNoteOff(c) => {
                cx.mailbox.push(Event::ReleaseKey(ReleaseKeyEvent {
                    tick: cx.tick,
                    sequence: id,
                    track,
                    kernel_key: c.key.wrapping_add(21),
                }));
            }
            // The board ignores this command: the driver's write for it lands in ROM, not note RAM.
            Command::Nop(_) => {}
            _ => {}
        }
    }

    fn set_beat_to_rate(&mut self, id: u8) {
        let index = usize::from(id);
        self.beats[index].set_step(u32::from(self.state[index].rate));
    }

    fn start_sequence(&mut self, id: u8, cx: &mut Context) {
        if !self.bank.song(id).present {
            return;
        }
        let phase = self.state[usize::from(id)].phase;
        self.stop_sequence(id, cx);

        let song = self.bank.song(id);
        let rate = u16::from(song.rate);
        self.state[usize::from(id)] = SongState::started(rate, phase);
        // An ordinary start never interprets the selection list in the arrangement stream.
        cx.tracks.start_sequence(id, &song.initial, cx.tick, cx.mailbox);
        self.beats[usize::from(id)].reset();
        self.beats[usize::from(id)].set_step(u32::from(rate));
        self.initialize_tracks(id, id, cx);
    }

    fn initialize_tracks(&mut self, owner: u8, selected: u8, cx: &mut Context) {
        let song = self.bank.song(selected);
        if !song.present {
            panic!("arrangement selects absent HLE sequence");
        }

        let s = &mut self.state[usize::from(owner)];
        s.selected = selected;
        s.position = 0;
        s.conductor = 0;
        s.conductor_countdown = 1;
        s.conductor_ended = song.conductor().is_empty();

        cx.release_notes(owner);
        for (index, initial) in song.initial.iter().enumerate() {
            let t = index as u8;
            let previous_key = cx.tracks.previous_key(cx.tracks.track(owner, t).physical);
            let track = cx.tracks.track_mut(owner, t);
            track.config = *initial;
            track.config.parameters.set_previous_key(previous_key);
            track.cursor = 0;
            track.countdown = 1;

            let can_stream =
                track.physical != 0 && !song.music(index).is_empty() && track.config.program != NO_PROGRAM;
            track.status = if can_stream { TrackStatus::Streaming } else { TrackStatus::Inactive };
            if track.is_live() {
                cx.push_track(TrackEvent::refresh(cx.tick, owner, t));
            }
        }

        if !s.rate_override {
            s.saved_rate = u16::from(song.rate);
            s.rate = s.saved_rate;
            self.beats[usize::from(owner)].set_step(u32::from(s.rate));
        }

        self.process_due_events(owner, false, true, cx);
    }

    fn stop_sequence(&mut self, id: u8, cx: &mut Context) {
        cx.tracks.stop_sequence(id, cx.tick, cx.mailbox);
        self.beats[usize::from(id)].set_step(0);
        self.state[usize::from(id)].status = SequenceStatus::Stopped;
    }

    /// A natural end parks the sequence but keeps its track records: the driver still accepts
    /// direct note commands on them. An explicit stop removes the sequence instead.
    fn finish_sequence(&mut self, id: u8, cx: &mut Context) {
        cx.tracks.park_sequence(id, cx.tick, cx.mailbox);
        self.state[usize::from(id)].park_playback();
        self.beats[usize::from(id)].set_step(0);
    }

    fn selected_child(&self, id: u8) -> u8 {
        self.bank.song(id).selected_child(self.state[usize::from(id)].phase)
    }

    fn stop_selected_child(&mut self, id: u8, cx: &mut Context) {
        let child = self.selected_child(id);
        if child != 0 {
            self.stop_sequence(child, cx);
        }
        self.state[usize::from(id)].phase = 0;
    }

    fn advance_arrangement(
        &mut self,
        id: u8,
        previous: Option<ArrangementCarry>,
        cx: &mut Context,
    ) -> u8 {
        let list_len = self.bank.song(id).arrangement().len();

        let max_phase = list_len.min(MAX_ARRANGEMENT_PHASES);
        // A loop marker is not a playable phase. At the terminator the playlist returns to the
        // phase following its last marker.
        for _ in 0..=max_phase {
            let list = self.bank.song(id).arrangement();
            let s = &mut self.state[usize::from(id)];
            let playable = s
                .phase
                .checked_add(1)
                .filter(|&phase| usize::from(phase) <= max_phase)
                .filter(|&phase| list[usize::from(phase) - 1].opcode != EV_END);
            let Some(phase) = playable else {
                let last_marker = list.iter().rposition(|e| e.is_arrangement() && e.is_loop_marker());
                s.phase = last_marker
                    .and_then(|n| u8::try_from(n + 1).ok())
                    .filter(|&p| usize::from(p) <= MAX_ARRANGEMENT_PHASES)
                    .unwrap_or(0);
                if s.phase == 0 {
                    return 0;
                }
                continue;
            };
            s.phase = phase;

            let entry = list[usize::from(phase) - 1];
            if entry.is_loop_marker() {
                continue;
            }

            let child = entry.arrangement[0] as u8;
            if child == 0
                || usize::from(entry.arrangement[0]) >= SONG_SLOTS
                || !self.bank.song(child).present
            {
                panic!("invalid HLE arrangement selection");
            }

            self.state[usize::from(id)].arrangement_repeat = entry.arrangement[3] & 0x7f;
            if self.state[usize::from(child)].is_active() {
                return child;
            }

            self.start_sequence(child, cx);
            let next = &mut self.state[usize::from(child)];
            next.arrangement_owner = id;
            next.transpose_mask = entry.arrangement[2];
            next.transpose = entry.arrangement_transpose();

            if let Some(previous) = previous {
                cx.tracks.copy_mix(previous.selected, child);
                if let Some(saved) = previous.rate_override {
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

    fn tick_sequence(&mut self, id: u8, silent: bool, cx: &mut Context) {
        if !self.state[usize::from(id)].is_active() {
            return;
        }

        self.state[usize::from(id)].position = self.state[usize::from(id)].position.wrapping_add(1);
        let selected = self.state[usize::from(id)].selected;

        self.process_due_events(id, silent, false, cx);

        // The header's bar-boundary transition happens even when an individual track has one
        // final tick beyond that boundary. Without a bar count the song ends with its streams.
        let song = self.bank.song(selected);
        let boundary = u32::from(song.bars).wrapping_mul(u32::from(song.ticks_per_bar));
        let looping = song.flags & SEQ_FLAG_LOOP != 0;
        let state = &self.state[usize::from(id)];
        let finished = if boundary != 0 {
            state.position >= boundary
        } else {
            state.conductor_ended && !cx.tracks.tracks(id).iter().any(Track::is_streaming)
        };
        if !finished {
            return;
        }

        let owner = state.arrangement_owner;
        if owner != 0 {
            if self.state[usize::from(owner)].count_arrangement_repeat() {
                let carry = self.state[usize::from(id)].carry();
                let child = self.advance_arrangement(owner, Some(carry), cx);
                if child != id {
                    self.stop_sequence(id, cx);
                    return;
                }
            }
            self.initialize_tracks(id, id, cx);
        } else if looping {
            self.initialize_tracks(id, id, cx);
        } else {
            self.finish_sequence(id, cx);
        }
    }

    fn process_due_events(&mut self, id: u8, silent: bool, initial: bool, cx: &mut Context) {
        let selected = self.state[usize::from(id)].selected;
        let song = self.bank.song(selected);
        self.state[usize::from(id)].advance_conductor(song.conductor(), initial);

        for t in 0..TRACK_COUNT as u8 {
            let track = cx.tracks.track_mut(id, t);
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

            let events = song.music(usize::from(t));
            loop {
                let track = cx.tracks.track_mut(id, t);
                let Some(&event) = events.get(track.cursor) else {
                    break;
                };
                track.cursor += 1;
                if event.opcode == EV_END {
                    track.status = TrackStatus::Ended;
                    break;
                }
                if event.opcode < EV_NOTE_LONG_MAX {
                    if !silent {
                        self.serial = self.serial.wrapping_add(1);
                        let note = Note {
                            key: event.key,
                            velocity: event.velocity,
                            duration: event.duration,
                            instance: MUSIC_INSTANCE_BIT | self.serial,
                        };
                        cx.note_on(&self.state[usize::from(id)], id, t, note, true);
                    }
                } else {
                    cx.stream_control(id, t, &event, silent);
                }
                cx.tracks.track_mut(id, t).countdown = event.delay;
                if event.delay != 0 {
                    break;
                }
            }
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::hle::command::{
        SequenceRate, SequenceRef, SequenceSeek, SequenceStart, SequenceVolume, TrackController,
    };
    use crate::hle::driver_layout::STANDARD;
    use crate::hle::events::TrackEventKind;
    use crate::hle::sequence_bank::{
        ARRANGEMENT_LOOP_MARKER, ARRANGEMENT_STREAM_INDEX, EV_ARRANGEMENT, FIRST_MUSIC_STREAM_INDEX,
        SongData, TrackData,
    };
    use crate::hle::track_service::TrackConfig;

    const PARENT: u8 = 1;
    const FIRST_CHILD: u8 = 2;
    const SECOND_CHILD: u8 = 3;
    const SOLO: u8 = 4;
    const ABSENT: u8 = 5;

    fn end_event() -> StreamEvent {
        StreamEvent { opcode: EV_END, ..StreamEvent::default() }
    }

    fn playable_song(rate: u8) -> SongData {
        let mut song = SongData { present: true, rate, ..SongData::default() };
        song.initial[0] = TrackConfig { program: 0, ..TrackConfig::default() };
        song.tracks[FIRST_MUSIC_STREAM_INDEX] = TrackData { events: vec![end_event()] };
        song
    }

    fn playlist_entry(child: u8, repeats: u16) -> StreamEvent {
        StreamEvent {
            opcode: EV_ARRANGEMENT,
            arrangement: [u16::from(child), 0, 0, repeats],
            ..StreamEvent::default()
        }
    }

    fn loop_marker() -> StreamEvent {
        StreamEvent {
            opcode: EV_ARRANGEMENT,
            arrangement: [0, 0, 0, ARRANGEMENT_LOOP_MARKER],
            ..StreamEvent::default()
        }
    }

    fn arranged_song(entries: impl IntoIterator<Item = StreamEvent>) -> SongData {
        let mut song = SongData { present: true, ..SongData::default() };
        let mut events: Vec<StreamEvent> = entries.into_iter().collect();
        events.push(end_event());
        song.tracks[ARRANGEMENT_STREAM_INDEX] = TrackData { events };
        song
    }

    fn sequencer(songs: impl IntoIterator<Item = (u8, SongData)>) -> Sequencer {
        let mut slots: Vec<SongData> = (0..SONG_SLOTS).map(|_| SongData::default()).collect();
        for (id, song) in songs {
            slots[usize::from(id)] = song;
        }
        Sequencer {
            rom: Arc::from(&[][..]),
            layout: &STANDARD,
            bank: SequenceBank::from_songs(slots),
            state: [SongState::default(); SONG_SLOTS],
            beats: core::array::from_fn(|_| PulseTimer::new(625, 0)),
            serial: 0,
            current_bank: BOOT_BANK,
        }
    }

    struct Services {
        mailbox: Mailbox,
        tracks: Box<TrackService>,
    }

    impl Services {
        fn new() -> Services {
            Services { mailbox: Mailbox::new(), tracks: Box::new(TrackService::new()) }
        }

        fn send(&mut self, sequencer: &mut Sequencer, command: Command) {
            let packet = CommandPacket::new(0, command);
            sequencer.command(&packet, 1, 0, &mut self.mailbox, &mut self.tracks);
        }

        fn settle(&mut self) -> Vec<Event> {
            let mut events = Vec::new();
            self.mailbox.drain(|event| {
                if let Event::Track(e) = &event {
                    self.tracks.apply_track(e);
                }
                events.push(event);
            });
            events
        }
    }

    fn id(id: u8) -> SequenceRef {
        SequenceRef { id, selection: false }
    }

    fn arrangement_start(id: u8) -> Command {
        Command::SequenceStart(SequenceStart { sequence: SequenceRef { id, selection: true } })
    }

    #[test]
    fn carry_holds_the_tempo_only_when_it_was_overridden() {
        let mut state = SongState::started(90, 0);
        state.selected = 7;
        assert_eq!(state.carry(), ArrangementCarry { selected: 7, rate_override: None });
        state.override_rate(120);
        assert_eq!(state.carry(), ArrangementCarry { selected: 7, rate_override: Some(120) });
    }

    #[test]
    fn a_finished_child_hands_its_mix_and_tempo_override_to_the_next_child() {
        let mut sequencer = sequencer([
            (PARENT, arranged_song([playlist_entry(FIRST_CHILD, 1), playlist_entry(SECOND_CHILD, 1)])),
            (FIRST_CHILD, playable_song(100)),
            (SECOND_CHILD, playable_song(100)),
        ]);
        let mut services = Services::new();

        services.send(&mut sequencer, arrangement_start(PARENT));
        services.settle();
        assert!(sequencer.state[usize::from(FIRST_CHILD)].is_active());
        assert!(!sequencer.state[usize::from(SECOND_CHILD)].is_active());

        services.send(&mut sequencer, Command::SequenceRate(SequenceRate { sequence: id(FIRST_CHILD), rate: 77 }));
        services.send(
            &mut sequencer,
            Command::SequenceVolume(SequenceVolume { sequence: id(FIRST_CHILD), volume: 40 }),
        );
        services.settle();

        sequencer.pulse(FIRST_CHILD, 1, &mut services.mailbox, &mut services.tracks);
        services.settle();

        assert!(!sequencer.state[usize::from(FIRST_CHILD)].is_active());
        let next = &sequencer.state[usize::from(SECOND_CHILD)];
        assert!(next.is_active());
        assert_eq!(next.arrangement_owner, PARENT);
        assert!(next.rate_override);
        assert_eq!((next.rate, next.saved_rate), (77, 77));
        assert_eq!(services.tracks.track(SECOND_CHILD, 0).sequence_volume, 40);
    }

    #[test]
    fn seeking_an_absent_slot_does_nothing() {
        let mut sequencer = sequencer([]);
        let mut services = Services::new();
        let seek = SequenceSeek { sequence: id(ABSENT), position: 16 };
        services.send(&mut sequencer, Command::SequenceSeek(seek));
        assert!(services.settle().is_empty());
        assert!(!sequencer.state[usize::from(ABSENT)].is_active());
    }

    #[test]
    fn seeking_a_present_slot_starts_it_and_parks_it_at_the_end() {
        let mut sequencer = sequencer([(SOLO, playable_song(100))]);
        let mut services = Services::new();
        let seek = SequenceSeek { sequence: id(SOLO), position: 4 };
        services.send(&mut sequencer, Command::SequenceSeek(seek));
        assert_eq!(sequencer.state[usize::from(SOLO)].status, SequenceStatus::Parked);
    }

    fn controller_events(events: &[Event]) -> Vec<(u8, u8)> {
        events
            .iter()
            .filter_map(|event| match event {
                Event::Track(TrackEvent {
                    kind: TrackEventKind::Controller { index, value },
                    ..
                }) => Some((*index, *value)),
                _ => None,
            })
            .collect()
    }

    #[test]
    fn controllers_beyond_the_track_record_are_ignored() {
        let mut sequencer = sequencer([(SOLO, playable_song(100))]);
        let mut services = Services::new();
        services.send(&mut sequencer, Command::SequenceStart(SequenceStart { sequence: id(SOLO) }));
        services.settle();

        let last_valid = (0..=u8::MAX)
            .take_while(|&index| TrackParameters::valid_controller(usize::from(index)))
            .last()
            .unwrap();
        for index in [last_valid + 1, u8::MAX] {
            let command = TrackController { sequence: id(SOLO), track: 1, index, value: 5 };
            services.send(&mut sequencer, Command::TrackController(command));
            assert!(controller_events(&services.settle()).is_empty(), "index {index}");
        }

        let command = TrackController { sequence: id(SOLO), track: 1, index: last_valid, value: 5 };
        services.send(&mut sequencer, Command::TrackController(command));
        let queued = controller_events(&services.settle());
        assert_eq!(queued, vec![(last_valid, 5)]);
    }

    fn long_playlist(entries: usize, marker_at: Option<usize>) -> Sequencer {
        let playlist = (0..entries).map(|n| {
            if Some(n) == marker_at { loop_marker() } else { playlist_entry(FIRST_CHILD, 1) }
        });
        sequencer([(PARENT, arranged_song(playlist)), (FIRST_CHILD, playable_song(100))])
    }

    #[test]
    fn a_playlist_at_the_phase_limit_ends_without_overflowing() {
        let longest = usize::from(u8::MAX) - 2;
        let mut sequencer = long_playlist(longest, None);
        let mut services = Services::new();
        sequencer.state[usize::from(PARENT)].phase = u8::try_from(longest).unwrap();
        let mut cx = Context { tick: 0, mailbox: &mut services.mailbox, tracks: &mut services.tracks };
        assert_eq!(sequencer.advance_arrangement(PARENT, None, &mut cx), 0);
        assert_eq!(sequencer.state[usize::from(PARENT)].phase, 0);
    }

    #[test]
    fn a_phase_past_the_u8_range_returns_to_the_last_loop_marker() {
        let mut sequencer = long_playlist(3, Some(0));
        let mut services = Services::new();
        sequencer.state[usize::from(PARENT)].phase = u8::MAX;
        let mut cx = Context { tick: 0, mailbox: &mut services.mailbox, tracks: &mut services.tracks };
        assert_eq!(sequencer.advance_arrangement(PARENT, None, &mut cx), FIRST_CHILD);
        assert_eq!(sequencer.state[usize::from(PARENT)].phase, 2);
    }
}
