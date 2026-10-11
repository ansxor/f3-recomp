#pragma once
#include "settings.hpp"
#include <memory>
#include <vector>

namespace f3rt {
enum class UiActionKind {
    SaveState, LoadState, Screenshot, ApplySettings, ApplyPostprocess, SavePreferences,
    NetplayHost, NetplayJoin, NetplayLocal, NetplayCancel
};
struct UiAction { UiActionKind kind; unsigned slot = 0; };
struct FrontendUiState {
    bool gpu_available = false;
    bool enhanced_audio_available = true;
    std::string status = "Solo", message;
    std::string active_postprocess = "off";
    std::string active_renderer; // this session's renderer; differs from the saved preference after a restart-only change or a developer --renderer
    std::string set;

    // Netplay
    bool netplay_supported = false;
    bool netplay_has_state_sync = false;
    bool netplay_in_session = false;
    bool netplay_lobby_open = false;
    bool netplay_active = false;
    std::string netplay_room_code;
    std::string netplay_peer_name;
    std::string netplay_status;
    std::string netplay_own_name;
    std::string netplay_error;
    std::string netplay_desync_path; // local-only diagnostic bundle folder written after a DESYNC
    uint32_t netplay_ping = 0;
    uint64_t netplay_stall_count = 0;
    uint8_t netplay_delay = 0;
    uint8_t netplay_player_index = 0;
};
class FrontendUi {
public:
    // Exactly one renderer/device, context created lazily on first F1.
    FrontendUi(SDL_Window *window, SDL_Renderer *renderer, SDL_GPUDevice *gpu,
               FrontendSettings &settings, InputMapper &input);
    ~FrontendUi();
    bool process_event(const SDL_Event &event); // true consumes event; F1/Escape/F12 handled here
    bool open() const;
    void set_open(bool opened); // releases input on either visibility transition
    bool lobby_open() const;
    void set_lobby_open(bool open);
    const std::string &join_code() const;
    void draw(const FrontendUiState &state); // builds frame; call even when solo is paused
    std::vector<UiAction> take_actions();
    void open_chat();
    void close_chat();
    bool chat_input_open() const;
    bool chat_visible() const;
    void receive_chat(uint8_t from_player, std::string text);
    std::vector<std::string> take_outgoing_chat();
    void render_cpu(); // after game texture, before SDL_RenderPresent
    // GpuVideo overlay callback: after game blit, before command submission.
    void render_gpu(SDL_GPUCommandBuffer *, SDL_GPUTexture *, Uint32 width, Uint32 height);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
