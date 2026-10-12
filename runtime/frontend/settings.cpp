#include "settings.hpp"
#include "f3rt/audio.hpp"
#include "rom_manifest.hpp"
#include <algorithm>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <vector>

namespace f3rt {
bool audio_backend_available(AudioBackend backend) {
    return backend == AudioBackend::Enhanced || backend == AudioBackend::Reference;
}
const char *audio_backend_name(AudioBackend backend) {
    switch (backend) {
    case AudioBackend::Enhanced:
        return "enhanced";
    case AudioBackend::Reference:
        return "reference";
    }
    return "unknown";
}
FrontendSettings::FrontendSettings() {
    const std::array<SDL_Scancode, LOCAL_CONTROL_COUNT> p1 = {
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT, SDL_SCANCODE_Z,
        SDL_SCANCODE_X,  SDL_SCANCODE_C,    SDL_SCANCODE_1,    SDL_SCANCODE_5,     SDL_SCANCODE_F3,
        SDL_SCANCODE_F2, SDL_SCANCODE_A,    SDL_SCANCODE_S,    SDL_SCANCODE_D};
    for (unsigned i = 0; i < p1.size(); ++i) profiles[0].controls[i].key = p1[i];
    const std::array<SDL_Scancode, LOCAL_PLAYER_COUNT> starts = {SDL_SCANCODE_1, SDL_SCANCODE_2,
                                                                 SDL_SCANCODE_3, SDL_SCANCODE_4};
    const std::array<SDL_Scancode, LOCAL_PLAYER_COUNT> coins = {SDL_SCANCODE_5, SDL_SCANCODE_6,
                                                                SDL_SCANCODE_7, SDL_SCANCODE_8};
    for (unsigned p = 0; p < profiles.size(); ++p) {
        profiles[p].device_slot = int(p);
        profiles[p].controls[7].key = starts[p];
        profiles[p].controls[8].key = coins[p];
    }
}
std::string default_config_path(const std::string &set) {
    std::string_view pref = set.empty() ? std::string_view("f3rt") : std::string_view(set);
    for (const auto &candidate : rom_manifest::games) {
        if (candidate.id == set) {
            pref = candidate.pref_dir;
            break;
        }
    }
    char *p = SDL_GetPrefPath("f3-recomp", std::string(pref).c_str());
    if (!p) return {};
    std::string result = std::string(p) + "settings.cfg";
    SDL_free(p);
    return result;
}
namespace {
bool reserved(SDL_Scancode k) {
    return k == SDL_SCANCODE_F1 || k == SDL_SCANCODE_F11 || k == SDL_SCANCODE_F12;
}
bool choice(const std::string &s, std::initializer_list<const char *> values) {
    for (auto v : values)
        if (s == v) return true;
    return false;
}
bool valid(const FrontendSettings &s) {
    if (!audio_backend_available(s.audio_backend)) return false;
    if (!choice(s.renderer, {"accurate", "enhanced"}) ||
        !choice(s.video_scale, {"1", "2", "3", "4", "auto", "auto-integer"}) || s.border > 160 ||
        !choice(s.filter, {"nearest", "linear"}) ||
        !choice(s.interpolation, {"off", "linear", "fit"}) ||
        !choice(s.interpolation_fields, {"none", "geometry", "palette", "geometry,palette"}) ||
        !std::isfinite(s.volume) || s.volume < 0 || s.volume > 1)
        return false;
    if (s.netplay_server.empty() || s.netplay_server.size() > 256 || s.netplay_name.empty() ||
        s.netplay_name.size() > 24 || s.netplay_delay < 1 || s.netplay_delay > 8)
        return false;
    if (!choice(s.postprocess, {"off", "crt", "user"}) || s.user_shader.size() > 1000 ||
        s.user_shader.find_first_of("\r\n") != std::string::npos)
        return false;
    for (const auto &p : s.profiles) {
        if (p.device_slot < 0 || p.device_slot > 15) return false;
        for (const auto &b : p.controls)
            if (b.key < 0 || b.key >= SDL_SCANCODE_COUNT || reserved(b.key) || b.button < -1 ||
                b.button >= SDL_GAMEPAD_BUTTON_COUNT || b.axis < -1 ||
                b.axis >= SDL_GAMEPAD_AXIS_COUNT || (b.direction != 1 && b.direction != -1))
                return false;
    }
    return true;
}
bool integer(const std::string &s, int &v) {
    auto r = std::from_chars(s.data(), s.data() + s.size(), v);
    return r.ec == std::errc{} && r.ptr == s.data() + s.size();
}
} // namespace
bool load_frontend_settings(const std::string &path, FrontendSettings &settings,
                            std::string &error) {
    error.clear();
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) error = ec.message();
        return !ec;
    }
    if (std::filesystem::file_size(path, ec) > 32768 || ec) {
        error = "Config exceeds 32 KiB or is unreadable";
        return false;
    }
    std::ifstream file(path);
    if (!file) {
        error = "Cannot read config";
        return false;
    }
    auto s = settings;
    std::string line;
    unsigned count = 0;
    while (std::getline(file, line)) {
        if (++count > 512 || line.size() > 1024) {
            error = "Config limits exceeded";
            return false;
        }
        if (line.empty() || line[0] == '#') continue;
        auto split = line.find('=');
        if (split == std::string::npos) {
            error = "Invalid config line";
            return false;
        }
        const auto k = line.substr(0, split), v = line.substr(split + 1);
        int n = 0;
        if (k == "renderer") s.renderer = v;
        else if (k == "scale") s.video_scale = v;
        else if (k == "filter") s.filter = v;
        else if (k == "interpolation") s.interpolation = v;
        else if (k == "fields") s.interpolation_fields = v;
        else if (k == "postprocess") s.postprocess = v;
        else if (k == "user_shader") s.user_shader = v;
        else if (k == "sprite_behaviour" || k == "video" || k == "backend") {
        } // retired options (video/backend: replaced by renderer): ignored, old files still carry the keys
        else if (k == "audio") {
            if (v == "enhanced") s.audio_backend = AudioBackend::Enhanced;
            else if (v == "reference") s.audio_backend = AudioBackend::Reference;
            else {
                error = "Unknown audio backend";
                return false;
            }
        } else if (k == "volume") {
            std::istringstream in(v);
            in >> s.volume;
            if (!in || in.peek() != EOF) {
                error = "Invalid volume";
                return false;
            }
        } else if (k == "motion_interp") {
            if (v != "on" && v != "off") {
                error = "Invalid motion interpolation setting";
                return false;
            }
            s.motion_interp = v == "on";
        } else if (k == "fast_boot" || k == "boot_cache") {
            if (v != "on" && v != "off") {
                error = "Invalid fast boot setting";
                return false;
            }
            (k == "fast_boot" ? s.fast_boot : s.boot_cache) = v == "on";
        } else if (k == "netplay_server") s.netplay_server = v;
        else if (k == "netplay_name") s.netplay_name = v;
        else if (k == "netplay_delay") {
            if (!integer(v, n) || n < 1 || n > 8) {
                error = "Invalid netplay delay";
                return false;
            }
            s.netplay_delay = uint8_t(n);
        } else if (k == "border") {
            if (!integer(v, n) || n < 0) {
                error = "Invalid numeric setting";
                return false;
            }
            s.border = n;
        } else if (k.size() == 7 && k.substr(0, 6) == "device" && k[6] >= '0' &&
                   unsigned(k[6] - '0') < s.profiles.size()) {
            if (!integer(v, n)) {
                error = "Invalid device slot";
                return false;
            }
            s.profiles[k[6] - '0'].device_slot = n;
        } else if (k.starts_with("binding")) {
            unsigned p = 0, c = 0;
            char dot = 0;
            std::istringstream name(k.substr(7));
            name >> p >> dot >> c;
            InputBinding b;
            int key;
            std::istringstream in(v);
            in >> key >> b.button >> b.axis >> b.direction;
            if (!name || dot != '.' || name.peek() != EOF || p >= s.profiles.size() ||
                c >= LOCAL_CONTROL_COUNT || !in || in.peek() != EOF) {
                error = "Invalid binding";
                return false;
            }
            b.key = SDL_Scancode(key);
            s.profiles[p].controls[c] = b;
        } else {
            error = "Unknown config setting: " + k;
            return false;
        }
    }
    if (file.bad() || !valid(s)) {
        error = "Invalid config values";
        return false;
    }
    settings = std::move(s);
    return true;
}
bool save_frontend_settings(const std::string &path, const FrontendSettings &s,
                            std::string &error) {
    error.clear();
    if (path.empty() || !valid(s)) {
        error = "Invalid config path or values";
        return false;
    }
    std::error_code ec;
    auto target = std::filesystem::path(path);
    if (!target.parent_path().empty())
        std::filesystem::create_directories(target.parent_path(), ec);
    if (ec) {
        error = ec.message();
        return false;
    }
    // Same-directory temporary file keeps replacement atomic.
    std::string tmp = path + ".tmp." + std::to_string(SDL_GetTicksNS());
    std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
    if (!out) {
        error = "Cannot create config temporary file";
        return false;
    }
    out << "# f3rt settings, Dear ImGui frontend\nrenderer=" << s.renderer
        << "\nscale=" << s.video_scale << "\nborder=" << s.border << "\nfilter=" << s.filter
        << "\ninterpolation=" << s.interpolation << "\nfields=" << s.interpolation_fields
        << "\naudio=" << audio_backend_name(s.audio_backend) << "\nvolume=" << s.volume << '\n';
    out << "postprocess=" << s.postprocess << "\nuser_shader=" << s.user_shader << '\n';
    out << "motion_interp=" << (s.motion_interp ? "on" : "off") << '\n';
    out << "fast_boot=" << (s.fast_boot ? "on" : "off")
        << "\nboot_cache=" << (s.boot_cache ? "on" : "off") << '\n';
    out << "netplay_server=" << s.netplay_server << "\nnetplay_name=" << s.netplay_name
        << "\nnetplay_delay=" << int(s.netplay_delay) << '\n';
    for (unsigned p = 0; p < s.profiles.size(); ++p) {
        out << "device" << p << '=' << s.profiles[p].device_slot << '\n';
        for (unsigned c = 0; c < LOCAL_CONTROL_COUNT; ++c) {
            const auto &b = s.profiles[p].controls[c];
            out << "binding" << p << '.' << c << '=' << int(b.key) << ' ' << b.button << ' '
                << b.axis << ' ' << b.direction << '\n';
        }
    }
    out.close();
    if (!out) {
        error = "Cannot write config";
        std::filesystem::remove(tmp, ec);
        return false;
    }
    std::filesystem::rename(tmp, target, ec);
    if (ec) {
        error = ec.message();
        std::filesystem::remove(tmp, ec);
        return false;
    }
    return true;
}
InputMapper::InputMapper(const FrontendSettings &s) : m_profiles(s.profiles) {
    SDL_InitSubSystem(SDL_INIT_GAMEPAD);
    int count = 0;
    auto *ids = SDL_GetGamepads(&count);
    for (int i = 0; i < count; ++i) add_pad(ids[i]);
    SDL_free(ids);
}
InputMapper::~InputMapper() {
    for (unsigned i = 0; i < m_pad_count; ++i) SDL_CloseGamepad(m_pads[i].handle);
    SDL_QuitSubSystem(SDL_INIT_GAMEPAD);
}
void InputMapper::add_pad(SDL_JoystickID id) {
    for (unsigned i = 0; i < m_pad_count; ++i)
        if (m_pads[i].id == id) return;
    if (m_pad_count == m_pads.size()) return;
    auto *pad = SDL_OpenGamepad(id);
    if (pad) m_pads[m_pad_count++] = {id, pad};
}
void InputMapper::configure(const FrontendSettings &s) {
    release();
    m_profiles = s.profiles;
}
void InputMapper::release() {
    m_capture = false;
    for (unsigned i = 0; i < m_keys.size(); ++i) m_blocked_keys[i] = m_blocked_keys[i] || m_keys[i];
    for (unsigned i = 0; i < m_pad_count; ++i) {
        auto &p = m_pads[i];
        for (unsigned j = 0; j < p.buttons.size(); ++j)
            p.blocked_buttons[j] = p.blocked_buttons[j] || p.buttons[j];
        for (unsigned j = 0; j < p.axes.size(); ++j)
            p.blocked_axes[j] = p.blocked_axes[j] || std::abs(int(p.axes[j])) > 8000;
    }
}
void InputMapper::start_capture(unsigned p, unsigned c, bool pad) {
    if (p >= m_profiles.size() || c >= LOCAL_CONTROL_COUNT) return;
    release();
    m_capture_profile = p;
    m_capture_control = c;
    m_capture_pad = pad;
    m_capture = true;
    m_captured = false;
}
bool InputMapper::capturing() const { return m_capture; }
bool InputMapper::take_binding(unsigned &p, unsigned &c, InputBinding &b) {
    if (!m_captured) return false;
    p = m_capture_profile;
    c = m_capture_control;
    b = m_captured_binding;
    m_captured = false;
    return true;
}
void InputMapper::process_event(const SDL_Event &e, bool suppress) {
    auto capture = [&](InputBinding b) {
        m_captured_binding = b;
        m_capture = false;
        m_captured = true;
        release();
    };
    if (e.type == SDL_EVENT_WINDOW_FOCUS_LOST) release();
    if (e.type == SDL_EVENT_GAMEPAD_ADDED) add_pad(e.gdevice.which);
    if (e.type == SDL_EVENT_GAMEPAD_REMOVED) {
        for (unsigned i = 0; i < m_pad_count; ++i)
            if (m_pads[i].id == e.gdevice.which) {
                release();
                SDL_CloseGamepad(m_pads[i].handle);
                for (unsigned j = i + 1; j < m_pad_count; ++j) m_pads[j - 1] = m_pads[j];
                m_pads[--m_pad_count] = {};
                break;
            }
    }
    if (e.type == SDL_EVENT_KEY_DOWN || e.type == SDL_EVENT_KEY_UP) {
        auto k = e.key.scancode;
        if (k <= 0 || k >= SDL_SCANCODE_COUNT) return;
        bool down = e.type == SDL_EVENT_KEY_DOWN;
        m_keys[k] = down;
        if (!down) m_blocked_keys[k] = false;
        else if (suppress) m_blocked_keys[k] = true;
        if (m_capture && down && k == SDL_SCANCODE_ESCAPE) m_capture = false;
        else if (m_capture && !m_capture_pad && down && !e.key.repeat && !reserved(k) &&
                 !(k == SDL_SCANCODE_RETURN && (e.key.mod & SDL_KMOD_ALT))) {
            InputBinding b = m_profiles[m_capture_profile].controls[m_capture_control];
            b.key = k;
            capture(b);
        }
    }
    if (e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN || e.type == SDL_EVENT_GAMEPAD_BUTTON_UP) {
        for (unsigned i = 0; i < m_pad_count; ++i)
            if (m_pads[i].id == e.gbutton.which && e.gbutton.button < SDL_GAMEPAD_BUTTON_COUNT) {
                auto &p = m_pads[i];
                auto b = e.gbutton.button;
                bool down = e.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
                p.buttons[b] = down;
                if (!down) p.blocked_buttons[b] = false;
                else if (suppress) p.blocked_buttons[b] = true;
                if (m_capture && m_capture_pad && down &&
                    int(i) == m_profiles[m_capture_profile].device_slot) {
                    auto binding = m_profiles[m_capture_profile].controls[m_capture_control];
                    binding.button = b;
                    binding.axis = -1;
                    capture(binding);
                }
            }
    }
    if (e.type == SDL_EVENT_GAMEPAD_AXIS_MOTION) {
        for (unsigned i = 0; i < m_pad_count; ++i)
            if (m_pads[i].id == e.gaxis.which && e.gaxis.axis < SDL_GAMEPAD_AXIS_COUNT) {
                auto &p = m_pads[i];
                auto a = e.gaxis.axis;
                p.axes[a] = e.gaxis.value;
                if (std::abs(int(e.gaxis.value)) < 8000) p.blocked_axes[a] = false;
                else if (suppress) p.blocked_axes[a] = true;
                if (m_capture && m_capture_pad && std::abs(int(e.gaxis.value)) > 20000 &&
                    int(i) == m_profiles[m_capture_profile].device_slot) {
                    auto b = m_profiles[m_capture_profile].controls[m_capture_control];
                    b.button = -1;
                    b.axis = a;
                    b.direction = e.gaxis.value > 0 ? 1 : -1;
                    capture(b);
                }
            }
    }
}
LocalInputWord InputMapper::word(unsigned profile) const {
    if (profile >= m_profiles.size() || m_capture) return 0;
    LocalInputWord word = 0;
    const auto &p = m_profiles[profile];
    if (m_keys[SDL_SCANCODE_RETURN] && (m_keys[SDL_SCANCODE_LALT] || m_keys[SDL_SCANCODE_RALT]))
        return 0;
    for (unsigned i = 0; i < LOCAL_CONTROL_COUNT; ++i) {
        const auto &b = p.controls[i];
        bool held = b.key != SDL_SCANCODE_UNKNOWN && m_keys[b.key] && !m_blocked_keys[b.key];
        if (b.key == SDL_SCANCODE_RETURN &&
            (m_keys[SDL_SCANCODE_LALT] || m_keys[SDL_SCANCODE_RALT]))
            held = false;
        if (p.device_slot < int(m_pad_count)) {
            const auto &pad = m_pads[p.device_slot];
            if (b.button >= 0) held |= pad.buttons[b.button] && !pad.blocked_buttons[b.button];
            if (b.axis >= 0)
                held |= !pad.blocked_axes[b.axis] && int(pad.axes[b.axis]) * b.direction > 16000;
        }
        if (held) word |= LocalInputWord(1u << i);
    }
    return word;
}
} // namespace f3rt
