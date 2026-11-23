#include "renderer/gpu/pacing.hpp"
#include "renderer/gpu/sdl_gpu.hpp"
#include <utility>
#ifdef __APPLE__
#include "renderer/gpu/pacing_macos.hpp"
#endif

namespace f3rt {
DisplayPacer::~DisplayPacer() {
#ifdef __APPLE__
    destroy_macos_motion_pacing(m_platform);
#endif
}

void DisplayPacer::enable(SDL_GPUDevice *device, SDL_Window *window, bool vsync) {
    if (m_enabled) return;
    if (window && !SDL_SetGPUAllowedFramesInFlight(device, 1))
        gpu_fail("Set temporal GPU queue depth");
#ifdef __APPLE__
    if (window && vsync) m_platform = create_macos_motion_pacing(window);
#else
    (void)vsync;
#endif
    m_enabled = true;
}

bool DisplayPacer::pace(bool vsync) {
    if (m_platform && vsync && !m_pace_prepared)
        m_pace_prepared = wait_tick(vsync) == TickWait::Waited;
    return m_pace_prepared;
}

bool DisplayPacer::consume_prepared() {
    return std::exchange(m_pace_prepared, false);
}

void DisplayPacer::reset_prepared() { m_pace_prepared = false; }

TickWait DisplayPacer::wait_tick(bool vsync) {
#ifdef __APPLE__
    if (!m_platform || !vsync) return TickWait::Unavailable;
    if (m_link_alive) {
        m_link_alive = wait_macos_motion_pacing(m_platform);
        return m_link_alive ? TickWait::Waited : TickWait::Unavailable;
    }
    m_link_alive = poll_macos_motion_pacing(m_platform);
    return m_link_alive ? TickWait::LinkRecoveredForNext : TickWait::Unavailable;
#else
    (void)vsync;
    return TickWait::Unavailable;
#endif
}

double DisplayPacer::display_hz(SDL_Window *window) const {
    if (!window) return 0;
    const auto *mode = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(window));
    return mode ? mode->refresh_rate : 0;
}

double DisplayPacer::callback_hz() const {
#ifdef __APPLE__
    return macos_motion_callback_hz(m_platform);
#else
    return 0;
#endif
}

double DisplayPacer::requested_hz() const {
#ifdef __APPLE__
    return macos_motion_requested_hz(m_platform);
#else
    return 0;
#endif
}
}
