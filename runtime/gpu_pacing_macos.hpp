#pragma once
#include <SDL3/SDL.h>

namespace f3rt {
// NSWindow display-link callbacks are vsync notifications, not proof that an
// SDL drawable reached the display. This helper never acquires a drawable.
void *create_macos_motion_pacing(SDL_Window *window);
void destroy_macos_motion_pacing(void *pacing);
void wait_macos_motion_pacing(void *pacing);
double macos_motion_callback_hz(void *pacing);
double macos_motion_requested_hz(void *pacing);
}
