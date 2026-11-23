//! Display pacing. On macOS the pacer owns a `CADisplayLink` (see [`super::pacing_macos`]);
//! other platforms have no display link and callers pace on their own deadlines.

// Raw GPU handles are only ever passed back to SDL3.
#![allow(clippy::not_unsafe_ptr_arg_deref)]

use sdl3_sys::gpu::SDL_GPUDevice;
use sdl3_sys::video::{SDL_GetCurrentDisplayMode, SDL_GetDisplayForWindow, SDL_Window};

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum TickWait {
    Waited,
    LinkRecoveredForNext,
    Unavailable,
}

#[cfg(target_os = "macos")]
type PlatformPacer = super::pacing_macos::MacosDisplayLink;

#[cfg(not(target_os = "macos"))]
struct PlatformPacer;

#[cfg(not(target_os = "macos"))]
impl PlatformPacer {
    fn wait(&mut self) -> bool {
        false
    }

    fn poll(&mut self) -> bool {
        false
    }

    fn callback_hz(&self) -> f64 {
        0.0
    }

    fn requested_hz(&self) -> f64 {
        0.0
    }
}

/// One GPU frame in flight and (macOS) a display link for the window.
pub struct DisplayPacer {
    window: *mut SDL_Window,
    enabled: bool,
    pace_prepared: bool,
    link_alive: bool,
    platform: Option<PlatformPacer>,
}

impl DisplayPacer {
    pub fn new(window: *mut SDL_Window) -> Self {
        Self {
            window,
            enabled: false,
            pace_prepared: false,
            link_alive: true,
            platform: None,
        }
    }

    /// Requires the device and whether a window exists. Idempotent.
    pub fn enable(
        &mut self,
        device: *mut SDL_GPUDevice,
        has_window: bool,
        vsync: bool,
    ) -> Result<(), String> {
        if self.enabled {
            return Ok(());
        }
        if has_window {
            // SAFETY: the device is live.
            if !unsafe { sdl3_sys::gpu::SDL_SetGPUAllowedFramesInFlight(device, 1) } {
                return Err(super::sdl_gpu::failure("Set temporal GPU queue depth"));
            }
            if vsync {
                self.platform = create_pacer(self.window);
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
        let Some(platform) = self.platform.as_mut() else {
            return TickWait::Unavailable;
        };
        if !vsync {
            return TickWait::Unavailable;
        }
        if self.link_alive {
            self.link_alive = platform.wait();
            if self.link_alive {
                TickWait::Waited
            } else {
                TickWait::Unavailable
            }
        } else {
            self.link_alive = platform.poll();
            if self.link_alive {
                TickWait::LinkRecoveredForNext
            } else {
                TickWait::Unavailable
            }
        }
    }

    /// SDL current display mode, not observed cadence.
    pub fn display_hz(&self) -> f64 {
        if self.window.is_null() {
            return 0.0;
        }
        // SAFETY: the window is live.
        let display = unsafe { SDL_GetDisplayForWindow(self.window) };
        // SAFETY: `display` identifies an attached display.
        let mode = unsafe { SDL_GetCurrentDisplayMode(display) };
        if mode.is_null() {
            0.0
        } else {
            // SAFETY: SDL returned a live display mode.
            f64::from(unsafe { (*mode).refresh_rate })
        }
    }

    /// Observed vsync callback cadence (macOS only; 0 elsewhere).
    pub fn callback_hz(&self) -> f64 {
        self.platform
            .as_ref()
            .map_or(0.0, PlatformPacer::callback_hz)
    }

    /// Best-effort display-link request (macOS only; 0 elsewhere).
    pub fn requested_hz(&self) -> f64 {
        self.platform
            .as_ref()
            .map_or(0.0, PlatformPacer::requested_hz)
    }
}

#[cfg(target_os = "macos")]
fn create_pacer(window: *mut SDL_Window) -> Option<PlatformPacer> {
    super::pacing_macos::MacosDisplayLink::new(window)
}

#[cfg(not(target_os = "macos"))]
fn create_pacer(_window: *mut SDL_Window) -> Option<PlatformPacer> {
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn pacer_without_a_display_link_reports_unavailable() {
        let mut pacer = DisplayPacer::new(std::ptr::null_mut());
        pacer
            .enable(std::ptr::null_mut(), false, false)
            .expect("headless enable");
        assert_eq!(pacer.wait_tick(true), TickWait::Unavailable);
        assert!(!pacer.pace(true));
        assert_eq!(pacer.display_hz(), 0.0);
        assert_eq!(pacer.callback_hz(), 0.0);
        assert_eq!(pacer.requested_hz(), 0.0);
    }
}
