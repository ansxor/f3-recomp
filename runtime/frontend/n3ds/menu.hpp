#pragma once
#include "perf.hpp"
#include "settings.hpp"
#include <chrono>
#include <cstdint>
#include <string>

namespace f3rt::n3ds {
class Session;
struct Shared;

// Bottom-screen menu, drawn on the main thread. A tap opens it (and pauses emulation, as F1 does on
// desktop); a tap released over no item closes it and resumes. Touch drives every setting.
class Menu {
public:
    struct Status {
        double emu_fps = 0, comp_fps = 0, comp_ms = 0, audio_ms = 0;
        uint64_t drops = 0;
    };

    Menu(Shared &shared, Session &session, Settings &settings, std::string settings_path,
         PerfStats &perf);

    // Called once per vblank inside the ImGui frame, after imgui::ctru::newFrame().
    void draw(const Status &status, const std::string &error, const std::string &audio_note);
    bool open() const { return m_open; }
    bool take_restart();

private:
    void update_tap();
    void game_tab();
    void audio_tab();
    void input_tab();
    void system_tab();
    void status_line(const Status &status) const;
    void commit(const std::string &line);

    Shared &m_shared;
    Session &m_session;
    Settings &m_settings;
    std::string m_settings_path;
    PerfStats &m_perf;

    bool m_open = false, m_restart = false;
    bool m_touching = false, m_press_hovered = false;
    // Press point and the last contact point; the release frame itself reports no position.
    float m_touch_x = 0, m_touch_y = 0, m_last_x = 0, m_last_y = 0;
    std::chrono::steady_clock::time_point m_touch_start;
    int m_tab = 0;
    int m_rebind = -1;
};
} // namespace f3rt::n3ds
