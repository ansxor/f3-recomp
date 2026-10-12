#include "ui.hpp"
#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#ifdef F3RT_GPU
#include "imgui_impl_sdlgpu3.h"
#endif
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <stdexcept>

namespace f3rt {
namespace {
bool combo(const char *label, std::string &value, std::initializer_list<const char *> choices) {
    bool changed = false;
    if (ImGui::BeginCombo(label, value.c_str())) {
        for (auto c : choices)
            if (ImGui::Selectable(c, value == c)) {
                value = c;
                changed = true;
            }
        ImGui::EndCombo();
    }
    return changed;
}
} // namespace
constexpr float CHAT_HOLD_SECONDS = 5.0f;
constexpr float CHAT_FADE_SECONDS = 1.5f;

namespace {
struct ChatEntry {
    uint8_t from_player = 0;
    std::string text;
};
} // namespace

struct FrontendUi::Impl {
    SDL_Window *window;
    SDL_Renderer *renderer;
    SDL_GPUDevice *gpu;
    FrontendSettings &settings;
    InputMapper &input;
    ImGuiContext *context = nullptr;
    bool opened = false, lobby_opened = false, draw_ready = false;
    unsigned save_slot = 0;
    std::vector<UiAction> actions;
    char join_code_buf[16] = {0};
    std::string join_code_str;
    bool in_session = false, chat_open = false, chat_focus_needed = false,
         chat_scroll_to_bottom = false;
    char chat_input_buf[256] = {0};
    uint64_t current_frame = 0, chat_activity_frame = 0;
    std::chrono::steady_clock::time_point chat_activity_time{};
    std::vector<ChatEntry> chat_history;
    std::vector<std::string> outgoing_chat;
    Impl(SDL_Window *w, SDL_Renderer *r, SDL_GPUDevice *g, FrontendSettings &s, InputMapper &i)
        : window(w), renderer(r), gpu(g), settings(s), input(i) {}
    void set_lobby_open(bool o) {
        if (lobby_opened == o) return;
        if (o) initialize();
        lobby_opened = o;
        draw_ready = false;
        input.release();
    }
    void activate() { ImGui::SetCurrentContext(context); }
    void initialize() {
        if (context) return;
        IMGUI_CHECKVERSION();
        context = ImGui::CreateContext();
        activate();
        ImGui::GetIO().IniFilename = nullptr;
        ImGui::StyleColorsDark();
        bool ok = false;
        // UI uses window coordinates, not the game's SDL logical presentation.
        if (renderer)
            ok = ImGui_ImplSDL3_InitForOther(window) && ImGui_ImplSDLRenderer3_Init(renderer);
#ifdef F3RT_GPU
        else if (gpu) {
            ImGui_ImplSDLGPU3_InitInfo info;
            info.Device = gpu;
            info.ColorTargetFormat = SDL_GetGPUSwapchainTextureFormat(gpu, window);
            ok = ImGui_ImplSDL3_InitForSDLGPU(window) && ImGui_ImplSDLGPU3_Init(&info);
        }
#endif
        if (!ok) throw std::runtime_error("Dear ImGui renderer initialization failed");
    }
    ~Impl() {
        if (!context) return;
        activate();
        if (renderer) ImGui_ImplSDLRenderer3_Shutdown();
#ifdef F3RT_GPU
        else if (gpu) {
            SDL_WaitForGPUIdle(gpu);
            ImGui_ImplSDLGPU3_Shutdown();
        }
#endif
        ImGui_ImplSDL3_Shutdown();
        ImGui::DestroyContext(context);
    }
    void action(UiActionKind k, unsigned slot = 0) { actions.push_back({k, slot}); }
};
FrontendUi::FrontendUi(SDL_Window *w, SDL_Renderer *r, SDL_GPUDevice *g, FrontendSettings &s,
                       InputMapper &i)
    : m_impl(std::make_unique<Impl>(w, r, g, s, i)) {}
FrontendUi::~FrontendUi() = default;
bool FrontendUi::open() const { return m_impl->opened; }
void FrontendUi::set_open(bool opened) {
    auto &p = *m_impl;
    if (p.opened == opened) return;
    if (opened) p.initialize();
    p.opened = opened;
    p.draw_ready = false;
    p.input.release();
}
bool FrontendUi::lobby_open() const { return m_impl->lobby_opened; }
void FrontendUi::set_lobby_open(bool opened) {
    auto &p = *m_impl;
    if (p.lobby_opened == opened) return;
    if (opened) {
        p.initialize();
        p.action(UiActionKind::Screenshot);
    }
    p.lobby_opened = opened;
    p.draw_ready = false;
    p.input.release();
}
const std::string &FrontendUi::join_code() const { return m_impl->join_code_str; }
bool FrontendUi::process_event(const SDL_Event &e) {
    auto &p = *m_impl;
    if (p.chat_open) {
        if (p.context) {
            p.activate();
            ImGui_ImplSDL3_ProcessEvent(&e);
        }
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
            if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
                p.chat_open = false;
                p.chat_input_buf[0] = '\0';
                p.chat_activity_time = std::chrono::steady_clock::now();
                p.chat_activity_frame = p.current_frame;
                return true;
            }
            if (e.key.scancode == SDL_SCANCODE_RETURN) {
                return true;
            }
        }
        return true;
    }
    if (p.in_session && !p.opened && !p.lobby_opened) {
        if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat &&
            e.key.scancode == SDL_SCANCODE_RETURN) {
            p.initialize();
            p.activate();
            p.chat_open = true;
            p.chat_focus_needed = true;
            p.chat_activity_time = std::chrono::steady_clock::now();
            p.chat_activity_frame = p.current_frame;
            return true;
        }
    }
    if (p.context) {
        p.activate();
        ImGui_ImplSDL3_ProcessEvent(&e);
    }
    if (e.type == SDL_EVENT_KEY_DOWN && !e.key.repeat) {
        if (e.key.scancode == SDL_SCANCODE_F1) {
            set_open(!p.opened);
            return true;
        }
        if (e.key.scancode == SDL_SCANCODE_F12) {
            p.action(UiActionKind::Screenshot);
            return true;
        }
        if (e.key.scancode == SDL_SCANCODE_ESCAPE) {
            if (p.opened && !p.input.capturing()) {
                p.opened = false;
                p.draw_ready = false;
                p.input.release();
                return true;
            }
            if (p.lobby_opened) {
                p.action(UiActionKind::NetplayCancel);
                p.lobby_opened = false;
                p.input.release();
                return true;
            }
        }
    }
    return (p.opened || p.lobby_opened) &&
           (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP ||
            e.type == SDL_EVENT_TEXT_INPUT || e.type == SDL_EVENT_MOUSE_BUTTON_DOWN ||
            e.type == SDL_EVENT_MOUSE_BUTTON_UP || e.type == SDL_EVENT_MOUSE_MOTION ||
            e.type == SDL_EVENT_MOUSE_WHEEL || e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN ||
            e.type == SDL_EVENT_GAMEPAD_BUTTON_UP || e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION);
}
void FrontendUi::open_chat() {
    auto &p = *m_impl;
    p.initialize();
    p.activate();
    p.chat_open = true;
    p.chat_focus_needed = true;
    p.chat_activity_time = std::chrono::steady_clock::now();
    p.chat_activity_frame = p.current_frame;
    p.draw_ready = true;
}
void FrontendUi::close_chat() {
    auto &p = *m_impl;
    p.chat_open = false;
    p.chat_input_buf[0] = '\0';
    p.chat_activity_time = std::chrono::steady_clock::now();
    p.chat_activity_frame = p.current_frame;
}
bool FrontendUi::chat_input_open() const { return m_impl->chat_open; }
bool FrontendUi::chat_visible() const {
    auto &p = *m_impl;
    if (p.chat_open) return true;
    if (p.chat_history.empty()) return false;
    auto now = std::chrono::steady_clock::now();
    float real_sec = std::chrono::duration<float>(now - p.chat_activity_time).count();
    float frame_sec =
        (p.chat_activity_frame > 0) ? float(p.current_frame - p.chat_activity_frame) / 60.0f : 0.0f;
    float elapsed = std::max(real_sec, frame_sec);
    return elapsed < (CHAT_HOLD_SECONDS + CHAT_FADE_SECONDS);
}
void FrontendUi::receive_chat(uint8_t from_player, std::string text) {
    auto &p = *m_impl;
    p.chat_history.push_back({from_player, std::move(text)});
    p.chat_activity_time = std::chrono::steady_clock::now();
    p.chat_activity_frame = p.current_frame;
    p.chat_scroll_to_bottom = true;
    p.draw_ready = true;
}
std::vector<std::string> FrontendUi::take_outgoing_chat() {
    std::vector<std::string> res;
    res.swap(m_impl->outgoing_chat);
    return res;
}
std::vector<UiAction> FrontendUi::take_actions() {
    std::vector<UiAction> result;
    result.swap(m_impl->actions);
    return result;
}
void FrontendUi::draw(const FrontendUiState &state) {
    auto &p = *m_impl;
    p.in_session = state.netplay_in_session;
    p.draw_ready = false;
    ++p.current_frame;
    const auto now = std::chrono::steady_clock::now();
    const float real_sec = std::chrono::duration<float>(now - p.chat_activity_time).count();
    const float frame_sec =
        (p.chat_activity_frame > 0) ? float(p.current_frame - p.chat_activity_frame) / 60.0f : 0.0f;
    const float elapsed = std::max(real_sec, frame_sec);
    const bool chat_vis = p.chat_open || (!p.chat_history.empty() &&
                                          elapsed < (CHAT_HOLD_SECONDS + CHAT_FADE_SECONDS));
    const bool should_draw = p.opened || p.lobby_opened || state.netplay_active || chat_vis;
    if (!should_draw) return;
    p.initialize();
    p.activate();
    if (p.renderer) ImGui_ImplSDLRenderer3_NewFrame();
#ifdef F3RT_GPU
    else if (p.gpu) ImGui_ImplSDLGPU3_NewFrame();
#endif
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    auto &s = p.settings;
    bool changed = false;
    unsigned profile, control;
    InputBinding binding;
    if (p.input.take_binding(profile, control, binding)) {
        s.profiles[profile].controls[control] = binding;
        p.input.configure(s);
        changed = true;
    }
    if (p.opened) {
        ImGui::SetNextWindowSize(ImVec2(650, 560), ImGuiCond_FirstUseEver);
        bool visible = p.opened;
        if (ImGui::Begin("F3 runtime - F1 menu", &visible)) {
            if (!state.message.empty()) ImGui::TextWrapped("%s", state.message.c_str());
            if (ImGui::BeginTabBar("Panels")) {
                if (ImGui::BeginTabItem("Video")) {
                    changed |= combo("Renderer (restart)", s.renderer, {"accurate", "enhanced"});
                    ImGui::TextWrapped("Active this session: %s.%s", state.active_renderer.c_str(),
                                       s.renderer == "enhanced"
                                           ? ""
                                           : " Accurate is the MAME-derived reference picture; the "
                                             "options below belong to Enhanced.");
                    if (!state.gpu_available)
                        ImGui::TextWrapped(
                            "Enhanced is unavailable in this build (no GPU support); selecting it "
                            "requires a GPU-enabled build on restart.");
                    ImGui::BeginDisabled(s.renderer == "accurate");
                    changed |=
                        combo("Scale", s.video_scale, {"1", "2", "3", "4", "auto", "auto-integer"});
                    int border = int(s.border);
                    if (ImGui::SliderInt("Widescreen border (restart)", &border, 0, 160)) {
                        s.border = border;
                        changed = true;
                    }
                    changed |= combo("Filtering", s.filter, {"nearest", "linear"});
                    changed |= combo("Interpolation", s.interpolation, {"off", "linear", "fit"});
                    changed |= combo("Interpolation fields", s.interpolation_fields,
                                     {"none", "geometry", "palette", "geometry,palette"});
                    changed |= ImGui::Checkbox("Motion interpolation", &s.motion_interp);
                    ImGui::EndDisabled();
                    ImGui::TextWrapped("Motion interpolation: experimental temporal sprite/scroll "
                                       "interpolation at display refresh; adds one native frame of "
                                       "positional latency. Requires the Enhanced renderer.");
                    ImGui::TextWrapped(
                        "Scale, filtering, interpolation and motion interpolation apply live under "
                        "Enhanced; the renderer and border apply on restart. Settings are "
                        "preferences; CLI flags override them on launch.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Shaders")) {
                    if (!p.gpu)
                        ImGui::TextUnformatted(
                            "Requires the Enhanced renderer (Video tab; restart).");
                    ImGui::BeginDisabled(!p.gpu);
                    if (combo("Post-process", s.postprocess, {"off", "crt", "user"}) &&
                        s.postprocess != "user")
                        p.action(UiActionKind::ApplyPostprocess);
                    if (s.postprocess == "user") {
                        char path[1001];
                        std::snprintf(path, sizeof path, "%s", s.user_shader.c_str());
                        if (ImGui::InputText("Shader file", path, sizeof path))
                            s.user_shader = path;
                        ImGui::TextWrapped("Metal: .metal, entry f3_postprocess. Vulkan: .spv, "
                                           "entry main. One source texture/sampler and float4 "
                                           "dimensions/scale/time uniform; see README.");
                        ImGui::BeginDisabled(s.user_shader.empty());
                        if (ImGui::Button("Load / reload shader"))
                            p.action(UiActionKind::ApplyPostprocess);
                        ImGui::EndDisabled();
                    }
                    ImGui::EndDisabled();
                    ImGui::Text("Active: %s", state.active_postprocess.c_str());
                    ImGui::TextWrapped(
                        "Default off. Post-processing affects GPU presentation/screenshots only, "
                        "never native pixels, simulation or the menu. A failed load retains the "
                        "last valid shader.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Audio")) {
                    changed |= ImGui::SliderFloat("Host output volume", &s.volume, 0, 1, "%.2f");
                    if (ImGui::BeginCombo("Sound backend (restart)",
                                          audio_backend_name(s.audio_backend))) {
                        for (auto backend : {AudioBackend::Enhanced, AudioBackend::Reference}) {
                            ImGui::BeginDisabled(backend == AudioBackend::Enhanced &&
                                                 !state.enhanced_audio_available);
                            if (ImGui::Selectable(audio_backend_name(backend),
                                                  s.audio_backend == backend)) {
                                s.audio_backend = backend;
                                changed = true;
                            }
                            ImGui::EndDisabled();
                        }
                        ImGui::EndCombo();
                    }
                    ImGui::TextWrapped(
                        "Reference audio uses cycle-accurate sound hardware emulation. Enhanced "
                        "(HLE) is approximate high-level synthesis.");
                    ImGui::TextWrapped("Volume changes host SDL output gain only. Emulated PCM and "
                                       "WAV captures remain unchanged.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Save states")) {
                    int slot = int(p.save_slot);
                    if (ImGui::SliderInt("Slot", &slot, 0, 9)) p.save_slot = slot;
                    ImGui::BeginDisabled(state.netplay_active);
                    if (ImGui::Button("Save")) p.action(UiActionKind::SaveState, p.save_slot);
                    ImGui::SameLine();
                    if (ImGui::Button("Load")) p.action(UiActionKind::LoadState, p.save_slot);
                    ImGui::EndDisabled();
                    if (state.netplay_active)
                        ImGui::TextColored(
                            ImVec4(1.0f, 0.6f, 0.2f, 1.0f),
                            "Save/load states disabled during active netplay match.");
                    if (ImGui::Button("Screenshot (F12)")) p.action(UiActionKind::Screenshot);
                    ImGui::SeparatorText("Startup");
                    changed |= ImGui::Checkbox("Fast boot (next launch)", &s.fast_boot);
                    changed |= ImGui::Checkbox("Boot state cache (next launch)", &s.boot_cache);
                    ImGui::TextWrapped(
                        "Fast boot runs the power-on frames without presentation and uses a "
                        "locally generated initialised EEPROM. The boot cache stores that "
                        "post-boot state so later launches skip it. Both are host-side only and "
                        "never change emulation. Saved with Save preferences.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Input")) {
                    const std::array<const char *, LOCAL_CONTROL_COUNT> names = {
                        "Up",    "Down", "Left",    "Right", "Button 1", "Button 2", "Button 3",
                        "Start", "Coin", "Service", "Test",  "Button 4", "Button 5", "Button 6"};
                    for (unsigned player = 0; player < s.profiles.size(); ++player) {
                        ImGui::PushID(int(player));
                        char label[48];
                        std::snprintf(label, sizeof label, "Player %u", player + 1);
                        ImGui::SeparatorText(label);
                        int slot = s.profiles[player].device_slot;
                        if (ImGui::SliderInt("Gamepad device ordinal", &slot, 0, 15)) {
                            s.profiles[player].device_slot = slot;
                            p.input.configure(s);
                            changed = true;
                        }
                        for (unsigned c = 0; c < LOCAL_CONTROL_COUNT; ++c) {
                            ImGui::PushID(int(c));
                            const auto &b = s.profiles[player].controls[c];
                            ImGui::Text("%s: %s | button %d axis %d (%+d)", names[c],
                                        b.key == SDL_SCANCODE_UNKNOWN ? "unbound"
                                                                      : SDL_GetScancodeName(b.key),
                                        b.button, b.axis, b.direction);
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Key")) p.input.start_capture(player, c, false);
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Pad")) p.input.start_capture(player, c, true);
                            ImGui::SameLine();
                            if (ImGui::SmallButton("Clear")) {
                                s.profiles[player].controls[c] = {};
                                p.input.configure(s);
                                changed = true;
                            }
                            ImGui::PopID();
                        }
                        ImGui::PopID();
                    }
                    if (p.input.capturing())
                        ImGui::TextUnformatted("Press a key or gamepad control on the selected "
                                               "device. Escape cancels.");
                    ImGui::TextWrapped(
                        "F1/F11/F12 and Alt+Enter are reserved. Device ordinal follows connected "
                        "SDL gamepads; hotplug releases all controls. No gamepad bindings are "
                        "added by default. Players 3/4 and buttons 4/5/6 are offline only; "
                        "Left/Right also drive native dial/paddle counters.");
                    ImGui::EndTabItem();
                }
                if (ImGui::BeginTabItem("Netplay")) {
                    char s_buf[256];
                    std::snprintf(s_buf, sizeof s_buf, "%s", s.netplay_server.c_str());
                    if (ImGui::InputText("Server (host:port)", s_buf, sizeof s_buf)) {
                        s.netplay_server = s_buf;
                        changed = true;
                    }
                    char n_buf[32];
                    std::snprintf(n_buf, sizeof n_buf, "%s", s.netplay_name.c_str());
                    if (ImGui::InputText("Player Name", n_buf, sizeof n_buf)) {
                        s.netplay_name = n_buf;
                        changed = true;
                    }
                    int d_val = int(s.netplay_delay);
                    if (ImGui::SliderInt("Input Delay", &d_val, 1, 8)) {
                        s.netplay_delay = uint8_t(d_val);
                        changed = true;
                    }
                    ImGui::Separator();
                    if (!state.netplay_supported) {
                        ImGui::TextWrapped(
                            "2P versus netplay trigger is not supported for this game.");
                    } else {
                        ImGui::TextWrapped("Netplay trigger is active for %s. When 2P versus mode "
                                           "begins, the netplay lobby opens automatically.",
                                           state.set.c_str());
                        if (state.netplay_active) {
                            ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f),
                                               "Match in progress! (P%u)",
                                               state.netplay_player_index + 1);
                            if (!state.netplay_room_code.empty())
                                ImGui::Text("Room: %s", state.netplay_room_code.c_str());
                            if (!state.netplay_peer_name.empty())
                                ImGui::Text("Peer: %s", state.netplay_peer_name.c_str());
                            ImGui::Text("Ping: %u ms | Stalls: %llu", state.netplay_ping,
                                        static_cast<unsigned long long>(state.netplay_stall_count));
                        } else {
                            ImGui::TextWrapped("The netplay lobby opens automatically at the "
                                               "2-player versus transition.");
                        }
                    }
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::Separator();
            if (ImGui::Button("Save preferences")) p.action(UiActionKind::SavePreferences);
            ImGui::SameLine();
            ImGui::TextUnformatted("Only this button writes the config file.");
        }
        ImGui::End();
        if (!visible) {
            p.opened = false;
            p.input.release();
        }
    }
    // Lobby panel
    if (p.lobby_opened) {
        ImGui::SetNextWindowSize(ImVec2(520, 380), ImGuiCond_FirstUseEver);
        bool lobby_vis = p.lobby_opened;
        if (ImGui::Begin("2P Versus - Netplay Lobby", &lobby_vis, ImGuiWindowFlags_NoCollapse)) {
            ImGui::TextColored(ImVec4(0.3f, 0.8f, 1.0f, 1.0f), "2-Player Versus Mode Committed!");
            ImGui::Separator();
            if (!state.netplay_has_state_sync) {
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.75f, 0.2f, 1.0f));
                ImGui::TextWrapped("Warning: state sync not yet implemented for this game -- peers "
                                   "must reach the trigger identically");
                ImGui::PopStyleColor();
                ImGui::Separator();
            }
            char s_buf[256];
            std::snprintf(s_buf, sizeof s_buf, "%s", s.netplay_server.c_str());
            if (ImGui::InputText("Server (host:port)", s_buf, sizeof s_buf)) {
                s.netplay_server = s_buf;
                changed = true;
            }
            char n_buf[32];
            std::snprintf(n_buf, sizeof n_buf, "%s", s.netplay_name.c_str());
            if (ImGui::InputText("Player Name", n_buf, sizeof n_buf)) {
                s.netplay_name = n_buf;
                changed = true;
            }
            int d_val = int(s.netplay_delay);
            if (ImGui::SliderInt("Input Delay", &d_val, 1, 8)) {
                s.netplay_delay = uint8_t(d_val);
                changed = true;
            }
            ImGui::Separator();
            if (!state.netplay_status.empty())
                ImGui::Text("Status: %s", state.netplay_status.c_str());
            if (!state.netplay_room_code.empty())
                ImGui::TextColored(ImVec4(0.2f, 1.0f, 0.2f, 1.0f), "Room Code: %s",
                                   state.netplay_room_code.c_str());
            if (!state.netplay_error.empty())
                ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1.0f), "Error: %s",
                                   state.netplay_error.c_str());
            if (!state.netplay_desync_path.empty()) {
                ImGui::TextWrapped("Diagnostics saved locally (never uploaded). Send this folder "
                                   "from both players:");
                ImGui::TextWrapped("%s", state.netplay_desync_path.c_str());
                if (ImGui::Button("Copy path"))
                    SDL_SetClipboardText(state.netplay_desync_path.c_str());
                ImGui::SameLine();
                if (ImGui::Button("Open folder"))
                    SDL_OpenURL(("file://" + state.netplay_desync_path).c_str());
            }
            ImGui::Separator();
            if (!state.netplay_active && state.netplay_status.empty()) {
                if (ImGui::Button("Host room")) p.action(UiActionKind::NetplayHost);
                ImGui::SameLine();
                ImGui::SetNextItemWidth(100);
                if (ImGui::InputText("##JoinCode", p.join_code_buf, sizeof p.join_code_buf,
                                     ImGuiInputTextFlags_CharsUppercase)) {
                    p.join_code_str = p.join_code_buf;
                }
                ImGui::SameLine();
                ImGui::BeginDisabled(p.join_code_str.empty());
                if (ImGui::Button("Join room")) p.action(UiActionKind::NetplayJoin);
                ImGui::EndDisabled();
                ImGui::SameLine();
                if (ImGui::Button("Play locally")) {
                    p.action(UiActionKind::NetplayLocal);
                    p.lobby_opened = false;
                }
            } else {
                if (ImGui::Button("Cancel Netplay")) p.action(UiActionKind::NetplayCancel);
                ImGui::SameLine();
                if (ImGui::Button("Play locally")) {
                    p.action(UiActionKind::NetplayLocal);
                    p.lobby_opened = false;
                }
            }
        }
        ImGui::End();
        if (!lobby_vis) {
            p.action(UiActionKind::NetplayCancel);
            p.lobby_opened = false;
        }
    }
    // In-match overlay (top-right corner)
    if (state.netplay_active) {
        ImGui::SetNextWindowPos(ImVec2(ImGui::GetIO().DisplaySize.x - 10.0f, 10.0f),
                                ImGuiCond_Always, ImVec2(1.0f, 0.0f));
        ImGui::SetNextWindowBgAlpha(0.65f);
        ImGuiWindowFlags overlay_flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
            ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove;
        if (ImGui::Begin("##NetplayInMatchOverlay", nullptr, overlay_flags)) {
            ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1.0f), "NETPLAY (P%u)",
                               state.netplay_player_index + 1);
            if (!state.netplay_room_code.empty())
                ImGui::Text("Room: %s", state.netplay_room_code.c_str());
            if (!state.netplay_peer_name.empty())
                ImGui::Text("Peer: %s", state.netplay_peer_name.c_str());
            ImGui::Text("Ping: %u ms | Stalls: %llu", state.netplay_ping,
                        static_cast<unsigned long long>(state.netplay_stall_count));
            ImGui::Text("Delay: %u frames", state.netplay_delay);
        }
        ImGui::End();
    }
    // In-session chat frame in bottom-left corner
    if (chat_vis) {
        ImGui::SetNextWindowPos(ImVec2(10.0f, ImGui::GetIO().DisplaySize.y - 10.0f),
                                ImGuiCond_Always, ImVec2(0.0f, 1.0f));
        if (p.chat_open) {
            ImGui::SetNextWindowSize(ImVec2(380.0f, 160.0f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.80f);
            ImGuiWindowFlags chat_flags = ImGuiWindowFlags_NoDecoration |
                                          ImGuiWindowFlags_NoSavedSettings |
                                          ImGuiWindowFlags_NoMove;
            if (ImGui::Begin("##NetplayChatActive", nullptr, chat_flags)) {
                float footer = ImGui::GetFrameHeightWithSpacing();
                if (ImGui::BeginChild("##ChatScroll", ImVec2(0, -footer), false,
                                      ImGuiWindowFlags_NoScrollbar)) {
                    for (const auto &entry : p.chat_history) {
                        std::string name =
                            (entry.from_player == state.netplay_player_index)
                                ? (state.netplay_own_name.empty() ? "You" : state.netplay_own_name)
                                : (state.netplay_peer_name.empty() ? "Peer"
                                                                   : state.netplay_peer_name);
                        ImGui::TextWrapped("%s: %s", name.c_str(), entry.text.c_str());
                    }
                    if (p.chat_scroll_to_bottom) {
                        ImGui::SetScrollHereY(1.0f);
                        p.chat_scroll_to_bottom = false;
                    }
                }
                ImGui::EndChild();
                if (p.chat_focus_needed) {
                    ImGui::SetKeyboardFocusHere();
                    p.chat_focus_needed = false;
                }
                ImGui::SetNextItemWidth(-1);
                if (ImGui::InputText("##ChatInput", p.chat_input_buf, sizeof(p.chat_input_buf),
                                     ImGuiInputTextFlags_EnterReturnsTrue)) {
                    std::string text(p.chat_input_buf);
                    while (!text.empty() && (text.front() == ' ' || text.front() == '\t'))
                        text.erase(text.begin());
                    while (!text.empty() && (text.back() == ' ' || text.back() == '\t'))
                        text.pop_back();
                    if (!text.empty()) {
                        p.outgoing_chat.push_back(text);
                        p.chat_scroll_to_bottom = true;
                    }
                    p.chat_input_buf[0] = '\0';
                    p.chat_open = false;
                    p.chat_activity_time = std::chrono::steady_clock::now();
                    p.chat_activity_frame = p.current_frame;
                }
            }
            ImGui::End();
        } else {
            float text_alpha = 1.0f;
            if (elapsed >= CHAT_HOLD_SECONDS) {
                text_alpha = 1.0f - (elapsed - CHAT_HOLD_SECONDS) / CHAT_FADE_SECONDS;
                if (text_alpha < 0.0f) text_alpha = 0.0f;
            }
            ImGui::SetNextWindowSize(ImVec2(380.0f, 0.0f), ImGuiCond_Always);
            ImGui::SetNextWindowBgAlpha(0.0f);
            ImGuiWindowFlags pass_flags =
                ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoFocusOnAppearing |
                ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoInputs;
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, text_alpha);
            if (ImGui::Begin("##NetplayChatPassive", nullptr, pass_flags)) {
                ImGui::PushTextWrapPos(360.0f);
                size_t sidx = (p.chat_history.size() > 5) ? (p.chat_history.size() - 5) : 0;
                for (size_t i = sidx; i < p.chat_history.size(); ++i) {
                    const auto &entry = p.chat_history[i];
                    std::string name =
                        (entry.from_player == state.netplay_player_index)
                            ? (state.netplay_own_name.empty() ? "You" : state.netplay_own_name)
                            : (state.netplay_peer_name.empty() ? "Peer" : state.netplay_peer_name);
                    ImGui::TextWrapped("%s: %s", name.c_str(), entry.text.c_str());
                }
                ImGui::PopTextWrapPos();
            }
            ImGui::End();
            ImGui::PopStyleVar();
        }
    }
    if (changed) p.action(UiActionKind::ApplySettings);
    ImGui::Render();
    p.draw_ready = p.opened || p.lobby_opened || state.netplay_active || chat_vis;
}
void FrontendUi::render_cpu() {
    auto &p = *m_impl;
    if (!p.draw_ready || !p.renderer) return;
    p.activate();
    SDL_Renderer *r = p.renderer;
    int w = 0, h = 0;
    SDL_RendererLogicalPresentation mode;
    SDL_GetRenderLogicalPresentation(r, &w, &h, &mode);
    SDL_Rect viewport;
    SDL_GetRenderViewport(r, &viewport);
    float sx = 1, sy = 1;
    SDL_GetRenderScale(r, &sx, &sy);
    SDL_Rect clip;
    bool clipping = SDL_RenderClipEnabled(r);
    SDL_GetRenderClipRect(r, &clip);
    SDL_SetRenderLogicalPresentation(r, 0, 0, SDL_LOGICAL_PRESENTATION_DISABLED);
    SDL_SetRenderViewport(r, nullptr);
    SDL_SetRenderScale(r, 1, 1);
    SDL_SetRenderClipRect(r, nullptr);
    ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), r);
    SDL_SetRenderLogicalPresentation(r, w, h, mode);
    SDL_SetRenderViewport(r, &viewport);
    SDL_SetRenderScale(r, sx, sy);
    SDL_SetRenderClipRect(r, clipping ? &clip : nullptr);
}
void FrontendUi::render_gpu(SDL_GPUCommandBuffer *command, SDL_GPUTexture *target, Uint32, Uint32) {
#ifdef F3RT_GPU
    auto &p = *m_impl;
    if (!p.draw_ready || !p.gpu) return;
    p.activate();
    auto *data = ImGui::GetDrawData();
    ImGui_ImplSDLGPU3_PrepareDrawData(data, command);
    SDL_GPUColorTargetInfo info{};
    info.texture = target;
    info.load_op = SDL_GPU_LOADOP_LOAD;
    info.store_op = SDL_GPU_STOREOP_STORE;
    auto *pass = SDL_BeginGPURenderPass(command, &info, 1, nullptr);
    if (!pass) throw std::runtime_error(SDL_GetError());
    ImGui_ImplSDLGPU3_RenderDrawData(data, command, pass);
    SDL_EndGPURenderPass(pass);
#else
    (void)command;
    (void)target;
#endif
}
} // namespace f3rt
