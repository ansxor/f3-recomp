#pragma once
#include <SDL3/SDL.h>

namespace f3rt {
enum class TickWait : uint8_t { Waited, LinkRecoveredForNext, Unavailable };

class DisplayPacer {
public:
    DisplayPacer() = default;
    ~DisplayPacer();
    DisplayPacer(const DisplayPacer &) = delete;
    DisplayPacer &operator=(const DisplayPacer &) = delete;

    // One GPU frame in flight and (macOS) a display link for the window. Idempotent.
    void enable(SDL_GPUDevice *device, SDL_Window *window, bool vsync);
    // Waits for the next display tick unless one is already prepared; true when a tick is prepared.
    // False without an active link: callers must pace on their own deadlines.
    bool pace(bool vsync);
    // True once if pace() prepared a tick since the last presenting frame; clears the flag.
    bool consume_prepared();
    void reset_prepared();
    // Waits for one display-link tick while the link is alive; while recovering from a timeout,
    // polls non-blockingly and reports LinkRecoveredForNext so waiting resumes on the next call.
    TickWait wait_tick(bool vsync);
    // SDL current display mode, not observed cadence.
    double display_hz(SDL_Window *window) const;
    // Observed vsync callback cadence (macOS only; 0 elsewhere).
    double callback_hz() const;
    // Best-effort display-link request (macOS only; 0 elsewhere).
    double requested_hz() const;

private:
    bool m_enabled = false;
    bool m_pace_prepared = false;
    bool m_link_alive = true;
    void *m_platform = nullptr;
};
}
