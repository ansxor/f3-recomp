//! The 32-voice pool, the driver clock and the delayed patches.
//!
//! Programs are already resolved to patches; this stage schedules them, keeps the voices and their
//! envelopes, and walks the slots on the 12 ms sound CPU budget ring.
//!
//! The ring is serviced from the render at the first rendered frame whose tick reached the service
//! tick, not at the clock tick itself: the 1 kHz grid can name a tick whose frame is still pending in
//! the frame -> tick map, and the pre-refactor ring was always stepped on the frame that reached the
//! tick.
//!
//! `VoicePool` owns the voices and the ring. `VoiceService` sits between the sequencer's notes and the
//! pool: it owns the driver clock, the key-on queue and the delayed-patch table that decide when a
//! note reaches the pool.

use super::events::{NoteOnEvent, TIMER_TICKS, TRACK_COUNT};
use super::note_timing::{DriverClock, NoteQueue, QueueAction};
use super::patch::{AllocationMode, NoteStart, Patch, PatchList, PriorWindow};
use super::sound_bank::SoundBank;
use super::track_service::{Track, TrackService};
use super::voice::{NoiseState, Voice};

const VOICES: usize = 32;
const PENDING_NOTES: usize = 96;

/// The track a note was played on. Voices and pending notes name their track by sequence and index.
fn track_of<'a>(tracks: &'a mut TrackService, note: &NoteOnEvent) -> &'a Track {
    &tracks.tracks(note.sequence)[usize::from(note.track)]
}

/// The voices, the budget ring that steps them and the shared noise and ordering counters.
pub struct VoicePool {
    pub voices: [Voice; VOICES],
    next: [u8; VOICES],
    prev: [u8; VOICES],
    cursor: u8,
    budget_slot: u8,
    noise: NoiseState,
    serial: u64,
    filter_epoch: u32,
    launched: u64,
}

impl VoicePool {
    pub fn new(bank: &SoundBank) -> VoicePool {
        let mut pool = VoicePool {
            voices: std::array::from_fn(|_| Voice::default()),
            next: [0; VOICES],
            prev: [0; VOICES],
            cursor: 0,
            budget_slot: 0,
            noise: NoiseState::default(),
            serial: 0,
            filter_epoch: 0,
            launched: 0,
        };
        pool.reset(bank);
        pool
    }

    pub fn reset(&mut self, bank: &SoundBank) {
        for (slot, voice) in self.voices.iter_mut().enumerate() {
            *voice = Voice::default();
            voice.slot = slot as u8;
        }
        for i in 0..VOICES {
            self.next[i] = ((i + 1) % VOICES) as u8;
            self.prev[i] = ((i + VOICES - 1) % VOICES) as u8;
        }
        self.cursor = 0;
        self.budget_slot = 0;
        self.noise.value = bank.rom.read_u16(bank.layout.noise_seed_ptr);
        self.noise.history = bank.rom.read_u16(bank.layout.noise_history_ptr);
        self.serial = 0;
    }

    pub fn launched(&self) -> u64 {
        self.launched
    }

    pub fn active_voices(&self) -> u32 {
        self.voices.iter().filter(|voice| voice.active).count() as u32
    }

    fn move_before_cursor(&mut self, i: usize) {
        let before = self.prev[i] as usize;
        let after = self.next[i] as usize;
        self.next[before] = after as u8;
        self.prev[after] = before as u8;
        if self.cursor as usize == i {
            self.cursor = after as u8;
        }
        let tail = self.prev[self.cursor as usize] as usize;
        self.next[tail] = i as u8;
        self.prev[i] = tail as u8;
        self.next[i] = self.cursor;
        self.prev[self.cursor as usize] = i as u8;
    }

    /// Whether `voice` plays a note of the same track and layer slot as `note`, and may be taken over.
    fn takeable_by(voice: &Voice, note: &NoteOnEvent, patch: &Patch) -> bool {
        voice.active
            && !voice.released
            && voice.owner.sequence == note.sequence
            && voice.owner.track == note.track
            && patch.reuses(voice)
    }

    /// The voice index the note takes, stealing by class and age when none is free; `VOICES` when
    /// nothing may be stolen.
    fn allocate(
        &mut self,
        note: &NoteOnEvent,
        patch: &Patch,
        priority: u32,
        mode: AllocationMode,
        bank: &SoundBank,
    ) -> usize {
        if mode == AllocationMode::Legato || mode == AllocationMode::LegatoHold {
            let oldest = self
                .voices
                .iter()
                .enumerate()
                .filter(|(_, v)| Self::takeable_by(v, note, patch))
                .min_by_key(|(_, v)| v.serial);
            if let Some((index, _)) = oldest {
                return index;
            }
        }
        if mode == AllocationMode::Retrigger {
            let held = self
                .voices
                .iter()
                .position(|v| Self::takeable_by(v, note, patch));
            if let Some(index) = held {
                self.release(index, bank);
            }
        }
        if let Some(free) = self.voices.iter().rposition(|v| !v.active) {
            return free;
        }
        for class in [0u32, 64, 128] {
            if class == 64 && priority == 0 {
                break;
            }
            if class == 128 && priority <= 64 {
                break;
            }
            for released in [true, false] {
                let age = |v: &Voice| if released { v.release_serial } else { v.serial };
                let oldest = self
                    .voices
                    .iter()
                    .enumerate()
                    .filter(|(_, v)| {
                        v.released == released && (class == 128 || u32::from(v.priority) == class)
                    })
                    .min_by_key(|(_, v)| age(v));
                if let Some((index, _)) = oldest {
                    self.voices[index].stop();
                    return index;
                }
            }
        }
        VOICES
    }

    /// Starts the note's patch on a voice: the allocation, then the ring admission and the opening
    /// step. A legato note re-enters the running voice instead.
    pub fn begin(&mut self, note: &NoteOnEvent, patch: Patch, bank: &SoundBank, track: &Track) {
        self.launched = self.launched.wrapping_add(1);
        let kernel = i32::from(track.config.parameters.transposed_key(note.kernel_key));
        let weight = patch.weight(note, kernel, bank);
        if weight < 0 {
            return;
        }
        let mode = patch.allocation();
        let priority = patch.priority();
        let mut prior_pitch = 0i32;
        let mut prior_found = false;
        if mode == AllocationMode::Retrigger {
            let previous = self.voices.iter().find(|old| {
                old.active
                    && old.owner.sequence == note.sequence
                    && old.owner.track == note.track
                    && old.patch.map(|p| p.index()) == Some(patch.index())
            });
            if let Some(old) = previous {
                prior_pitch = i32::from(old.glide_target);
                prior_found = true;
            }
        }
        let index = self.allocate(note, &patch, priority, mode, bank);
        if index == VOICES {
            return;
        }
        let v = &mut self.voices[index];
        let legato =
            v.active && (mode == AllocationMode::Legato || mode == AllocationMode::LegatoHold);
        let old_board = f64::from((u32::from(v.selector) >> 1) << 20);
        let prior = PriorWindow {
            start: v.start - old_board,
            end: v.end - old_board,
            valid: v.hardware_valid,
        };
        if legato {
            let velocity = v.owner.velocity;
            v.owner = *note;
            v.owner.velocity = velocity;
            v.released = false;
            v.retarget(kernel, &patch.program(), bank);
            v.apply_parameters(false, bank, track);
            return;
        }
        v.stop();

        *v = Voice::default();
        v.slot = index as u8;
        self.filter_epoch = self.filter_epoch.wrapping_add(1);
        v.filter_epoch = self.filter_epoch;
        v.owner = *note;
        v.patch = Some(patch);
        v.priority = priority as u8;
        v.allocation = mode as u8;
        v.effective_key = kernel.clamp(21, 108) as u8;
        v.weight = weight as u8;
        self.serial = self.serial.wrapping_add(1);
        v.serial = self.serial;
        let start = NoteStart {
            previous_key: note.previous_key,
            kernel,
            transpose: i32::from(track.config.parameters.signed_transpose()),
            mode,
            prior_pitch,
            prior_found,
            program: patch.program(),
        };
        v.initialize(&start, &mut self.noise, &prior, bank, track);
        v.active = true;
        v.hardware_valid = true;
        // Ring admission, then the opening step: a layered voice steps once here, a direct voice waits
        // for the ring.
        self.move_before_cursor(index);
        let v = &mut self.voices[index];
        v.apply_parameters(true, bank, track);
        let step = v.onset(&mut self.noise, bank, track);
        if step.ended {
            v.active = false;
        }
    }

    fn release(&mut self, index: usize, bank: &SoundBank) {
        if self.voices[index].released {
            return;
        }
        self.serial = self.serial.wrapping_add(1);
        let voice = &mut self.voices[index];
        voice.released = true;
        voice.release_serial = self.serial;
        if voice.release(bank) {
            voice.stop();
        }
    }

    pub fn release_instance(&mut self, instance: u64, bank: &SoundBank) {
        for index in 0..VOICES {
            let voice = &self.voices[index];
            if voice.active && voice.owner.instance == instance {
                self.release(index, bank);
            }
        }
    }

    pub fn refresh_track(&mut self, sequence: u8, track_index: u8, track: &Track, bank: &SoundBank) {
        for voice in &mut self.voices {
            if voice.active && voice.owner.sequence == sequence && voice.owner.track == track_index {
                voice.apply_parameters(false, bank, track);
            }
        }
    }

    /// Track-major: all voices of track 0, then track 1, and so on.
    pub fn refresh_sequence(
        &mut self,
        sequence: u8,
        tracks: &[Track; TRACK_COUNT],
        bank: &SoundBank,
    ) {
        for (index, track) in tracks.iter().enumerate() {
            self.refresh_track(sequence, index as u8, track, bank);
        }
    }

    /// Steps the voices the current budget slot of the ring reaches.
    pub fn service_ring(&mut self, bank: &SoundBank, tracks: &mut TrackService) {
        let mut count = bank.budgets[usize::from(self.budget_slot)];
        self.budget_slot = (self.budget_slot + 1) % 12;
        while count != 0 {
            count -= 1;
            let index = usize::from(self.cursor);
            self.cursor = self.next[index];
            let v = &mut self.voices[index];
            if !v.active {
                continue;
            }
            let track = track_of(tracks, &v.owner);
            let step = v.step(&mut self.noise, bank, track);
            if step.ended {
                v.stop();
                continue;
            }
            v.advance_glide();
            v.apply_parameters(false, bank, track);
        }
    }
}

/// A note the driver clock or a delayed-patch delay starts later than it was requested.
#[derive(Clone, Copy, Default)]
struct Pending {
    note: NoteOnEvent,
    patch: Option<Patch>,
    tick: u64,
    active: bool,
    release_trigger: bool,
}

impl Pending {
    fn patch(&self) -> Patch {
        self.patch.expect("a pending note names its patch")
    }
}

pub struct VoiceService {
    pub pool: VoicePool,
    queue: NoteQueue<Pending, PENDING_NOTES>,
    pending: [Pending; PENDING_NOTES],
    driver: DriverClock,
    tick: u64,
}

impl VoiceService {
    pub fn new(bank: &SoundBank) -> VoiceService {
        let mut service = VoiceService {
            pool: VoicePool::new(bank),
            queue: NoteQueue::new(),
            pending: [Pending::default(); PENDING_NOTES],
            driver: DriverClock::new(),
            tick: u64::from(TIMER_TICKS),
        };
        service.reset(0, bank);
        service
    }

    pub fn reset(&mut self, tick: u64, bank: &SoundBank) {
        self.pool.reset(bank);
        for pending in self.pending.iter_mut() {
            pending.active = false;
        }
        self.queue.clear();
        self.driver.reset(tick);
        self.tick = (tick / u64::from(TIMER_TICKS) + 1) * u64::from(TIMER_TICKS);
    }

    pub fn note_on(
        &mut self,
        event: &NoteOnEvent,
        patches: &PatchList,
        bank: &SoundBank,
        tracks: &mut TrackService,
    ) {
        let mut note = *event;
        note.velocity &= 127; // c16faa clears the velocity flag bit.
        let immediate = patches.iter().filter(|patch| patch.delay_ticks() == 0).count() as u32;
        let start = if immediate != 0 {
            self.driver.start(note.tick, immediate)
        } else {
            note.tick
        };
        for patch in patches.iter() {
            if patch.delay_ticks() == 0 {
                self.launch(&note, patch, bank, tracks, start);
                continue;
            }
            let Some(slot) = self.pending.iter_mut().find(|pending| !pending.active) else {
                panic!("HLE delayed-note pool exhausted");
            };
            *slot = Pending {
                note,
                patch: Some(patch),
                tick: note.tick + u64::from(patch.delay_ticks()) * u64::from(TIMER_TICKS),
                active: true,
                release_trigger: patch.starts_on_release(),
            };
        }
    }

    pub fn note_off(&mut self, instance: u64, tick: u64, bank: &SoundBank, tracks: &mut TrackService) {
        self.queue.note_off(instance, tick);
        for pending in self.pending.iter_mut() {
            if !(pending.active && pending.note.instance == instance) {
                continue;
            }
            if pending.release_trigger {
                let mut note = pending.note;
                note.tick = tick;
                self.pool.begin(&note, pending.patch(), bank, track_of(tracks, &note));
            }
            pending.active = false;
        }
        self.pool.release_instance(instance, bank);
    }

    /// Starts the queued notes and releases the queued note-offs the driver reached by `tick`.
    pub fn service_queue(&mut self, tick: u64, bank: &SoundBank, tracks: &mut TrackService) {
        while let Some(action) = self.queue.pop_due(tick) {
            match action {
                QueueAction::Begin { payload, start } => {
                    let mut note = payload.note;
                    note.tick = start;
                    self.pool.begin(&note, payload.patch(), bank, track_of(tracks, &note));
                }
                QueueAction::Release { instance, .. } => self.pool.release_instance(instance, bank),
            }
        }
    }

    pub fn refresh_track(
        &mut self,
        sequence: u8,
        track_index: u8,
        bank: &SoundBank,
        tracks: &mut TrackService,
    ) {
        let track = &tracks.tracks(sequence)[usize::from(track_index)];
        self.pool.refresh_track(sequence, track_index, track, bank);
    }

    pub fn refresh_sequence(&mut self, sequence: u8, bank: &SoundBank, tracks: &mut TrackService) {
        self.pool.refresh_sequence(sequence, tracks.tracks(sequence), bank);
    }

    /// One 1 kHz service tick: due delayed patches, then the budget ring.
    pub fn service(&mut self, bank: &SoundBank, tracks: &mut TrackService) {
        let tick = self.tick;
        self.tick = self.tick.wrapping_add(u64::from(TIMER_TICKS));
        // Delayed layered patches whose delay elapsed; the start still pays the driver's key-on cost.
        for i in 0..self.pending.len() {
            let pending = self.pending[i];
            if !(pending.active && !pending.release_trigger && pending.tick <= tick) {
                continue;
            }
            self.pending[i].active = false;
            let mut note = pending.note;
            note.tick = pending.tick;
            let start = self.driver.start(note.tick, 1);
            self.launch(&note, pending.patch(), bank, tracks, start);
        }
        self.pool.service_ring(bank, tracks);
    }

    /// Starts the note now when the driver is free, else queues it for the tick the driver reaches it.
    fn launch(
        &mut self,
        note: &NoteOnEvent,
        patch: Patch,
        bank: &SoundBank,
        tracks: &mut TrackService,
        start: u64,
    ) {
        if start == note.tick && self.queue.empty() {
            self.pool.begin(note, patch, bank, track_of(tracks, note));
            return;
        }
        let queued = Pending {
            note: *note,
            patch: Some(patch),
            tick: start,
            active: true,
            release_trigger: false,
        };
        if !self.queue.push(queued, note.instance, note.tick, start) {
            panic!("HLE driver key-on queue exhausted");
        }
    }

    /// The ring's own 1 kHz service tick, grid-aligned from reset. The render calls `service` when it
    /// reaches the tick.
    pub fn next_tick(&self) -> u64 {
        self.tick
    }

    pub fn next_queue_tick(&self) -> u64 {
        self.queue.next_event()
    }

    pub fn voices_mut(&mut self) -> &mut [Voice; VOICES] {
        &mut self.pool.voices
    }

    pub fn launched(&self) -> u64 {
        self.pool.launched()
    }

    pub fn active_voices(&self) -> u32 {
        self.pool.active_voices()
    }
}
