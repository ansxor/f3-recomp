//! `audio/hle/voice_service.hpp` + `voice_service.cpp`: the 32-voice pool, the driver clock and the
//! delayed patches.
//!
//! Programs are already resolved to patches; this stage schedules them, keeps the voices and their
//! envelopes, and walks the slots on the 12 ms sound CPU budget ring.
//!
//! The ring is serviced from the render at the first rendered frame whose tick reached the service
//! tick, not at the clock tick itself: the 1 kHz grid can name a tick whose frame is still pending in
//! the frame -> tick map, and the pre-refactor ring was always stepped on the frame that reached the
//! tick.
//!
//! The C++ is a pimpl class; `Impl` holds the state and the public methods delegate. The C++ also hands
//! its own members (`m_voices[i]`, `m_pending[i]`) to its own methods, so the pool and the pending
//! table are reached through a raw pointer to the array where a method needs both.

use std::sync::Arc;

use super::events::{NoteOnEvent, TIMER_TICKS, TRACK_COUNT};
use super::note_pool::NoteSink;
use super::note_timing::{DriverClock, NoteQueue};
use super::patch::{AllocationMode, NoteStart, Patch, PriorWindow};
use super::program_service::PatchList;
use super::sound_bank::SoundBank;
use super::sound_rom_5a7e9117;
use super::track_service::Track;
use super::voice::{NoiseState, Voice, advance_glide};

pub struct VoiceService {
    impl_: Box<Impl>,
}

/// A note the driver clock or a delayed-patch delay starts later than it was requested.
#[derive(Clone, Copy)]
struct Pending {
    note: NoteOnEvent,
    patch: *const Patch,
    track: *const Track,
    tick: u64,
    active: bool,
    release_trigger: bool,
}

impl Default for Pending {
    fn default() -> Pending {
        Pending {
            note: NoteOnEvent::default(),
            patch: std::ptr::null(),
            track: std::ptr::null(),
            tick: 0,
            active: false,
            release_trigger: false,
        }
    }
}

struct Impl {
    bank: Arc<SoundBank>,
    voices: [Voice; 32],
    launched: u64,
    pending: [Pending; 96],
    driver: DriverClock,
    queue: NoteQueue<Pending, 96>,
    next: [u8; 32],
    prev: [u8; 32],
    cursor: u8,
    budget_slot: u8,
    noise: NoiseState,
    serial: u64,
    tick: u64,
    filter_epoch: u32,
}

impl Impl {
    fn new(bank: Arc<SoundBank>) -> Box<Impl> {
        let mut imp = Box::new(Impl {
            bank,
            voices: std::array::from_fn(|_| Voice::default()),
            launched: 0,
            pending: [Pending::default(); 96],
            driver: DriverClock::new(),
            queue: NoteQueue::new(),
            next: [0; 32],
            prev: [0; 32],
            cursor: 0,
            budget_slot: 0,
            noise: NoiseState::default(),
            serial: 0,
            tick: u64::from(TIMER_TICKS),
            filter_epoch: 0,
        });
        imp.reset(0);
        imp
    }

    fn reset(&mut self, tick: u64) {
        let pool: *mut Voice = self.voices.as_mut_ptr();
        for i in 0..32u8 {
            // SAFETY: `i` names a slot of the fixed pool; `stop` only writes that slot.
            let voice: &mut Voice = unsafe { &mut *pool.add(i as usize) };
            self.stop(voice, tick);
            *voice = Voice::default();
            voice.slot = i;
        }
        for p in self.pending.iter_mut() {
            p.active = false;
        }
        self.queue.clear();
        self.driver.reset(tick);
        for i in 0..32usize {
            self.next[i] = ((i + 1) % 32) as u8;
            self.prev[i] = ((i + 31) % 32) as u8;
        }
        self.cursor = 0;
        self.budget_slot = 0;
        self.tick = (tick / u64::from(TIMER_TICKS) + 1) * u64::from(TIMER_TICKS);
        // SAFETY: the bank outlives the service; it owns the ROM the noise seeds live in.
        self.noise.value = self.bank.rom.read_u16(sound_rom_5a7e9117::NOISE_SEED_PTR);
        self.noise.history = self
            .bank
            .rom
            .read_u16(sound_rom_5a7e9117::NOISE_HISTORY_PTR);
        self.serial = 0;
    }

    fn apply_parameters(&mut self, voice: &mut Voice, initial: bool, _tick: u64, _notify: bool) {
        voice.apply_parameters(initial);
    }

    fn move_before_cursor(&mut self, i: u32) {
        let i = i as usize;
        let mut a = self.prev[i] as usize;
        let b = self.next[i] as usize;
        self.next[a] = b as u8;
        self.prev[b] = a as u8;
        if self.cursor as usize == i {
            self.cursor = b as u8;
        }
        a = self.prev[self.cursor as usize] as usize;
        self.next[a] = i as u8;
        self.prev[i] = a as u8;
        self.next[i] = self.cursor;
        self.prev[self.cursor as usize] = i as u8;
    }

    fn allocate(
        &mut self,
        n: &NoteOnEvent,
        patch: &Patch,
        priority: u32,
        mode: AllocationMode,
    ) -> u32 {
        if mode == AllocationMode::Legato || mode == AllocationMode::LegatoHold {
            let mut found: Option<usize> = None;
            for i in 0..32 {
                let v = &self.voices[i];
                if v.active
                    && !v.released
                    && v.owner.sequence == n.sequence
                    && v.owner.track == n.track
                    && patch.reuses(v)
                {
                    match found {
                        None => found = Some(i),
                        Some(f) => {
                            if v.serial < self.voices[f].serial {
                                found = Some(i);
                            }
                        }
                    }
                }
            }
            if let Some(f) = found {
                return f as u32;
            }
        }
        if mode == AllocationMode::Retrigger {
            for i in 0..32 {
                let reuses = {
                    let v = &self.voices[i];
                    v.active
                        && !v.released
                        && v.owner.sequence == n.sequence
                        && v.owner.track == n.track
                        && patch.reuses(v)
                };
                if reuses {
                    // SAFETY: `i` names a slot of the fixed pool; `release` writes only that voice and
                    // the service's own event state.
                    let voice: &mut Voice = unsafe { &mut *self.voices.as_mut_ptr().add(i) };
                    self.release(voice, n.tick);
                    break;
                }
            }
        }
        let mut i = 32;
        while i > 0 {
            i -= 1;
            if !self.voices[i].active {
                return i as u32;
            }
        }
        for klass in [0u32, 64, 128] {
            if klass == 64 && priority == 0 {
                break;
            }
            if klass == 128 && priority <= 64 {
                break;
            }
            for released in [true, false] {
                let mut old: Option<usize> = None;
                for i in 0..32 {
                    let v = &self.voices[i];
                    if v.released == released && (klass == 128 || u32::from(v.priority) == klass) {
                        match old {
                            None => old = Some(i),
                            Some(o) => {
                                let v_k = if released { v.release_serial } else { v.serial };
                                let o_k = if released {
                                    self.voices[o].release_serial
                                } else {
                                    self.voices[o].serial
                                };
                                if v_k < o_k {
                                    old = Some(i);
                                }
                            }
                        }
                    }
                }
                if let Some(o) = old {
                    // SAFETY: `o` names a slot of the fixed pool; `stop` only writes that slot.
                    let voice: &mut Voice = unsafe { &mut *self.voices.as_mut_ptr().add(o) };
                    self.stop(voice, n.tick);
                    return o as u32;
                }
            }
        }
        32
    }

    fn begin(&mut self, n: &NoteOnEvent, patch: &Patch, track: &Track, at: u64) {
        self.launched = self.launched.wrapping_add(1);
        let kernel = i32::from(track.config.parameters.transposed_key(n.kernel_key));
        let weight = patch.weight(n, kernel);
        if weight < 0 {
            return;
        }
        let mode = patch.allocation();
        let priority = patch.priority();
        let mut prior_pitch = 0i32;
        let mut prior_found = false;
        if mode == AllocationMode::Retrigger {
            for old in self.voices.iter() {
                // SAFETY: an active voice always names the patch it was started with.
                if old.active
                    && old.owner.sequence == n.sequence
                    && old.owner.track == n.track
                    && unsafe { (*old.patch).index() } == patch.index()
                {
                    prior_pitch = i32::from(old.glide_target);
                    prior_found = true;
                    break;
                }
            }
        }
        let index = self.allocate(n, patch, priority, mode);
        if index == 32 {
            return;
        }
        // SAFETY: `index` names a slot of the fixed pool; the service's other members are separate.
        let v: &mut Voice = unsafe { &mut *self.voices.as_mut_ptr().add(index as usize) };
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
            v.owner = *n;
            v.owner.velocity = velocity;
            v.released = false;
            v.retarget(kernel, &patch.program());
            self.apply_parameters(v, false, at, true);
            return;
        }
        self.stop(v, at);
        *v = Voice::default();
        v.slot = index as u8;
        self.filter_epoch = self.filter_epoch.wrapping_add(1);
        v.filter_epoch = self.filter_epoch;
        v.owner = *n;
        v.patch = patch as *const Patch;
        // The C++ member is `const Track *`; voice.rs keeps it as `*mut Track`.
        v.track = track as *const Track as *mut Track;
        v.priority = priority as u8;
        v.allocation = mode as u8;
        v.effective_key = kernel.clamp(21, 108) as u8;
        v.weight = weight as u8;
        self.serial = self.serial.wrapping_add(1);
        v.serial = self.serial;
        let start = NoteStart {
            event: n as *const NoteOnEvent,
            kernel,
            transpose: i32::from(track.config.parameters.signed_transpose()),
            mode,
            prior_pitch,
            prior_found,
            program: patch.program(),
        };
        v.initialize(&start, &mut self.noise, &prior);
        v.active = true;
        v.hardware_valid = true;
        // Ring admission, then the opening step: a layered voice steps once here, a direct voice waits
        // for the ring.
        self.move_before_cursor(index);
        self.apply_parameters(v, true, at, false);
        let step = v.onset(&mut self.noise);
        if step.ended {
            v.active = false;
        }
    }

    fn active_voices(&self) -> u32 {
        let mut count = 0;
        for voice in self.voices.iter() {
            if voice.active {
                count += 1;
            }
        }
        count
    }

    fn launch(&mut self, n: &NoteOnEvent, patch: &Patch, track: &Track, start: u64) {
        if start == n.tick && self.queue.empty() {
            self.begin(n, patch, track, start);
            return;
        }
        let q = Pending {
            note: *n,
            patch: patch as *const Patch,
            track: track as *const Track,
            tick: start,
            active: true,
            release_trigger: false,
        };
        if !self.queue.push(q, n.instance, n.tick, start) {
            panic!("HLE driver key-on queue exhausted");
        }
    }

    fn release(&mut self, v: &mut Voice, tick: u64) {
        if v.released {
            return;
        }
        v.released = true;
        self.serial = self.serial.wrapping_add(1);
        v.release_serial = self.serial;
        if v.release() {
            self.stop(v, tick);
        }
    }

    fn stop(&mut self, voice: &mut Voice, _tick: u64) {
        voice.active = false;
    }

    fn end_sample(&mut self, voice: &mut Voice, tick: u64) {
        if voice.allocation != 0 {
            voice.sample_finished = true;
        } else {
            self.stop(voice, tick);
        }
    }

    fn release_instance(&mut self, instance: u64, tick: u64) {
        for i in 0..32 {
            // SAFETY: `i` names a slot of the fixed pool; `release` writes only that voice.
            let v: &mut Voice = unsafe { &mut *self.voices.as_mut_ptr().add(i) };
            if v.active && v.owner.instance == instance {
                self.release(v, tick);
            }
        }
    }

    fn note_on(&mut self, event: &NoteOnEvent, patches: &PatchList, track: &Track) {
        let mut note = *event;
        note.velocity &= 127; // c16faa clears the velocity flag bit.
        let list = &patches.patches[..patches.count as usize];
        let mut immediate = 0u32;
        for &patch in list {
            // SAFETY: the patch list points into the program service's tables, which outlive the note.
            if unsafe { (*patch).delay_ticks() } == 0 {
                immediate += 1;
            }
        }
        let start = if immediate != 0 {
            self.driver.start(note.tick, immediate)
        } else {
            note.tick
        };
        for &patch in list {
            // SAFETY: the patch list points into the program service's tables, which outlive the note.
            let p = unsafe { &*patch };
            if p.delay_ticks() == 0 {
                self.launch(&note, p, track, start);
                continue;
            }
            let mut slot: Option<usize> = None;
            for (i, pending) in self.pending.iter().enumerate() {
                if !pending.active {
                    slot = Some(i);
                    break;
                }
            }
            let Some(slot) = slot else {
                panic!("HLE delayed-note pool exhausted");
            };
            self.pending[slot] = Pending {
                note,
                patch,
                track: track as *const Track,
                tick: note.tick + u64::from(p.delay_ticks()) * u64::from(TIMER_TICKS),
                active: true,
                release_trigger: p.starts_on_release(),
            };
        }
    }

    fn note_off(&mut self, instance: u64, tick: u64) {
        self.queue.note_off(instance, tick);
        for i in 0..self.pending.len() {
            if !(self.pending[i].active && self.pending[i].note.instance == instance) {
                continue;
            }
            if self.pending[i].release_trigger {
                let mut n = self.pending[i].note;
                n.tick = tick;
                let patch = self.pending[i].patch;
                let track = self.pending[i].track;
                // SAFETY: a pending entry always names the patch and track of a live note.
                self.begin(&n, unsafe { &*patch }, unsafe { &*track }, tick);
            }
            self.pending[i].active = false;
        }
        self.release_instance(instance, tick);
    }

    fn service_queue(&mut self, tick: u64) {
        let this: *mut Impl = self;
        self.queue.service(
            tick,
            |p: Pending, start: u64| {
                let mut note = p.note;
                note.tick = start;
                // SAFETY: the queue is a separate field of this service and the sinks only touch the
                // pool, the driver and the pending table, as the C++ lambdas do.
                unsafe { (*this).begin(&note, &*p.patch, &*p.track, start) };
            },
            |instance: u64, at: u64| unsafe { (*this).release_instance(instance, at) },
        );
    }

    fn refresh_track(&mut self, sequence: u8, track: u8, tick: u64) {
        for i in 0..32 {
            // SAFETY: `i` names a slot of the fixed pool; the re-applied parameters write only that voice.
            let v: &mut Voice = unsafe { &mut *self.voices.as_mut_ptr().add(i) };
            if v.active && v.owner.sequence == sequence && v.owner.track == track {
                self.apply_parameters(v, false, tick, true);
            }
        }
    }

    fn refresh_sequence(&mut self, sequence: u8, tick: u64) {
        // Track-major, as the old per-track channel sync was: all voices of track 0, then track 1, ...
        for track in 0..TRACK_COUNT {
            let track = track as u8;
            for i in 0..32 {
                // SAFETY: `i` names a slot of the fixed pool; the re-applied parameters write only that
                // voice.
                let v: &mut Voice = unsafe { &mut *self.voices.as_mut_ptr().add(i) };
                if v.active && v.owner.sequence == sequence && v.owner.track == track {
                    self.apply_parameters(v, false, tick, true);
                }
            }
        }
    }

    fn service(&mut self) {
        let tick = self.tick;
        self.tick = self.tick.wrapping_add(u64::from(TIMER_TICKS));
        // Delayed layered patches whose delay elapsed; the start still pays the driver's key-on cost.
        for i in 0..self.pending.len() {
            if !(self.pending[i].active
                && !self.pending[i].release_trigger
                && self.pending[i].tick <= tick)
            {
                continue;
            }
            let mut n = self.pending[i].note;
            n.tick = self.pending[i].tick;
            self.pending[i].active = false;
            let patch = self.pending[i].patch;
            let track = self.pending[i].track;
            let start = self.driver.start(n.tick, 1);
            // SAFETY: a pending entry always names the patch and track of a live note.
            unsafe { self.launch(&n, &*patch, &*track, start) };
        }
        // SAFETY: the bank outlives the service and owns the budget table.
        let mut count = u32::from(self.bank.budgets[self.budget_slot as usize]);
        self.budget_slot = ((u32::from(self.budget_slot) + 1) % 12) as u8;
        while count != 0 {
            count -= 1;
            let index = u32::from(self.cursor);
            self.cursor = self.next[self.cursor as usize];
            // SAFETY: `index` names a slot of the fixed pool; the ring steps one voice at a time.
            let v: &mut Voice = unsafe { &mut *self.voices.as_mut_ptr().add(index as usize) };
            if !v.active {
                continue;
            }
            let step = v.step(&mut self.noise);
            if step.ended {
                self.stop(v, tick);
                continue;
            }
            advance_glide(v);
            self.apply_parameters(v, false, tick, true);
        }
    }
}

impl VoiceService {
    pub fn new(bank: Arc<SoundBank>) -> VoiceService {
        VoiceService {
            impl_: Impl::new(bank),
        }
    }

    pub fn reset(&mut self, tick: u64) {
        self.impl_.reset(tick);
    }

    /// Starts every patch of the note at its driver start tick (or schedules its delay/release trigger).
    pub fn note_on(&mut self, event: &NoteOnEvent, patches: &PatchList, track: &Track) {
        self.impl_.note_on(event, patches, track);
    }

    pub fn note_off(&mut self, instance: u64, tick: u64) {
        self.impl_.note_off(instance, tick);
    }

    /// Re-runs the voice parameters of one track's / one sequence's live voices after a track event.
    pub fn refresh_track(&mut self, sequence: u8, track: u8, tick: u64) {
        self.impl_.refresh_track(sequence, track, tick);
    }

    pub fn refresh_sequence(&mut self, sequence: u8, tick: u64) {
        self.impl_.refresh_sequence(sequence, tick);
    }

    /// The ring's own 1 kHz service tick (grid-aligned from reset) and the service itself: due delayed
    /// patches, then the budget ring. The render calls it when it reaches the tick.
    pub fn next_tick(&self) -> u64 {
        self.impl_.tick
    }

    pub fn service(&mut self) {
        self.impl_.service();
    }

    /// Driver starts that fall between two service ticks, at the exact tick the render reaches them.
    pub fn service_queue(&mut self, tick: u64) {
        self.impl_.service_queue(tick);
    }

    pub fn next_queue_tick(&self) -> u64 {
        self.impl_.queue.next_event()
    }

    pub fn pool(&mut self) -> &mut [Voice; 32] {
        &mut self.impl_.voices
    }

    pub fn launched(&self) -> u64 {
        self.impl_.launched
    }

    pub fn active_voices(&self) -> u32 {
        self.impl_.active_voices()
    }

    /// Deactivates a voice. `end_sample` keeps a retriggered voice's slot for its envelope restart.
    pub fn stop(&mut self, voice: &mut Voice, tick: u64) {
        self.impl_.stop(voice, tick);
    }

    pub fn end_sample(&mut self, voice: &mut Voice, tick: u64) {
        self.impl_.end_sample(voice, tick);
    }

    // The voice-event observer is compiled out (`F3RT_HLE_EVENTS`): the render hooks do nothing.
    pub fn begin_render(&mut self) {}

    pub fn finish_render(&mut self) {}

    pub fn set_render_voice(&mut self, _voice: u32) {}

    pub fn set_render_frame(&mut self, _frame: usize) {}
}

impl NoteSink for VoiceService {
    fn note_off(&mut self, instance: u64, tick: u64) {
        Impl::note_off(&mut self.impl_, instance, tick);
    }
}
