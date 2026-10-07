#pragma once
#include "f3rt/input.hpp"
#include <SDL3/SDL.h>
#include <array>
#include <cstdint>
#include <string>

namespace f3rt {
enum class AudioBackend { Oracle, Native, Hle };
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
    std::array<InputBinding, local_control_count> controls{};
};
struct FrontendSettings {
    std::string video_mode = "fdp", video_backend = "cpu", video_scale = "1";
    unsigned border = 0;
    std::string filter = "nearest", interpolation = "off", interpolation_fields = "geometry";
    std::string postprocess = "off", user_shader;
    AudioBackend audio_backend = AudioBackend::Oracle;
    float volume = 1.0f;
    bool fast_boot = true, boot_cache = false;
    std::string server = "127.0.0.1:9000", room;
    unsigned requested_slot = 0, delay = 2;
    std::array<InputProfile, local_player_count> profiles;
    FrontendSettings();
};
std::string default_config_path(const std::string &set);
// Load before applying explicit CLI flags. Invalid/missing files leave defaults intact.
bool load_frontend_settings(const std::string &path, FrontendSettings &settings, std::string &error);
bool save_frontend_settings(const std::string &path, const FrontendSettings &settings, std::string &error);
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
    struct Pad { SDL_JoystickID id; SDL_Gamepad *handle; std::array<bool, SDL_GAMEPAD_BUTTON_COUNT> buttons{}, blocked_buttons{}; std::array<int16_t, SDL_GAMEPAD_AXIS_COUNT> axes{}; std::array<bool, SDL_GAMEPAD_AXIS_COUNT> blocked_axes{}; };
    std::array<InputProfile, local_player_count> profiles_;
    std::array<bool, SDL_SCANCODE_COUNT> keys_{}, blocked_keys_{};
    std::array<Pad, 16> pads_{};
    unsigned pad_count_ = 0, capture_profile_ = 0, capture_control_ = 0;
    bool capture_ = false, capture_pad_ = false, captured_ = false;
    InputBinding captured_binding_{};
    void add_pad(SDL_JoystickID id);
};
}
