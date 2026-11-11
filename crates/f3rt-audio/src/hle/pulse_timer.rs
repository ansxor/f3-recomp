//! `audio/hle/pulse_timer.hpp`: a rate accumulator on the 1 kHz grid.
//!
//! Every clock tick adds `step`, and each time the sum reaches `period` the timer pulses. `period` 625
//! is one musical pulse at the ROM's tempo unit, 12 is the sequence fade pulse. A step of zero never
//! pulses. Listeners are raw pointers, as in the C++ (the graph is built once and lives as long as
//! the timer).

use super::clock;

pub trait Listener {
    fn pulse(&mut self, tick: u64, tag: u8);
}

pub struct PulseTimer {
    period: u32,
    step: u32,
    accumulator: u32,
    listeners: Vec<(*mut dyn Listener, u8)>,
}

impl PulseTimer {
    pub fn new(period: u32, step: u32) -> PulseTimer {
        PulseTimer { period, step, accumulator: 0, listeners: Vec::new() }
    }

    pub fn reset(&mut self) {
        self.accumulator = 0;
    }

    pub fn set_period(&mut self, period: u32) {
        self.period = period;
    }

    pub fn set_step(&mut self, step: u32) {
        self.step = step;
    }

    /// Registrations happen while the graph is built; listeners outlive the timer.
    pub fn add(&mut self, listener: *mut dyn Listener, tag: u8) {
        self.listeners.push((listener, tag));
    }
}

impl clock::Listener for PulseTimer {
    fn clock_tick(&mut self, tick: u64) {
        self.accumulator += self.step;
        if self.accumulator < self.period {
            return;
        }
        self.accumulator -= self.period;
        for (listener, tag) in &self.listeners {
            unsafe { (**listener).pulse(tick, *tag) };
        }
    }
}
