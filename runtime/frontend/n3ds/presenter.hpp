#pragma once
#include "perf.hpp"
#include "session.hpp"
#include "settings.hpp"
#include "frontend/frame_slots.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <memory>

struct ImTextureData;

namespace f3rt::n3ds {
class Menu;

// The main-thread presenter: C3D render targets, the 512x256 game texture, the bottom-screen menu
// and the per-vblank shell. The GPU does the display transfer, the textured quad and ImGui drawing.
class Presenter {
public:
    Presenter(Shared &shared, Settings &settings, PerfStats &perf);
    ~Presenter();
    Presenter(const Presenter &) = delete;
    Presenter &operator=(const Presenter &) = delete;

    // C3D, render targets and staging. Call before imgui::citro3d::init().
    bool init();
    // ImGui-owned game texture. Call after imgui::citro3d::init().
    bool create_texture();
    void shutdown();
    // One vblank: take a new composed frame, draw the game and the menu, present both screens.
    void frame(Menu &menu, const std::string &error, const std::string &audio_note);

private:
    void report_rates();

    Shared &m_shared;
    Settings &m_settings;
    PerfStats &m_perf;
    void *m_top = nullptr;
    void *m_bottom = nullptr;
    void *m_texture = nullptr;
    void *m_texture_address = nullptr;
    std::array<void *, f3rt::FrameSlots::SLOT_COUNT> m_staging{};
    uint32_t *m_transfer_buffer = nullptr;
    std::unique_ptr<ImTextureData> m_game_texture;
    void *m_game_texture_id = nullptr;
    bool m_transfer_pending = false;
    uint64_t m_last_emulated = 0, m_last_composed = 0, m_last_presented = 0;
    double m_emu_fps = 0, m_comp_fps = 0, m_present_fps = 0;
    std::chrono::steady_clock::time_point m_report_since;
};
} // namespace f3rt::n3ds
