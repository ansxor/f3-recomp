#pragma once
#include <SDL3/SDL.h>

namespace f3rt {
// NSWindow display-link callbacks are vsync notifications, not proof that an
// SDL drawable reached the display. This helper never acquires a drawable.
void *create_macos_motion_pacing(SDL_Window *window);
void destroy_macos_motion_pacing(void *pacing);
// Blocks (bounded to 50 ms) for the next display-link tick. False: no tick arrived, i.e. the link is
// not running (hidden/occluded window, display asleep); callers must stop waiting and pace on a timer.
bool wait_macos_motion_pacing(void *pacing);
// Non-blocking: services the run loop once and reports whether a tick arrived since the last wait/poll.
bool poll_macos_motion_pacing(void *pacing);
double macos_motion_callback_hz(void *pacing);
double macos_motion_requested_hz(void *pacing);
}
