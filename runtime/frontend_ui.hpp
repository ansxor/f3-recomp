#pragma once
#include "frontend_settings.hpp"
#include <memory>
#include <vector>

namespace f3rt {
enum class UiActionKind { Host, Join, Disconnect, SaveState, LoadState, Screenshot, ApplySettings, ApplyPostprocess, SavePreferences };
struct UiAction { UiActionKind kind; unsigned slot = 0; };
struct FrontendUiState {
    bool connected = false, transferring = false, gpu_available = false;
    std::string status = "Solo", message;
    std::string active_postprocess = "off";
    std::string active_renderer; // this session's renderer; differs from the saved preference after a restart-only change or a developer --renderer
    double rtt_ms = 0;
    unsigned rollback_depth = 0;
    int frame_advantage = 0;
    bool desync = false;
    float transfer_progress = 0;
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
    void draw(const FrontendUiState &state); // builds frame; call even when solo is paused
    std::vector<UiAction> take_actions();
    void render_cpu(); // after game texture, before SDL_RenderPresent
    // GpuVideo overlay callback: after game blit, before command submission.
    void render_gpu(SDL_GPUCommandBuffer *, SDL_GPUTexture *, Uint32 width, Uint32 height);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
