use super::events::DRIVER_KEYON_TICKS;

/// The sound driver programs the ES5505 voices of a sequencer pulse one after another on a CPU that is
/// also servicing the 1 kHz timer interrupt, so the Nth voice of a busy pulse sounds later than the
/// first. The clock models that single resource: a request made at `tick` for `voices` voices starts at
/// max(tick, busy) and keeps the driver busy for voices * DRIVER_KEYON_TICKS afterwards. Requests are
/// made in the order the driver handles them (pulse order, track-major), which is the order
/// notes reach the voice service in.
#[derive(Default)]
pub struct DriverClock {
    busy: u64,
}

impl DriverClock {
    pub fn new() -> DriverClock {
        DriverClock::default()
    }

    pub fn start(&mut self, tick: u64, voices: u32) -> u64 {
        let at = tick.max(self.busy);
        self.busy = at + u64::from(voices) * u64::from(DRIVER_KEYON_TICKS);
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

impl<P> NoteQueueEntry<P> {
    fn due_tick(&self) -> u64 {
        if self.state == NoteQueueState::Started {
            self.off
        } else {
            self.start
        }
    }
}

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub enum QueueAction<P> {
    Begin { payload: P, start: u64 },
    Release { instance: u64, at: u64 },
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
        let Some(slot) = self.entries.iter_mut().find(|e| e.state == NoteQueueState::Free) else {
            return false;
        };
        *slot = NoteQueueEntry {
            payload,
            instance,
            origin,
            start,
            off: 0,
            serial: self.serial,
            state: NoteQueueState::Waiting,
            has_off: false,
        };
        self.serial += 1;
        self.used += 1;
        true
    }

    pub fn empty(&self) -> bool {
        self.used == 0
    }

    /// Earliest tick at which `pop_due` has something to hand out.
    pub fn next_event(&self) -> u64 {
        if self.used == 0 {
            return Self::NONE;
        }
        self.live().map(|(_, e)| e.due_tick()).min().unwrap_or(Self::NONE)
    }

    pub fn note_off(&mut self, instance: u64, at: u64) {
        let mut remaining = self.used;
        for e in self.entries.iter_mut() {
            if remaining == 0 {
                return;
            }
            if e.state == NoteQueueState::Free {
                continue;
            }
            remaining -= 1;
            if e.instance != instance || e.has_off {
                continue;
            }
            e.has_off = true;
            e.off = at + e.start.saturating_sub(e.origin);
        }
    }

    pub fn clear(&mut self) {
        for e in self.entries.iter_mut() {
            e.state = NoteQueueState::Free;
        }
        self.used = 0;
    }

    /// The next action due at or before `limit`: ascending tick, begins before releases at equal
    /// ticks, request order among equals. A release never precedes its begin.
    pub fn pop_due(&mut self, limit: u64) -> Option<QueueAction<P>> {
        let picked = self.pick_next_due(limit)?;
        let e = self.entries[picked];
        if e.state == NoteQueueState::Started {
            self.entries[picked].state = NoteQueueState::Free;
            self.used -= 1;
            return Some(QueueAction::Release { instance: e.instance, at: e.off });
        }
        if e.has_off {
            self.entries[picked].state = NoteQueueState::Started;
        } else {
            self.entries[picked].state = NoteQueueState::Free;
            self.used -= 1;
        }
        Some(QueueAction::Begin { payload: e.payload, start: e.start })
    }

    fn live(&self) -> impl Iterator<Item = (usize, &NoteQueueEntry<P>)> {
        self.entries
            .iter()
            .enumerate()
            .filter(|(_, e)| e.state != NoteQueueState::Free)
            .take(self.used)
    }

    fn pick_next_due(&self, limit: u64) -> Option<usize> {
        if self.used == 0 {
            return None;
        }
        self.live()
            .filter(|(_, e)| e.due_tick() <= limit)
            .min_by_key(|(_, e)| (e.due_tick(), e.state == NoteQueueState::Started, e.serial))
            .map(|(i, _)| i)
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    type Queue = NoteQueue<u32, 4>;

    fn drain(queue: &mut Queue, limit: u64) -> Vec<QueueAction<u32>> {
        std::iter::from_fn(|| queue.pop_due(limit)).collect()
    }

    #[test]
    fn driver_clock_staggers_voices_of_one_pulse() {
        let mut clock = DriverClock::new();
        let cycles = u64::from(DRIVER_KEYON_TICKS);
        assert_eq!(clock.start(1000, 2), 1000);
        assert_eq!(clock.start(1000, 1), 1000 + 2 * cycles);
        assert_eq!(clock.busy_until(), 1000 + 3 * cycles);
    }

    #[test]
    fn driver_clock_idles_until_the_request() {
        let mut clock = DriverClock::new();
        clock.start(0, 1);
        let later = 10 * u64::from(DRIVER_KEYON_TICKS);
        assert_eq!(clock.start(later, 1), later);
    }

    #[test]
    fn driver_clock_reset_drops_the_backlog() {
        let mut clock = DriverClock::new();
        clock.start(0, 8);
        clock.reset(500);
        assert_eq!(clock.start(500, 1), 500);
    }

    #[test]
    fn note_off_before_a_delayed_start_keeps_the_note_length() {
        let mut queue = Queue::new();
        assert!(queue.push(7, 1, 100, 300));
        queue.note_off(1, 150);
        assert_eq!(queue.next_event(), 300);
        assert!(drain(&mut queue, 299).is_empty());
        assert_eq!(drain(&mut queue, 300), [QueueAction::Begin { payload: 7, start: 300 }]);
        assert_eq!(queue.next_event(), 350);
        assert!(drain(&mut queue, 349).is_empty());
        assert_eq!(drain(&mut queue, 350), [QueueAction::Release { instance: 1, at: 350 }]);
        assert!(queue.empty());
    }

    #[test]
    fn note_without_a_note_off_starts_once_and_leaves() {
        let mut queue = Queue::new();
        queue.push(1, 1, 0, 10);
        assert_eq!(drain(&mut queue, 1000), [QueueAction::Begin { payload: 1, start: 10 }]);
        assert!(queue.empty());
        assert_eq!(queue.next_event(), Queue::NONE);
    }

    #[test]
    fn begins_precede_releases_at_equal_ticks_and_ties_keep_request_order() {
        let mut queue = Queue::new();
        queue.push(1, 1, 0, 100);
        queue.note_off(1, 300);
        queue.push(2, 2, 400, 400);
        queue.push(3, 3, 400, 400);
        assert_eq!(
            drain(&mut queue, 400),
            [
                QueueAction::Begin { payload: 1, start: 100 },
                QueueAction::Begin { payload: 2, start: 400 },
                QueueAction::Begin { payload: 3, start: 400 },
                QueueAction::Release { instance: 1, at: 400 },
            ]
        );
    }

    #[test]
    fn push_fails_when_every_slot_is_taken() {
        let mut queue = Queue::new();
        for i in 0..4 {
            assert!(queue.push(i, u64::from(i), 0, 10));
        }
        assert!(!queue.push(9, 9, 0, 10));
    }
}
