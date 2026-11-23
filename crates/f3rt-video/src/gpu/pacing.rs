//! Display pacing, mechanically ported from `runtime/renderer/gpu/pacing.{hpp,cpp}`.
//! The macOS `CADisplayLink` helper stays C++ (`pacing_macos.mm`) behind the driver.

use super::driver::{DriverRef, RawDevice, RawPacing};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TickWait {
    Waited,
    LinkRecoveredForNext,
    Unavailable,
}

/// One GPU frame in flight and (macOS) a display link for the window.
pub struct DisplayPacer {
    driver: DriverRef,
    enabled: bool,
    pace_prepared: bool,
    link_alive: bool,
    platform: Option<RawPacing>,
}

impl DisplayPacer {
    pub fn new(driver: DriverRef) -> Self {
        Self {
            driver,
            enabled: false,
            pace_prepared: false,
            link_alive: true,
            platform: None,
        }
    }

    /// Requires the device and whether a window exists. Idempotent.
    pub fn enable(
        &mut self,
        device: RawDevice,
        has_window: bool,
        vsync: bool,
    ) -> Result<(), String> {
        if self.enabled {
            return Ok(());
        }
        if has_window {
            self.driver
                .set_frames_in_flight(device, 1, "Set temporal GPU queue depth")?;
            if vsync {
                self.platform = self.driver.create_pacer();
            }
        }
        self.enabled = true;
        Ok(())
    }

    /// Waits for the next display tick unless one is already prepared; true when a tick is
    /// prepared. False without an active link: callers must pace on their own deadlines.
    pub fn pace(&mut self, vsync: bool) -> bool {
        if self.platform.is_some() && vsync && !self.pace_prepared {
            self.pace_prepared = self.wait_tick(vsync) == TickWait::Waited;
        }
        self.pace_prepared
    }

    /// True once if `pace()` prepared a tick since the last presenting frame; clears the flag.
    pub fn consume_prepared(&mut self) -> bool {
        std::mem::take(&mut self.pace_prepared)
    }

    pub fn reset_prepared(&mut self) {
        self.pace_prepared = false;
    }

    /// Waits for one display-link tick while the link is alive; while recovering from a timeout,
    /// polls non-blockingly and reports `LinkRecoveredForNext` so waiting resumes next call.
    pub fn wait_tick(&mut self, vsync: bool) -> TickWait {
        let Some(platform) = self.platform else {
            return TickWait::Unavailable;
        };
        if !vsync {
            return TickWait::Unavailable;
        }
        if self.link_alive {
            self.link_alive = self.driver.wait_pacer(platform);
            if self.link_alive {
                TickWait::Waited
            } else {
                TickWait::Unavailable
            }
        } else {
            self.link_alive = self.driver.poll_pacer(platform);
            if self.link_alive {
                TickWait::LinkRecoveredForNext
            } else {
                TickWait::Unavailable
            }
        }
    }

    /// SDL current display mode, not observed cadence.
    pub fn display_hz(&self) -> f64 {
        self.driver.display_hz()
    }

    /// Observed vsync callback cadence (macOS only; 0 elsewhere).
    pub fn callback_hz(&self) -> f64 {
        self.platform
            .map(|platform| self.driver.pacer_callback_hz(platform))
            .unwrap_or(0.0)
    }

    /// Best-effort display-link request (macOS only; 0 elsewhere).
    pub fn requested_hz(&self) -> f64 {
        self.platform
            .map(|platform| self.driver.pacer_requested_hz(platform))
            .unwrap_or(0.0)
    }
}

impl Drop for DisplayPacer {
    fn drop(&mut self) {
        if let Some(platform) = self.platform {
            self.driver.destroy_pacer(platform);
        }
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::gpu::mock_driver::MockGpuDriver;
    use std::rc::Rc;

    fn make_pacer() -> (Rc<MockGpuDriver>, DisplayPacer) {
        let mock = Rc::new(MockGpuDriver::default());
        let driver: DriverRef = mock.clone();
        (mock, DisplayPacer::new(driver))
    }

    #[test]
    fn enabling_requires_one_frame_in_flight() {
        let (mock, mut pacer) = make_pacer();
        pacer.enable(RawDevice::null(), true, true).expect("enable");
        assert_eq!(mock.frames_in_flight(), vec![1]);
        assert_eq!(mock.call_count("create_pacer"), 1);
        pacer
            .enable(RawDevice::null(), true, true)
            .expect("idempotent");
        assert_eq!(mock.frames_in_flight(), vec![1]);

        let (mock, mut pacer) = make_pacer();
        mock.fail_after("frames_in_flight", 0);
        assert_eq!(
            pacer.enable(RawDevice::null(), true, true).unwrap_err(),
            "Set temporal GPU queue depth: mock failure"
        );
        assert_eq!(mock.call_count("create_pacer"), 0);
    }

    #[test]
    fn tick_waits_recover_through_the_poll_path() {
        let (mock, mut pacer) = make_pacer();
        pacer.enable(RawDevice::null(), true, true).expect("enable");
        mock.set_wait_results([true]);
        assert_eq!(pacer.wait_tick(true), TickWait::Waited);
        mock.set_wait_results([false]);
        assert_eq!(pacer.wait_tick(true), TickWait::Unavailable);
        mock.set_poll_results([true]);
        assert_eq!(pacer.wait_tick(true), TickWait::LinkRecoveredForNext);
        assert_eq!(pacer.wait_tick(true), TickWait::Unavailable);
        assert_eq!(pacer.wait_tick(false), TickWait::Unavailable);
    }

    #[test]
    fn pace_prepares_one_tick_until_consumed() {
        let (mock, mut pacer) = make_pacer();
        pacer.enable(RawDevice::null(), true, true).expect("enable");
        mock.set_wait_results([true, true]);
        assert!(pacer.pace(true));
        assert!(pacer.consume_prepared());
        assert!(!pacer.consume_prepared());
        assert!(!pacer.pace(false));
        assert!(pacer.pace(true));
        pacer.reset_prepared();
        assert!(!pacer.pace(true), "sources exhausted; nothing is prepared");
        assert_eq!(pacer.display_hz(), 60.0);
    }
}
