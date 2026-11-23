//! macOS `CADisplayLink` pacing, ported from the former Objective-C++ helper.
//!
//! The display link is created from the window's native `NSWindow` and drives a
//! `F3MotionDisplayClock` instance that measures the vsync callback cadence. It is a vsync
//! notification only: this never acquires an SDL drawable.

use std::cell::{Cell, RefCell};

use objc2::rc::{Retained, Weak};
use objc2::runtime::AnyObject;
use objc2::{AnyThread, DefinedClass, available, define_class, msg_send, sel};
use objc2_app_kit::NSWindow;
use objc2_core_foundation::{CFRunLoop, kCFRunLoopDefaultMode};
use objc2_foundation::{NSObject, NSRunLoop, NSRunLoopCommonModes};
use objc2_quartz_core::{CADisplayLink, CAFrameRateRange};
use sdl3_sys::properties::SDL_GetPointerProperty;
use sdl3_sys::timer::SDL_GetTicksNS;
use sdl3_sys::video::{SDL_GetWindowProperties, SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, SDL_Window};

/// Bounded wait for one display-link tick, in nanoseconds.
const WAIT_BUDGET_NS: u64 = 50_000_000;

/// Reset the cadence measurement when consecutive ticks are this far apart (seconds).
const MAX_TICK_GAP: f64 = 0.1;

/// Recompute the measured cadence once this much time has accumulated (seconds).
const MEASURE_WINDOW: f64 = 0.5;

struct Ivars {
    link: RefCell<Option<Retained<CADisplayLink>>>,
    window: Weak<NSWindow>,
    ticks: Cell<u64>,
    seen_ticks: Cell<u64>,
    first_timestamp: Cell<f64>,
    last_timestamp: Cell<f64>,
    intervals: Cell<u32>,
    measured_hz: Cell<f64>,
    requested_hz: Cell<f64>,
}

define_class!(
    // SAFETY: `NSObject` has no subclassing requirements and this class implements no `Drop`.
    #[unsafe(super(NSObject))]
    #[name = "F3MotionDisplayClock"]
    #[ivars = Ivars]
    struct F3MotionDisplayClock;

    impl F3MotionDisplayClock {
        #[unsafe(method(tick:))]
        fn tick(&self, sender: &CADisplayLink) {
            let ivars = self.ivars();
            ivars.ticks.set(ivars.ticks.get() + 1);
            let timestamp = sender.timestamp();
            let last_timestamp = ivars.last_timestamp.get();
            if last_timestamp == 0.0
                || timestamp <= last_timestamp
                || timestamp - last_timestamp > MAX_TICK_GAP
            {
                ivars.first_timestamp.set(timestamp);
                ivars.intervals.set(0);
                ivars.measured_hz.set(0.0);
            } else {
                ivars.intervals.set(ivars.intervals.get() + 1);
                let elapsed = timestamp - ivars.first_timestamp.get();
                if elapsed >= MEASURE_WINDOW {
                    ivars
                        .measured_hz
                        .set(f64::from(ivars.intervals.get()) / elapsed);
                    ivars.first_timestamp.set(timestamp);
                    ivars.intervals.set(0);
                }
            }
            ivars.last_timestamp.set(timestamp);
        }
    }
);

impl F3MotionDisplayClock {
    fn new(window: Weak<NSWindow>) -> Retained<Self> {
        let this = Self::alloc().set_ivars(Ivars {
            link: RefCell::new(None),
            window,
            ticks: Cell::new(0),
            seen_ticks: Cell::new(0),
            first_timestamp: Cell::new(0.0),
            last_timestamp: Cell::new(0.0),
            intervals: Cell::new(0),
            measured_hz: Cell::new(0.0),
            requested_hz: Cell::new(0.0),
        });
        // SAFETY: `NSObject`'s `init` has no additional requirements.
        unsafe { msg_send![super(this), init] }
    }

    /// Match the display link to the current screen's maximum refresh rate.
    fn request_screen_rate(&self) {
        // SAFETY: the weak window is only used while alive; pacing runs on the main thread.
        let Some(window) = self.ivars().window.load() else {
            return;
        };
        let Some(screen) = window.screen() else {
            return;
        };
        let maximum = screen.maximumFramesPerSecond() as f64;
        let ivars = self.ivars();
        if maximum > 0.0 && maximum != ivars.requested_hz.get() {
            let rate = maximum as f32;
            if let Some(link) = ivars.link.borrow().as_ref() {
                link.setPreferredFrameRateRange(CAFrameRateRange {
                    minimum: rate,
                    maximum: rate,
                    preferred: rate,
                });
            }
            ivars.requested_hz.set(maximum);
            ivars.first_timestamp.set(0.0);
            ivars.last_timestamp.set(0.0);
            ivars.intervals.set(0);
            ivars.measured_hz.set(0.0);
        }
    }

    /// Stop callbacks and release the run loop's reference to this target.
    fn invalidate(&self) {
        if let Some(link) = self.ivars().link.borrow_mut().take() {
            link.invalidate();
        }
    }
}

/// Owned macOS display-link pacer.
pub struct MacosDisplayLink {
    clock: Retained<F3MotionDisplayClock>,
}

impl MacosDisplayLink {
    /// Creates a display link for `window`, or `None` when unsupported or no native window exists.
    pub fn new(window: *mut SDL_Window) -> Option<Self> {
        if window.is_null() || !available!(macos = 14.0) {
            return None;
        }
        // SAFETY: the SDL window stays alive for the duration of this call; SDL exposes the
        // Cocoa window as an unretained pointer.
        let native = unsafe {
            let properties = SDL_GetWindowProperties(window);
            SDL_GetPointerProperty(
                properties,
                SDL_PROP_WINDOW_COCOA_WINDOW_POINTER,
                std::ptr::null_mut(),
            )
        };
        if native.is_null() {
            return None;
        }
        // SAFETY: SDL returns a live `NSWindow` for this window.
        let native = unsafe { &*native.cast::<NSWindow>() };

        let clock = F3MotionDisplayClock::new(Weak::new(native));
        let target: &AnyObject =
            // SAFETY: every Objective-C object is layout-compatible with `AnyObject`.
            unsafe { &*Retained::as_ptr(&clock).cast::<AnyObject>() };
        // SAFETY: the clock outlives the link (both owned by `MacosDisplayLink`) and `tick:` is
        // the selector this clock implements.
        let link = unsafe { native.displayLinkWithTarget_selector(target, sel!(tick:)) };
        *clock.ivars().link.borrow_mut() = Some(link);
        clock.request_screen_rate();
        // SAFETY: the display link is live and the main run loop may be serviced on this thread.
        unsafe {
            if let Some(link) = clock.ivars().link.borrow().as_ref() {
                link.addToRunLoop_forMode(&NSRunLoop::mainRunLoop(), NSRunLoopCommonModes);
            }
        }
        Some(Self { clock })
    }

    /// Blocks (bounded to 50 ms) for the next display-link tick.
    pub fn wait(&mut self) -> bool {
        self.clock.request_screen_rate();
        let before = self.clock.ivars().ticks.get();
        // SAFETY: plain C function.
        let deadline = unsafe { SDL_GetTicksNS() } + WAIT_BUDGET_NS;
        while self.clock.ivars().ticks.get() == before {
            // SAFETY: plain C function.
            let now = unsafe { SDL_GetTicksNS() };
            if now >= deadline {
                break;
            }
            let remaining = (deadline - now) as f64 / 1.0e9;
            // SAFETY: reading the CoreFoundation default-mode constant.
            let mode = unsafe { kCFRunLoopDefaultMode };
            let _ = CFRunLoop::run_in_mode(mode, remaining, true);
        }
        let ivars = self.clock.ivars();
        ivars.seen_ticks.set(ivars.ticks.get());
        if ivars.ticks.get() == before {
            ivars.measured_hz.set(0.0);
            return false;
        }
        true
    }

    /// Services the run loop once and reports whether a tick arrived since the last wait/poll.
    pub fn poll(&mut self) -> bool {
        // SAFETY: reading the CoreFoundation default-mode constant.
        let mode = unsafe { kCFRunLoopDefaultMode };
        let _ = CFRunLoop::run_in_mode(mode, 0.0, true);
        let ivars = self.clock.ivars();
        let ticked = ivars.ticks.get() != ivars.seen_ticks.get();
        ivars.seen_ticks.set(ivars.ticks.get());
        ticked
    }

    pub fn callback_hz(&self) -> f64 {
        self.clock.ivars().measured_hz.get()
    }

    pub fn requested_hz(&self) -> f64 {
        self.clock.ivars().requested_hz.get()
    }
}

impl Drop for MacosDisplayLink {
    fn drop(&mut self) {
        self.clock.invalidate();
    }
}
