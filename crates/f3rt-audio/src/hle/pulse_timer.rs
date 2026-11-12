//! `audio/hle/pulse_timer.hpp`: a rate accumulator on the 1 kHz grid.
//!
//! Every clock tick adds `step`, and each time the sum reaches `period` the timer pulses. `period` 625
//! is one musical pulse at the ROM's tempo unit, 12 is the sequence fade pulse. A step of zero never
//! pulses. The timer reports the pulse; its owner decides who receives it.

pub struct PulseTimer {
    period: u32,
    step: u32,
    accumulator: u32,
}

impl PulseTimer {
    pub fn new(period: u32, step: u32) -> PulseTimer {
        PulseTimer { period, step, accumulator: 0 }
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

    /// Advances one clock tick; true when the tick crosses a period boundary.
    pub fn tick(&mut self) -> bool {
        self.accumulator += self.step;
        if self.accumulator < self.period {
            return false;
        }
        self.accumulator -= self.period;
        true
    }
}
