//! `audio/hle/mailbox.hpp`: the cross-service event bus.
//!
//! It carries only the events that need coordination between services or an external injection:
//! notes, releases, track parameter writes and effect changes. Clock ticks, bank writes and reset are
//! not events. A full mailbox panics: an event is never dropped.

use super::events::Event;

pub const CAPACITY: usize = 1024;

pub struct Mailbox {
    events: [Event; CAPACITY],
    head: usize,
    tail: usize,
    count: usize,
}

impl Default for Mailbox {
    fn default() -> Self {
        Self::new()
    }
}

impl Mailbox {
    pub fn new() -> Mailbox {
        Mailbox { events: [Event::default(); CAPACITY], head: 0, tail: 0, count: 0 }
    }

    pub fn clear(&mut self) {
        self.head = 0;
        self.tail = 0;
        self.count = 0;
    }

    pub fn empty(&self) -> bool {
        self.count == 0
    }

    pub fn push(&mut self, event: Event) {
        if self.count == CAPACITY {
            panic!("HLE mailbox full");
        }
        self.events[self.tail] = event;
        self.tail = (self.tail + 1) % CAPACITY;
        self.count += 1;
    }

    /// FIFO. Each event leaves the mailbox before `f` sees it, so `f` may push.
    pub fn drain<F: FnMut(Event)>(&mut self, mut f: F) {
        while self.count != 0 {
            let event = self.events[self.head];
            self.head = (self.head + 1) % CAPACITY;
            self.count -= 1;
            f(event);
        }
    }
}
