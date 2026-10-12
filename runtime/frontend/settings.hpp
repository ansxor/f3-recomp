#pragma once
#include "f3rt/input.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace f3rt {
enum class AudioBackend { Enhanced, Reference };
bool audio_backend_available(AudioBackend backend);
const char *audio_backend_name(AudioBackend backend);
struct InputBinding {
    SDL_Scancode key = SDL_SCANCODE_UNKNOWN;
    int button = -1;
    int axis = -1;
    int direction = 1;
};
struct InputProfile {
    int device_slot = 0; // ordinal among connected SDL gamepads, not transient instance id
    std::array<InputBinding, LOCAL_CONTROL_COUNT> controls{};
};
struct FrontendSettings {
    std::string renderer = "accurate", video_scale = "1"; // renderer: accurate | enhanced
    unsigned border = 0;
    std::string filter = "nearest", interpolation = "off", interpolation_fields = "geometry";
    std::string postprocess = "off", user_shader;
    AudioBackend audio_backend = AudioBackend::Enhanced;
    float volume = 1.0f;
    bool fast_boot = true, boot_cache = false;
    bool motion_interp = false;
    std::array<InputProfile, LOCAL_PLAYER_COUNT> profiles;
    std::string netplay_server = "127.0.0.1:7845";
    std::string netplay_name = "Player";
    uint8_t netplay_delay = 2;
    FrontendSettings();
};
std::string default_config_path(const std::string &set);
// Load before applying explicit CLI flags. Invalid/missing files leave defaults intact.
bool load_frontend_settings(const std::string &path, FrontendSettings &settings,
                            std::string &error);
bool save_frontend_settings(const std::string &path, const FrontendSettings &settings,
                            std::string &error);
class InputMapper {
public:
    explicit InputMapper(const FrontendSettings &settings);
    ~InputMapper();
    void configure(const FrontendSettings &settings);
    void process_event(const SDL_Event &event, bool suppress);
    void release(); // cancels capture; blocks held controls until physical release
    LocalInputWord word(unsigned profile) const;
    void start_capture(unsigned profile, unsigned control, bool gamepad);
    bool capturing() const;
    bool take_binding(unsigned &profile, unsigned &control, InputBinding &binding);

private:
    struct Pad {
        SDL_JoystickID id;
        SDL_Gamepad *handle;
        std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{}, blocked_buttons{};
        std::array<int16_t, SDL_GAMEPAD_AXIS_COUNT> axes{};
        std::array<bool, SDL_GAMEPAD_AXIS_COUNT> blocked_axes{};
    };
    std::array<InputProfile, LOCAL_PLAYER_COUNT> m_profiles;
    std::array<bool, SDL_SCANCODE_COUNT> m_keys{}, m_blocked_keys{};
    std::array<Pad, 16> m_pads{};
    unsigned m_pad_count = 0, m_capture_profile = 0, m_capture_control = 0;
    bool m_capture = false, m_capture_pad = false, m_captured = false;
    InputBinding m_captured_binding{};
    void add_pad(SDL_JoystickID id);
};
} // namespace f3rt
