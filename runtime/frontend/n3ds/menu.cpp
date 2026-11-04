#include "menu.hpp"
#include "log.hpp"
#include "session.hpp"
#include <3ds.h>
#include <imgui.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <utility>

namespace f3rt::n3ds {
namespace {
constexpr float TAP_MS = 300;
constexpr float TAP_DISTANCE = 10;
constexpr std::array<const char *, LOCAL_CONTROL_COUNT> CONTROL_NAMES = {
    "Up", "Down", "Left", "Right", "B1", "B2", "B3", "Start",
    "Coin", "Service", "Test", "B4", "B5", "B6"};
char hex_mask[16];
const char *mask_text(uint32_t mask) {
    std::snprintf(hex_mask, sizeof hex_mask, "0x%x", unsigned(mask));
    return hex_mask;
}
} // namespace

Menu::Menu(Shared &shared, Session &session, Settings &settings, std::string settings_path,
           PerfStats &perf)
    : m_shared(shared), m_session(session), m_settings(settings),
      m_settings_path(std::move(settings_path)), m_perf(perf) {}
bool Menu::take_restart() { return std::exchange(m_restart, false); }

void Menu::commit(const std::string &line) {
    log_line("n3ds: setting %s", line.c_str());
    save_settings(m_settings_path, m_settings);
}

void Menu::update_tap() {
    const uint32_t down = hidKeysDown(), up = hidKeysUp(), held = hidKeysHeld();
    touchPosition position{};
    hidTouchRead(&position);
    const bool hovered = ImGui::IsAnyItemHovered();
    if (down & KEY_TOUCH) {
        m_touching = true;
        m_press_hovered = hovered;
        m_touch_x = m_last_x = float(position.px);
        m_touch_y = m_last_y = float(position.py);
        m_touch_start = std::chrono::steady_clock::now();
        return;
    }
    // hidTouchRead reports (0, 0) once the pen lifts, so the contact point is tracked while held.
    if ((held & KEY_TOUCH) && m_touching) {
        m_last_x = float(position.px);
        m_last_y = float(position.py);
        return;
    }
    if (!(up & KEY_TOUCH) || !m_touching) return;
    m_touching = false;
    const float moved = std::max(std::abs(m_last_x - m_touch_x), std::abs(m_last_y - m_touch_y));
    const double ms = std::chrono::duration<double, std::milli>(
                          std::chrono::steady_clock::now() - m_touch_start)
                          .count();
    if (ms >= TAP_MS || moved >= TAP_DISTANCE) return;
    if (!m_open) {
        m_open = true;
        m_session.post(Command::Pause);
        log_line("n3ds: menu open (paused)");
    } else if (!m_press_hovered && !hovered) {
        m_open = false;
        m_session.post(Command::Resume);
        log_line("n3ds: menu closed (resumed)");
    }
}

void Menu::status_line(const Status &status) const {
    ImGui::Text("%.1f fps | comp %.1f fps %.2f ms | audio %.1f ms | drops %llu", status.emu_fps,
                status.comp_fps, status.comp_ms, status.audio_ms,
                static_cast<unsigned long long>(status.drops));
}

void Menu::game_tab() {
    if (ImGui::Button("Resume", ImVec2(-1, 44))) {
        m_open = false;
        m_session.post(Command::Resume);
        log_line("n3ds: menu closed (resumed)");
    }
    ImGui::Dummy(ImVec2(0, 8));
    if (ImGui::Button("Exit", ImVec2(-1, 44))) {
        m_session.post(Command::Stop);
        m_shared.quit.store(true);
        ImGui::GetIO().WantCaptureMouse = false;
    }
    ImGui::Dummy(ImVec2(0, 8));
    ImGui::TextWrapped("Tap empty space to resume.");
}

void Menu::audio_tab() {
    int volume = int(m_settings.volume);
    if (ImGui::SliderInt("Volume", &volume, 0, 100, "%d%%")) {
        m_settings.volume = unsigned(std::clamp(volume, 0, 100));
        m_session.post(Command::SetVolume, m_settings.volume);
        commit("volume=" + std::to_string(m_settings.volume));
    }
}

void Menu::input_tab() {
    const uint32_t down = hidKeysDown();
    if (m_rebind >= 0) {
        const uint32_t pressed = down & ~uint32_t(KEY_TOUCH);
        ImGui::Text("Press a button for %s", CONTROL_NAMES[size_t(m_rebind)]);
        if (ImGui::Button("Cancel")) m_rebind = -1;
        if (pressed) {
            m_settings.keys[size_t(m_rebind)] = pressed;
            char line[64];
            std::snprintf(line, sizeof line, "key%d=%x", m_rebind, unsigned(pressed));
            commit(line);
            m_rebind = -1;
        }
        return;
    }
    for (unsigned control = 0; control < m_settings.keys.size(); ++control) {
        ImGui::Text("%-8s %s", CONTROL_NAMES[control], mask_text(m_settings.keys[control]));
        ImGui::SameLine(200);
        ImGui::PushID(int(control));
        if (ImGui::Button("Rebind")) m_rebind = int(control);
        ImGui::PopID();
    }
}

void Menu::system_tab() {
    if (ImGui::Checkbox("Fast boot", &m_settings.fast_boot))
        commit(std::string("fast_boot=") + (m_settings.fast_boot ? "on" : "off"));
    if (ImGui::Checkbox("Performance overlay", &m_settings.perf_overlay))
        commit(std::string("perf_overlay=") + (m_settings.perf_overlay ? "on" : "off"));
    ImGui::Dummy(ImVec2(0, 8));
    int border = int(m_settings.border);
    if (ImGui::RadioButton("Pixel-exact 320x232", &border, 0) ||
        ImGui::RadioButton("Widescreen border 400x232", &border, 40)) {
        if (unsigned(border) != m_settings.border) {
            m_settings.border = unsigned(border);
            commit("border=" + std::to_string(m_settings.border));
            m_restart = true;
            log_line("n3ds: border change applies on restart");
        }
    }
}

void Menu::draw(const Status &status, const std::string &error, const std::string &audio_note) {
    update_tap();
    const ImGuiViewport *viewport = ImGui::GetMainViewport();
    if (!m_open) {
        ImGui::SetNextWindowPos(ImVec2(viewport->Pos.x, viewport->Pos.y + 240));
        ImGui::SetNextWindowSize(ImVec2(400, 240));
        ImGui::Begin("##bottom", nullptr,
                     ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                         ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                         ImGuiWindowFlags_NoBackground);
        ImGui::SetCursorPos(ImVec2(120, 100));
        ImGui::TextUnformatted("Tap to open menu");
        if (m_settings.perf_overlay) {
            ImGui::SetCursorPos(ImVec2(20, 140));
            status_line(status);
        }
        if (!error.empty()) {
            ImGui::SetCursorPos(ImVec2(20, 180));
            ImGui::TextColored(ImVec4(1, 0.4f, 0.4f, 1), "%s", error.c_str());
        }
        ImGui::End();
        return;
    }
    ImGui::SetNextWindowPos(ImVec2(40, 240));
    ImGui::SetNextWindowSize(ImVec2(320, 240));
    ImGui::Begin("##menu", nullptr,
                 ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize |
                     ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings);
    if (ImGui::BeginTabBar("##tabs")) {
        if (ImGui::BeginTabItem("Game")) {
            m_tab = 0;
            game_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Audio")) {
            m_tab = 1;
            audio_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("Input")) {
            m_tab = 2;
            input_tab();
            ImGui::EndTabItem();
        }
        if (ImGui::BeginTabItem("System")) {
            m_tab = 3;
            system_tab();
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
    ImGui::Separator();
    if (!audio_note.empty()) ImGui::TextWrapped("%s", audio_note.c_str());
    status_line(status);
    ImGui::End();
}
} // namespace f3rt::n3ds
