#include "renderer/gpu/pacing_macos.hpp"
#import <AppKit/AppKit.h>
#import <QuartzCore/CADisplayLink.h>

API_AVAILABLE(macos(14.0))
@interface F3MotionDisplayClock : NSObject
@property(nonatomic, strong) CADisplayLink *link;
@property(nonatomic, weak) NSWindow *window;
@property(nonatomic) uint64_t ticks;
@property(nonatomic) uint64_t seenTicks;
@property(nonatomic) double firstTimestamp;
@property(nonatomic) double lastTimestamp;
@property(nonatomic) unsigned intervals;
@property(nonatomic) double measuredHz;
@property(nonatomic) double requestedHz;
- (void)tick:(CADisplayLink *)sender;
- (void)requestScreenRate;
@end

@implementation F3MotionDisplayClock
- (void)requestScreenRate {
    const double maximum = self.window.screen.maximumFramesPerSecond;
    if (maximum > 0 && maximum != self.requestedHz) {
        self.link.preferredFrameRateRange = CAFrameRateRangeMake(maximum, maximum, maximum);
        self.requestedHz = maximum;
        self.firstTimestamp = 0;
        self.lastTimestamp = 0;
        self.intervals = 0;
        self.measuredHz = 0;
    }
}
- (void)tick:(CADisplayLink *)sender {
    ++self.ticks;
    const double timestamp = sender.timestamp;
    if (self.lastTimestamp == 0 || timestamp <= self.lastTimestamp || timestamp - self.lastTimestamp > 0.1) {
        self.firstTimestamp = timestamp;
        self.intervals = 0;
        self.measuredHz = 0;
    } else {
        ++self.intervals;
        const double elapsed = timestamp - self.firstTimestamp;
        if (elapsed >= 0.5) {
            self.measuredHz = self.intervals / elapsed;
            self.firstTimestamp = timestamp;
            self.intervals = 0;
        }
    }
    self.lastTimestamp = timestamp;
}
@end

namespace f3rt {
void *create_macos_motion_pacing(SDL_Window *window) {
    if (@available(macOS 14.0, *)) {
        NSWindow *native = (__bridge NSWindow *)SDL_GetPointerProperty(
            SDL_GetWindowProperties(window), SDL_PROP_WINDOW_COCOA_WINDOW_POINTER, nullptr);
        if (!native) return nullptr;
        F3MotionDisplayClock *clock = [[F3MotionDisplayClock alloc] init];
        clock.window = native;
        clock.link = [native displayLinkWithTarget:clock selector:@selector(tick:)];
        [clock requestScreenRate];
        [clock.link addToRunLoop:NSRunLoop.mainRunLoop forMode:NSRunLoopCommonModes];
        return (__bridge_retained void *)clock;
    }
    return nullptr;
}
void destroy_macos_motion_pacing(void *pacing) {
    if (@available(macOS 14.0, *)) {
        if (!pacing) return;
        F3MotionDisplayClock *clock = (__bridge_transfer F3MotionDisplayClock *)pacing;
        [clock.link invalidate];
        clock.link = nil;
    }
}
bool wait_macos_motion_pacing(void *pacing) {
    if (@available(macOS 14.0, *)) {
        if (!pacing) return false;
        F3MotionDisplayClock *clock = (__bridge F3MotionDisplayClock *)pacing;
        [clock requestScreenRate];
        const uint64_t before = clock.ticks;
        const uint64_t deadline = SDL_GetTicksNS() + 50'000'000;
        // SDL's Metal queue fence only measures GPU completion. Run the native
        // display link instead; bound the wait because hidden windows stop it.
        while (clock.ticks == before) {
            const uint64_t now = SDL_GetTicksNS();
            if (now >= deadline) break;
            const double remaining = double(deadline - now) / 1.0e9;
            CFRunLoopRunInMode(kCFRunLoopDefaultMode, remaining, true);
        }
        clock.seenTicks = clock.ticks;
        if (clock.ticks == before) { clock.measuredHz = 0; return false; }
        return true;
    }
    return false;
}
bool poll_macos_motion_pacing(void *pacing) {
    if (@available(macOS 14.0, *)) {
        if (!pacing) return false;
        F3MotionDisplayClock *clock = (__bridge F3MotionDisplayClock *)pacing;
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true);
        const bool ticked = clock.ticks != clock.seenTicks;
        clock.seenTicks = clock.ticks;
        return ticked;
    }
    return false;
}
double macos_motion_callback_hz(void *pacing) {
    if (@available(macOS 14.0, *)) {
        if (pacing) return ((__bridge F3MotionDisplayClock *)pacing).measuredHz;
    }
    return 0;
}
double macos_motion_requested_hz(void *pacing) {
    if (@available(macOS 14.0, *)) {
        if (pacing) return ((__bridge F3MotionDisplayClock *)pacing).requestedHz;
    }
    return 0;
}
}
