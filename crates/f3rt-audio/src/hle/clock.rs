//! `audio/hle/clock.hpp`: the shared 1 kHz service grid.
//!
//! Every listener sees the same TIMER_TICKS boundaries, so the sequence tempo pulses, the sequence
//! fade pulse and the voice ring's service tick all sit on one grid. Listeners outlive the Clock and
//! are never removed; the graph is built once, at construction. Registration stores the same raw
//! listener pointers the C++ stores, so the services can be wired to each other as fields of one
//! owner (Core::Impl).

use super::events::TIMER_TICKS;

pub trait Listener {
    fn clock_tick(&mut self, tick: u64);
}

pub struct Clock {
    listeners: Vec<*mut dyn Listener>,
    next: u64,
}

impl Clock {
    /// The first delivered tick is the first grid boundary after `origin`.
    pub fn new(origin: u64) -> Clock {
        Clock { listeners: Vec::new(), next: (origin / TIMER_TICKS as u64 + 1) * TIMER_TICKS as u64 }
    }

    /// Re-anchors the grid: the next delivered tick is the first boundary after `tick`.
    pub fn reset(&mut self, tick: u64) {
        self.next = (tick / TIMER_TICKS as u64 + 1) * TIMER_TICKS as u64;
    }

    pub fn next_tick(&self) -> u64 {
        self.next
    }

    /// Registrations happen while the graph is built; listeners outlive the clock.
    pub fn add(&mut self, listener: *mut dyn Listener) {
        self.listeners.push(listener);
    }

    /// Notifies every listener with the current boundary, then moves to the next one.
    pub fn advance_one(&mut self) {
        let tick = self.next;
        for listener in &self.listeners {
            unsafe { (**listener).clock_tick(tick) };
        }
        self.next += TIMER_TICKS as u64;
    }
}
