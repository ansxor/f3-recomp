//! `audio/hle/note_timing.hpp`: the driver clock and the queue of notes whose start it delayed.

use super::events::DRIVER_KEYON_CYCLES;

/// The sound driver programs the ES5505 voices of a sequencer pulse one after another on a CPU that is
/// also servicing the 1 kHz timer interrupt, so the Nth voice of a busy pulse sounds later than the
/// first. The clock models that single resource: a request made at `tick` for `voices` voices starts at
/// max(tick, busy) and keeps the driver busy for voices * DRIVER_KEYON_CYCLES afterwards. Requests are
/// made in the order the driver handles them (pulse order, track-major), which is the order
/// VoiceService::note_on is called in.
pub struct DriverClock {
    busy: u64,
}

impl Default for DriverClock {
    fn default() -> DriverClock {
        DriverClock::new()
    }
}

impl DriverClock {
    pub fn new() -> DriverClock {
        DriverClock { busy: 0 }
    }

    pub fn start(&mut self, tick: u64, voices: u32) -> u64 {
        let at = if tick < self.busy { self.busy } else { tick };
        self.busy = at + u64::from(voices) * u64::from(DRIVER_KEYON_CYCLES);
        at
    }

    pub fn busy_until(&self) -> u64 {
        self.busy
    }

    pub fn reset(&mut self, tick: u64) {
        self.busy = tick;
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Default)]
#[repr(u8)]
enum NoteQueueState {
    #[default]
    Free,
    Waiting,
    Started,
}

#[derive(Clone, Copy, Default)]
struct NoteQueueEntry<P> {
    payload: P,
    instance: u64,
    origin: u64,
    start: u64,
    off: u64,
    serial: u64,
    state: NoteQueueState,
    has_off: bool,
}

/// Notes whose start the driver clock pushed past the tick they were requested at. Every queued entry
/// starts exactly once, in (start, request) order. A note-off that arrives before the start is
/// remembered and applied at the start plus the note's original length, so it can neither drop the note
/// nor release it early.
pub struct NoteQueue<P: Copy + Default, const CAPACITY: usize> {
    entries: [NoteQueueEntry<P>; CAPACITY],
    serial: u64,
    used: usize,
}

impl<P: Copy + Default, const CAPACITY: usize> Default for NoteQueue<P, CAPACITY> {
    fn default() -> NoteQueue<P, CAPACITY> {
        NoteQueue::new()
    }
}

impl<P: Copy + Default, const CAPACITY: usize> NoteQueue<P, CAPACITY> {
    pub const NONE: u64 = u64::MAX;

    pub fn new() -> NoteQueue<P, CAPACITY> {
        NoteQueue { entries: [NoteQueueEntry::default(); CAPACITY], serial: 0, used: 0 }
    }

    pub fn push(&mut self, payload: P, instance: u64, origin: u64, start: u64) -> bool {
        for e in self.entries.iter_mut() {
            if e.state != NoteQueueState::Free {
                continue;
            }
            let serial = self.serial;
            self.serial += 1;
            *e = NoteQueueEntry {
                payload,
                instance,
                origin,
                start,
                off: 0,
                serial,
                state: NoteQueueState::Waiting,
                has_off: false,
            };
            self.used += 1;
            return true;
        }
        false
    }

    pub fn empty(&self) -> bool {
        self.used == 0
    }

    pub fn size(&self) -> usize {
        self.used
    }

    /// Earliest tick at which service() has something to do.
    pub fn next_event(&self) -> u64 {
        let mut next = Self::NONE;
        let mut left = self.used;
        for e in self.entries.iter() {
            if left == 0 {
                break;
            }
            match e.state {
                NoteQueueState::Waiting => {
                    next = if e.start < next { e.start } else { next };
                }
                NoteQueueState::Started => {
                    next = if e.off < next { e.off } else { next };
                }
                NoteQueueState::Free => continue,
            }
            left -= 1;
        }
        next
    }

    pub fn note_off(&mut self, instance: u64, at: u64) {
        for e in self.entries.iter_mut() {
            if e.state == NoteQueueState::Free || e.instance != instance || e.has_off {
                continue;
            }
            e.has_off = true;
            e.off = at + (e.start - if e.start < e.origin { e.start } else { e.origin });
        }
    }

    pub fn clear(&mut self) {
        for e in self.entries.iter_mut() {
            e.state = NoteQueueState::Free;
        }
        self.used = 0;
    }

    /// begin(payload, start) once per entry whose start <= limit; release(instance, tick) once per
    /// entry whose note-off arrived, never before its begin. Order: ascending tick, begins before
    /// releases at equal ticks.
    pub fn service<B: FnMut(P, u64), R: FnMut(u64, u64)>(
        &mut self,
        limit: u64,
        mut begin: B,
        mut release: R,
    ) {
        while self.used != 0 {
            let mut pick: Option<usize> = None;
            let mut pick_release = false;
            let mut left = self.used;
            for (i, e) in self.entries.iter().enumerate() {
                if left == 0 {
                    break;
                }
                if e.state == NoteQueueState::Free {
                    continue;
                }
                left -= 1;
                let rel = e.state == NoteQueueState::Started;
                let tick = if rel { e.off } else { e.start };
                if tick > limit {
                    continue;
                }
                let Some(picked) = pick else {
                    pick = Some(i);
                    pick_release = rel;
                    continue;
                };
                let best = if pick_release {
                    self.entries[picked].off
                } else {
                    self.entries[picked].start
                };
                if tick < best
                    || (tick == best
                        && ((!rel && pick_release)
                            || (rel == pick_release
                                && e.serial < self.entries[picked].serial)))
                {
                    pick = Some(i);
                    pick_release = rel;
                }
            }
            let Some(picked) = pick else {
                return;
            };
            let e = self.entries[picked];
            if pick_release {
                self.entries[picked].state = NoteQueueState::Free;
                self.used -= 1;
                release(e.instance, e.off);
                continue;
            }
            if e.has_off {
                self.entries[picked].state = NoteQueueState::Started;
            } else {
                self.entries[picked].state = NoteQueueState::Free;
                self.used -= 1;
            }
            begin(e.payload, e.start);
        }
    }
}
