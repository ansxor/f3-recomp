//! The per-track note bookkeeping pool.
//!
//! Each track owns an intrusive singly linked list of `ActiveNote` slots (its head index lives in
//! the track); the pool hands out slots from a free list and takes them back. Note-offs the pool
//! decides on go to a caller-supplied `on_note_off(instance, tick)`.

pub const NOTE_CAPACITY: usize = 4096;
pub const NO_NOTE: u16 = 0xffff;

#[derive(Clone, Copy, PartialEq, Eq, Debug)]
pub struct ActiveNote {
    pub instance: u64,
    pub next: u16,
    pub remaining: u16,
    pub key: u8,
    pub kernel_key: u8,
}

impl Default for ActiveNote {
    fn default() -> ActiveNote {
        ActiveNote { instance: 0, next: NO_NOTE, remaining: 0, key: 0, kernel_key: 0 }
    }
}

pub struct NotePool {
    notes: Box<[ActiveNote; NOTE_CAPACITY]>,
    free_head: u16,
}

impl Default for NotePool {
    fn default() -> NotePool {
        NotePool::new()
    }
}

impl NotePool {
    pub fn new() -> NotePool {
        // Boxed so the 4096-slot table never lands on a stack frame.
        let notes: Box<[ActiveNote; NOTE_CAPACITY]> = vec![ActiveNote::default(); NOTE_CAPACITY]
            .into_boxed_slice()
            .try_into()
            .unwrap_or_else(|_| unreachable!());
        let mut pool = NotePool { notes, free_head: 0 };
        pool.reset();
        pool
    }

    /// Chains every slot into the free list in index order.
    pub fn reset(&mut self) {
        self.free_head = 0;
        for (n, note) in self.notes.iter_mut().enumerate() {
            *note = ActiveNote {
                next: if n + 1 == NOTE_CAPACITY { NO_NOTE } else { (n + 1) as u16 },
                ..ActiveNote::default()
            };
        }
    }

    /// Pushes a note on the front of the track's list.
    pub fn allocate_note(
        &mut self,
        track_head: &mut u16,
        instance: u64,
        key: u8,
        kernel_key: u8,
        duration: u16,
    ) {
        if self.free_head == NO_NOTE {
            panic!("HLE note bookkeeping pool exhausted");
        }
        let index = self.free_head;
        self.free_head = self.notes[index as usize].next;

        let n = &mut self.notes[index as usize];
        n.instance = instance;
        n.key = key;
        n.kernel_key = kernel_key;
        n.remaining = duration;
        n.next = *track_head;
        *track_head = index;
    }

    pub fn release_all(
        &mut self,
        track_head: &mut u16,
        tick: u64,
        mut on_note_off: impl FnMut(u64, u64),
    ) {
        while *track_head != NO_NOTE {
            let n = *track_head;
            *track_head = self.notes[n as usize].next;
            on_note_off(self.notes[n as usize].instance, tick);
            self.release_entry(n);
        }
    }

    /// Unlinks the first note of `kernel_key` and returns its instance id (0 when none). Reports no
    /// note-off: the caller releases the voice itself.
    pub fn release_matching_key(&mut self, track_head: &mut u16, kernel_key: u8) -> u64 {
        // `None` is the list head, `Some(i)` the `next` link of note `i`.
        let mut link: Option<usize> = None;
        loop {
            let n = match link {
                None => *track_head,
                Some(i) => self.notes[i].next,
            };
            if n == NO_NOTE {
                break;
            }
            let index = n as usize;
            if self.notes[index].kernel_key == kernel_key {
                let next = self.notes[index].next;
                match link {
                    None => *track_head = next,
                    Some(i) => self.notes[i].next = next,
                }
                let instance = self.notes[index].instance;
                self.release_entry(n);
                return instance;
            }
            link = Some(index);
        }
        0
    }

    /// Counts every note down one pulse and releases those that reach zero. A note allocated with
    /// duration 0 is released on its first pulse.
    pub fn step_durations(
        &mut self,
        track_head: &mut u16,
        tick: u64,
        mut on_note_off: impl FnMut(u64, u64),
    ) {
        let mut link: Option<usize> = None;
        loop {
            let n = match link {
                None => *track_head,
                Some(i) => self.notes[i].next,
            };
            if n == NO_NOTE {
                return;
            }
            let index = n as usize;
            self.notes[index].remaining = self.notes[index].remaining.saturating_sub(1);
            if self.notes[index].remaining == 0 {
                let next = self.notes[index].next;
                match link {
                    None => *track_head = next,
                    Some(i) => self.notes[i].next = next,
                }
                on_note_off(self.notes[index].instance, tick);
                self.release_entry(n);
            } else {
                link = Some(index);
            }
        }
    }

    fn release_entry(&mut self, index: u16) {
        self.notes[index as usize].next = self.free_head;
        self.free_head = index;
    }
}

#[cfg(test)]
mod tests {
    use super::*;

    fn offs(f: impl FnOnce(&mut dyn FnMut(u64, u64))) -> Vec<(u64, u64)> {
        let mut out = Vec::new();
        f(&mut |instance, tick| out.push((instance, tick)));
        out
    }

    #[test]
    fn note_is_released_when_its_duration_runs_out() {
        let mut pool = NotePool::new();
        let mut head = NO_NOTE;
        pool.allocate_note(&mut head, 7, 60, 81, 2);

        assert!(offs(|f| pool.step_durations(&mut head, 10, f)).is_empty());
        assert_eq!(offs(|f| pool.step_durations(&mut head, 11, f)), vec![(7, 11)]);
        assert_eq!(head, NO_NOTE);
    }

    #[test]
    fn zero_duration_note_is_released_on_first_pulse() {
        let mut pool = NotePool::new();
        let mut head = NO_NOTE;
        pool.allocate_note(&mut head, 3, 60, 81, 0);
        assert_eq!(offs(|f| pool.step_durations(&mut head, 5, f)), vec![(3, 5)]);
    }

    #[test]
    fn stepping_keeps_unexpired_notes_linked() {
        let mut pool = NotePool::new();
        let mut head = NO_NOTE;
        pool.allocate_note(&mut head, 1, 60, 81, 1);
        pool.allocate_note(&mut head, 2, 61, 82, 3);
        pool.allocate_note(&mut head, 3, 62, 83, 1);

        assert_eq!(offs(|f| pool.step_durations(&mut head, 0, f)), vec![(3, 0), (1, 0)]);
        assert_eq!(pool.release_matching_key(&mut head, 82), 2);
    }

    #[test]
    fn release_matching_key_unlinks_only_the_first_match() {
        let mut pool = NotePool::new();
        let mut head = NO_NOTE;
        pool.allocate_note(&mut head, 1, 60, 81, 9);
        pool.allocate_note(&mut head, 2, 60, 81, 9);
        pool.allocate_note(&mut head, 3, 61, 82, 9);

        assert_eq!(pool.release_matching_key(&mut head, 81), 2);
        assert_eq!(pool.release_matching_key(&mut head, 81), 1);
        assert_eq!(pool.release_matching_key(&mut head, 81), 0);
        assert_eq!(pool.release_matching_key(&mut head, 82), 3);
        assert_eq!(head, NO_NOTE);
    }

    #[test]
    fn release_all_reports_every_note_newest_first() {
        let mut pool = NotePool::new();
        let mut head = NO_NOTE;
        pool.allocate_note(&mut head, 1, 60, 81, 9);
        pool.allocate_note(&mut head, 2, 61, 82, 9);

        assert_eq!(offs(|f| pool.release_all(&mut head, 42, f)), vec![(2, 42), (1, 42)]);
        assert_eq!(head, NO_NOTE);
    }

    #[test]
    fn released_slot_is_reused_first() {
        let mut pool = NotePool::new();
        let mut head = NO_NOTE;
        pool.allocate_note(&mut head, 1, 60, 81, 9);
        let slot = head;
        pool.release_matching_key(&mut head, 81);

        pool.allocate_note(&mut head, 2, 62, 83, 9);
        assert_eq!(head, slot);
    }
}
