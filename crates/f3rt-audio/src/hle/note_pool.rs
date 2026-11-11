//! `audio/hle/note_pool.hpp`: the per-track note bookkeeping pool.
//!
//! Each track owns an intrusive singly linked list of `ActiveNote` slots; the pool hands out slots from
//! a free list and returns them there. The C++ `template <typename Sink>` sink is the `NoteSink` trait.

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

/// The receiver of a note-off the pool decides on. The C++ duck-types the `Sink &` template parameter.
pub trait NoteSink {
    fn note_off(&mut self, instance: u64, tick: u64);
}

pub struct NotePool {
    notes: [ActiveNote; NOTE_CAPACITY],
    free_head: u16,
}

impl Default for NotePool {
    fn default() -> NotePool {
        NotePool::new()
    }
}

impl NotePool {
    pub fn new() -> NotePool {
        let mut pool = NotePool { notes: [ActiveNote::default(); NOTE_CAPACITY], free_head: 0 };
        pool.reset();
        pool
    }

    pub fn reset(&mut self) {
        self.free_head = 0;
        for n in 0..NOTE_CAPACITY as u16 {
            self.notes[n as usize] = ActiveNote::default();
            self.notes[n as usize].next =
                if n + 1 == NOTE_CAPACITY as u16 { NO_NOTE } else { n + 1 };
        }
    }

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

    pub fn release_all<S: NoteSink>(&mut self, track_head: &mut u16, sink: &mut S, tick: u64) {
        while *track_head != NO_NOTE {
            let n = *track_head;
            *track_head = self.notes[n as usize].next;
            self.release_single(n, sink, tick);
        }
    }

    /// Unlinks the first note of `kernel_key` and returns its instance id (0 when none). No sink: the
    /// caller releases the voice itself.
    ///
    /// The C++ walks a `uint16_t *link` that points at the head or at a note's `next` field; here
    /// `None` names the head and `Some(i)` the `next` of note `i`, with the same reads and writes.
    pub fn release_matching_key(&mut self, track_head: &mut u16, kernel_key: u8) -> u64 {
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

    pub fn step_durations<S: NoteSink>(&mut self, track_head: &mut u16, sink: &mut S, tick: u64) {
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
            if self.notes[index].remaining != 0 {
                self.notes[index].remaining -= 1;
            }
            if self.notes[index].remaining == 0 {
                let next = self.notes[index].next;
                match link {
                    None => *track_head = next,
                    Some(i) => self.notes[i].next = next,
                }
                self.release_single(n, sink, tick);
            } else {
                link = Some(index);
            }
        }
    }

    fn release_entry(&mut self, index: u16) {
        self.notes[index as usize].next = self.free_head;
        self.free_head = index;
    }

    fn release_single<S: NoteSink>(&mut self, index: u16, sink: &mut S, tick: u64) {
        sink.note_off(self.notes[index as usize].instance, tick);
        self.release_entry(index);
    }
}
