#include "frontend/settings.hpp"
#include <gtest/gtest.h>
#include <rapidcheck/gtest.h>
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <vector>

namespace {

static std::atomic<uint64_t> g_temp_counter{0};

class FrontendEnvironment : public ::testing::Environment {
public:
    void SetUp() override {
        if (!SDL_WasInit(SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
            ASSERT_TRUE(SDL_Init(SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) << SDL_GetError();
        }
    }
    void TearDown() override {
        if (SDL_WasInit(SDL_INIT_EVENTS | SDL_INIT_GAMEPAD)) {
            SDL_Quit();
        }
    }
};

[[maybe_unused]] static auto *const g_frontend_env =
    ::testing::AddGlobalTestEnvironment(new FrontendEnvironment());

void send_key(f3rt::InputMapper &input, SDL_Scancode scan, bool down, bool suppressed = false) {
    SDL_Event event{};
    event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    event.key.scancode = scan;
    input.process_event(event, suppressed);
}

bool is_neutral(const f3rt::InputMapper &input) {
    for (unsigned p = 0; p < f3rt::local_player_count; ++p) {
        if (input.word(p)) return false;
    }
    return true;
}

struct ScopedTempFile {
    std::filesystem::path path;
    ScopedTempFile() {
        path = std::filesystem::temp_directory_path() /
            ("f3rt-frontend-rc-" + std::to_string(SDL_GetTicksNS()) + "-" +
             std::to_string(++g_temp_counter) + ".cfg");
    }
    ~ScopedTempFile() {
        std::error_code ec;
        if (!path.empty()) std::filesystem::remove(path, ec);
    }
    void write(const std::string &content) const {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << content;
    }
};

bool settings_equal(const f3rt::FrontendSettings &a, const f3rt::FrontendSettings &b) {
    if (a.renderer != b.renderer || a.video_scale != b.video_scale ||
        a.border != b.border || a.filter != b.filter ||
        a.interpolation != b.interpolation ||
        a.interpolation_fields != b.interpolation_fields ||
        a.postprocess != b.postprocess || a.user_shader != b.user_shader ||
        a.audio_backend != b.audio_backend ||
        std::abs(a.volume - b.volume) >= 0.001f ||
        a.fast_boot != b.fast_boot || a.boot_cache != b.boot_cache ||
        a.motion_interp != b.motion_interp) {
        return false;
    }
    for (unsigned p = 0; p < f3rt::local_player_count; ++p) {
        if (a.profiles[p].device_slot != b.profiles[p].device_slot) return false;
        for (unsigned c = 0; c < f3rt::local_control_count; ++c) {
            const auto &ca = a.profiles[p].controls[c];
            const auto &cb = b.profiles[p].controls[c];
            if (ca.key != cb.key || ca.button != cb.button ||
                ca.axis != cb.axis || ca.direction != cb.direction) {
                return false;
            }
        }
    }
    return true;
}

static const std::vector<SDL_Scancode> kValidScancodes = {
    SDL_SCANCODE_UNKNOWN, SDL_SCANCODE_A, SDL_SCANCODE_B, SDL_SCANCODE_C,
    SDL_SCANCODE_D, SDL_SCANCODE_E, SDL_SCANCODE_F, SDL_SCANCODE_G,
    SDL_SCANCODE_H, SDL_SCANCODE_I, SDL_SCANCODE_J, SDL_SCANCODE_K,
    SDL_SCANCODE_L, SDL_SCANCODE_M, SDL_SCANCODE_N, SDL_SCANCODE_O,
    SDL_SCANCODE_P, SDL_SCANCODE_Q, SDL_SCANCODE_R, SDL_SCANCODE_S,
    SDL_SCANCODE_T, SDL_SCANCODE_U, SDL_SCANCODE_V, SDL_SCANCODE_W,
    SDL_SCANCODE_X, SDL_SCANCODE_Y, SDL_SCANCODE_Z,
    SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4,
    SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8,
    SDL_SCANCODE_9, SDL_SCANCODE_0,
    SDL_SCANCODE_RETURN, SDL_SCANCODE_SPACE,
    SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
    SDL_SCANCODE_F2, SDL_SCANCODE_F3, SDL_SCANCODE_F4, SDL_SCANCODE_F5
};

} // namespace

class Frontend : public ::testing::Test {
protected:
    std::filesystem::path temp_path_;

    void SetUp() override {
        if (!SDL_WasInit(SDL_INIT_EVENTS)) {
            ASSERT_TRUE(SDL_Init(SDL_INIT_EVENTS)) << SDL_GetError();
        }
        temp_path_ = std::filesystem::temp_directory_path() /
            ("f3rt-frontend-test-" + std::to_string(SDL_GetTicksNS()) + "-" +
             std::to_string(++g_temp_counter) + ".cfg");
    }

    void TearDown() override {
        std::error_code ec;
        if (!temp_path_.empty()) {
            std::filesystem::remove(temp_path_, ec);
        }
    }

    void write_config(const std::string &text) {
        std::ofstream out(temp_path_, std::ios::trunc);
        out << text;
        out.close();
        ASSERT_TRUE(bool(out)) << "Write temporary settings fixture";
    }
};

TEST_F(Frontend, Player1DefaultsAndLocalProfileIndependence) {
    f3rt::FrontendSettings settings;
    f3rt::InputMapper input(settings);
    const std::array<SDL_Scancode, f3rt::local_control_count> p1 = {
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_1, SDL_SCANCODE_5,
        SDL_SCANCODE_F3, SDL_SCANCODE_F2, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D};
    for (unsigned c = 0; c < p1.size(); ++c) {
        send_key(input, p1[c], true);
        EXPECT_EQ(input.word(0), 1u << c) << "P1 control indices and extra-button defaults";
        for (unsigned p = 1; p < f3rt::local_player_count; ++p) {
            EXPECT_EQ(input.word(p), 0) << "Other local profiles default to independent controls";
        }
        send_key(input, p1[c], false);
    }
    EXPECT_EQ(input.word(f3rt::local_player_count), 0) << "Out-of-range player word is neutral";
}

TEST_F(Frontend, FourPlayerStartAndCoinDefaults) {
    f3rt::FrontendSettings settings;
    f3rt::InputMapper input(settings);
    const std::array<SDL_Scancode, f3rt::local_player_count> starts = {
        SDL_SCANCODE_1, SDL_SCANCODE_2, SDL_SCANCODE_3, SDL_SCANCODE_4};
    const std::array<SDL_Scancode, f3rt::local_player_count> coins = {
        SDL_SCANCODE_5, SDL_SCANCODE_6, SDL_SCANCODE_7, SDL_SCANCODE_8};
    for (unsigned p = 0; p < f3rt::local_player_count; ++p) {
        EXPECT_EQ(settings.profiles[p].device_slot, int(p)) << "Default gamepad ordinals follow all local players";
        send_key(input, starts[p], true);
        send_key(input, coins[p], true);
        for (unsigned other = 0; other < f3rt::local_player_count; ++other) {
            EXPECT_EQ(input.word(other), other == p ? 0x180 : 0) << "Each local start/coin pair targets only its player";
        }
        send_key(input, starts[p], false);
        send_key(input, coins[p], false);
    }
}

TEST_F(Frontend, MenuClosureSuppressesHeldInputUntilPhysicalRelease) {
    f3rt::FrontendSettings settings;
    f3rt::InputMapper input(settings);
    send_key(input, SDL_SCANCODE_Z, true);
    input.release();
    send_key(input, SDL_SCANCODE_Z, true);
    EXPECT_TRUE(is_neutral(input)) << "Held input must remain suppressed after menu closure";
    send_key(input, SDL_SCANCODE_Z, false);
    send_key(input, SDL_SCANCODE_Z, true);
    EXPECT_EQ(input.word(0), 0x10) << "Physical release rearms gameplay input";
    send_key(input, SDL_SCANCODE_Z, false);
}

TEST_F(Frontend, ExtraButtonSuppressionReleaseAndFocusLoss) {
    f3rt::FrontendSettings settings;
    f3rt::InputMapper input(settings);
    const std::array<SDL_Scancode, f3rt::local_control_count> p1 = {
        SDL_SCANCODE_UP, SDL_SCANCODE_DOWN, SDL_SCANCODE_LEFT, SDL_SCANCODE_RIGHT,
        SDL_SCANCODE_Z, SDL_SCANCODE_X, SDL_SCANCODE_C, SDL_SCANCODE_1, SDL_SCANCODE_5,
        SDL_SCANCODE_F3, SDL_SCANCODE_F2, SDL_SCANCODE_A, SDL_SCANCODE_S, SDL_SCANCODE_D};
    for (unsigned c = 11; c < p1.size(); ++c) {
        send_key(input, p1[c], true, true);
        send_key(input, p1[c], true);
        EXPECT_TRUE(is_neutral(input)) << "Suppressed extra buttons stay blocked until physical release";
        send_key(input, p1[c], false);
        send_key(input, p1[c], true);
        EXPECT_EQ(input.word(0), 1u << c) << "Physical release rearms each extra button";
        input.release();
        send_key(input, p1[c], true);
        EXPECT_TRUE(is_neutral(input)) << "Menu closure blocks each held extra button";
        send_key(input, p1[c], false);
    }
    send_key(input, SDL_SCANCODE_A, true);
    send_key(input, SDL_SCANCODE_S, true);
    send_key(input, SDL_SCANCODE_D, true);
    EXPECT_EQ(input.word(0), 0x3800) << "All three extra buttons may be held together";
    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    input.process_event(focus, false);
    EXPECT_TRUE(is_neutral(input)) << "Focus loss releases every extra button";
    for (unsigned c = 11; c < p1.size(); ++c) send_key(input, p1[c], false);
}

TEST_F(Frontend, CaptureCancellationAndRemapIndependence) {
    f3rt::FrontendSettings settings;
    f3rt::InputMapper input(settings);
    input.start_capture(1, 4, true);
    input.release(); // F1 closure, focus loss, or device removal.
    send_key(input, SDL_SCANCODE_Z, true);
    EXPECT_TRUE(!input.capturing() && input.word(0) == 0x10) << "Closing gamepad capture must not disable keyboard gameplay";
    unsigned profile = 0, control = 0;
    f3rt::InputBinding binding;
    EXPECT_FALSE(input.take_binding(profile, control, binding)) << "Cancelled capture must not swallow a later gameplay key";
    send_key(input, SDL_SCANCODE_Z, false);
    input.start_capture(1, 4, false);
    send_key(input, SDL_SCANCODE_V, true, true);
    EXPECT_TRUE(input.take_binding(profile, control, binding) && profile == 1 && control == 4) << "P2 binding capture identifies its own control";
    settings.profiles[profile].controls[control] = binding;
    input.configure(settings);
    EXPECT_TRUE(is_neutral(input)) << "Capture key must not leak into any local player";
    send_key(input, SDL_SCANCODE_V, false);
    send_key(input, SDL_SCANCODE_V, true);
    EXPECT_TRUE(input.word(1) == 0x10 && input.word(0) == 0) << "P2 remap must remain independent of P1";
    send_key(input, SDL_SCANCODE_V, false);
}

TEST_F(Frontend, ExtraButtonCaptureAndCancelForAdditionalPlayers) {
    f3rt::FrontendSettings settings;
    f3rt::InputMapper input(settings);
    unsigned profile = 0, control = 0;
    f3rt::InputBinding binding;
    const std::array<SDL_Scancode, 2> extra_keys = {SDL_SCANCODE_G, SDL_SCANCODE_H};
    for (unsigned p = 2; p < f3rt::local_player_count; ++p) {
        const unsigned c = (p == 2 ? 11 : 13);
        input.start_capture(p, c, true);
        input.release();
        send_key(input, SDL_SCANCODE_D, true);
        EXPECT_TRUE(!input.capturing() && input.word(0) == 0x2000) << "Cancelling P3/P4 extra-button pad capture restores gameplay";
        EXPECT_FALSE(input.take_binding(profile, control, binding)) << "Cancelled extra-button capture yields no binding";
        send_key(input, SDL_SCANCODE_D, false);
        input.start_capture(p, c, false);
        send_key(input, extra_keys[p - 2], true, true);
        EXPECT_TRUE(input.take_binding(profile, control, binding) && profile == p && control == c) << "P3/P4 capture preserves player and appended control indices";
        settings.profiles[profile].controls[control] = binding;
        input.configure(settings);
        EXPECT_TRUE(is_neutral(input)) << "Captured extra-button key remains blocked for all profiles";
        send_key(input, extra_keys[p - 2], false);
        send_key(input, extra_keys[p - 2], true);
        for (unsigned other = 0; other < f3rt::local_player_count; ++other) {
            EXPECT_EQ(input.word(other), other == p ? (1u << c) : 0) << "P3/P4 remaps are independent of every other player";
        }
        send_key(input, extra_keys[p - 2], false);
    }
    send_key(input, SDL_SCANCODE_G, true);
    send_key(input, SDL_SCANCODE_H, true);
    SDL_Event focus{};
    focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
    input.process_event(focus, false);
    EXPECT_TRUE(is_neutral(input)) << "Focus loss releases independent P3/P4 remapped extra buttons";
}

TEST_F(Frontend, CaptureEscapeAndBoundsValidation) {
    f3rt::FrontendSettings settings;
    f3rt::InputMapper input(settings);
    unsigned profile = 0, control = 0;
    f3rt::InputBinding binding;
    input.start_capture(3, 12, false);
    send_key(input, SDL_SCANCODE_ESCAPE, true, true);
    EXPECT_TRUE(!input.capturing() && !input.take_binding(profile, control, binding) && is_neutral(input))
        << "Escape cancels an appended-control capture without leaking gameplay input";
    send_key(input, SDL_SCANCODE_ESCAPE, false);
    input.start_capture(f3rt::local_player_count, 13, false);
    input.start_capture(3, f3rt::local_control_count, false);
    EXPECT_FALSE(input.capturing()) << "Capture rejects players or controls outside the shared local counts";
}

TEST_F(Frontend, LegacySettingsCompatibility) {
    const std::array<SDL_Scancode, 11> old_keys = {
        SDL_SCANCODE_I, SDL_SCANCODE_K, SDL_SCANCODE_J, SDL_SCANCODE_L,
        SDL_SCANCODE_Q, SDL_SCANCODE_W, SDL_SCANCODE_E, SDL_SCANCODE_R,
        SDL_SCANCODE_T, SDL_SCANCODE_Y, SDL_SCANCODE_U};
    std::ostringstream old;
    old << "device0=2\ndevice1=3\n";
    for (unsigned c = 0; c < old_keys.size(); ++c) {
        old << "binding0." << c << '=' << int(old_keys[c]) << " -1 -1 1\n";
    }
    old << "binding1.7=" << int(SDL_SCANCODE_N) << " -1 -1 1\nbinding1.8=" << int(SDL_SCANCODE_M) << " -1 -1 1\n";
    write_config(old.str());
    f3rt::FrontendSettings settings;
    std::string error;
    ASSERT_TRUE(f3rt::load_frontend_settings(temp_path_.string(), settings, error)) << "Load an old two-player, eleven-control settings file";
    f3rt::InputMapper input(settings);
    for (unsigned c = 0; c < old_keys.size(); ++c) {
        send_key(input, old_keys[c], true);
        EXPECT_EQ(input.word(0), 1u << c) << "Old persisted control indices retain their original gameplay meaning";
        send_key(input, old_keys[c], false);
    }
    send_key(input, SDL_SCANCODE_N, true);
    send_key(input, SDL_SCANCODE_M, true);
    EXPECT_EQ(input.word(1), 0x180) << "Old P2 start/coin bindings retain indices seven and eight";
    send_key(input, SDL_SCANCODE_N, false);
    send_key(input, SDL_SCANCODE_M, false);
    send_key(input, SDL_SCANCODE_A, true);
    send_key(input, SDL_SCANCODE_S, true);
    send_key(input, SDL_SCANCODE_D, true);
    EXPECT_EQ(input.word(0), 0x3800) << "Missing appended settings leave extra-button defaults intact";
    send_key(input, SDL_SCANCODE_A, false);
    send_key(input, SDL_SCANCODE_S, false);
    send_key(input, SDL_SCANCODE_D, false);
    send_key(input, SDL_SCANCODE_3, true);
    send_key(input, SDL_SCANCODE_7, true);
    send_key(input, SDL_SCANCODE_4, true);
    send_key(input, SDL_SCANCODE_8, true);
    EXPECT_TRUE(input.word(2) == 0x180 && input.word(3) == 0x180 && input.word(0) == 0 && input.word(1) == 0)
        << "Old settings preserve default additional-player start/coin inputs";
}

TEST_F(Frontend, SettingsRoundTripAndReloadedInput) {
    f3rt::FrontendSettings settings;
    settings.profiles[2].device_slot = 5;
    settings.profiles[3].device_slot = 7;
    settings.profiles[2].controls[0].key = SDL_SCANCODE_H;
    settings.profiles[2].controls[11] = {SDL_SCANCODE_V, SDL_GAMEPAD_BUTTON_NORTH, -1, 1};
    settings.profiles[2].controls[12].key = SDL_SCANCODE_F;
    settings.profiles[2].controls[13].key = SDL_SCANCODE_G;
    settings.profiles[3].controls[1].key = SDL_SCANCODE_J;
    settings.profiles[3].controls[11].key = SDL_SCANCODE_B;
    settings.profiles[3].controls[12] = {SDL_SCANCODE_N, -1, SDL_GAMEPAD_AXIS_RIGHTX, -1};
    settings.profiles[3].controls[13].key = SDL_SCANCODE_M;
    std::string error;
    ASSERT_TRUE(f3rt::save_frontend_settings(temp_path_.string(), settings, error)) << "Save four profiles with appended button bindings";
    f3rt::FrontendSettings loaded;
    ASSERT_TRUE(f3rt::load_frontend_settings(temp_path_.string(), loaded, error)) << "Reload four-player settings";
    for (unsigned p = 0; p < f3rt::local_player_count; ++p) {
        EXPECT_EQ(loaded.profiles[p].device_slot, settings.profiles[p].device_slot) << "Roundtrip preserves each player's gamepad ordinal";
        for (unsigned c = 0; c < f3rt::local_control_count; ++c) {
            const auto &actual = loaded.profiles[p].controls[c], &expected = settings.profiles[p].controls[c];
            EXPECT_TRUE(actual.key == expected.key && actual.button == expected.button && actual.axis == expected.axis && actual.direction == expected.direction)
                << "Roundtrip preserves keyboard, button and signed-axis bindings for every local control";
        }
    }
    f3rt::InputMapper input(loaded);
    for (unsigned p = 2; p < f3rt::local_player_count; ++p) {
        for (unsigned c = 0; c < f3rt::local_control_count; ++c) {
            const auto scan = loaded.profiles[p].controls[c].key;
            if (scan == SDL_SCANCODE_UNKNOWN) continue;
            send_key(input, scan, true);
            for (unsigned other = 0; other < f3rt::local_player_count; ++other) {
                EXPECT_EQ(input.word(other), other == p ? (1u << c) : 0) << "Reloaded P3/P4 remaps produce only the intended player's input bits";
            }
            send_key(input, scan, false);
        }
    }
}

TEST_F(Frontend, SettingsValidationRejectsCorruptInput) {
    f3rt::FrontendSettings settings;
    settings.profiles[3].controls[13].key = SDL_SCANCODE_M;
    std::string error;
    ASSERT_TRUE(f3rt::save_frontend_settings(temp_path_.string(), settings, error));
    f3rt::FrontendSettings loaded;
    ASSERT_TRUE(f3rt::load_frontend_settings(temp_path_.string(), loaded, error));
    f3rt::InputMapper input(loaded);

    const std::array<std::string, 8> invalid = {
        "device" + std::to_string(f3rt::local_player_count) + "=0\n",
        "device3=16\n",
        "binding" + std::to_string(f3rt::local_player_count) + ".0=0 -1 -1 1\n",
        "binding3." + std::to_string(f3rt::local_control_count) + "=0 -1 -1 1\n",
        "binding3.13=" + std::to_string(SDL_SCANCODE_F1) + " -1 -1 1\n",
        "binding3.13=0 " + std::to_string(SDL_GAMEPAD_BUTTON_COUNT) + " -1 1\n",
        "binding3.13=0 -1 " + std::to_string(SDL_GAMEPAD_AXIS_COUNT) + " 1\n",
        "binding3.13=0 -1 -1 0\n"};
    for (const auto &text : invalid) {
        write_config(text);
        EXPECT_FALSE(f3rt::load_frontend_settings(temp_path_.string(), loaded, error)) << "Reject invalid device or extra-player binding without applying it";
        input.configure(loaded);
        send_key(input, SDL_SCANCODE_M, true);
        EXPECT_TRUE(input.word(3) == 0x2000 && input.word(0) == 0 && input.word(1) == 0 && input.word(2) == 0)
            << "Rejected settings preserve the previous playable P4 mapping";
        send_key(input, SDL_SCANCODE_M, false);
    }
}

TEST_F(Frontend, SaveValidationRejectsReservedKeys) {
    f3rt::FrontendSettings settings;
    std::string error;
    auto invalid_settings = settings;
    invalid_settings.profiles[3].controls[13].key = SDL_SCANCODE_F12;
    EXPECT_FALSE(f3rt::save_frontend_settings(temp_path_.string(), invalid_settings, error))
        << "Save validates additional-player appended bindings";
}

// RapidCheck property: Generated valid settings survive a save->load round-trip exactly.
RC_GTEST_PROP(FrontendSettings, SaveLoadRoundTrip, ()) {
    auto gen_clean_string = [](size_t max_len) {
        const size_t len = *rc::gen::inRange<size_t>(0, max_len + 1);
        std::string s;
        s.reserve(len);
        for (size_t i = 0; i < len; ++i) {
            s.push_back(char(*rc::gen::inRange(32, 127)));
        }
        return s;
    };

    f3rt::FrontendSettings s;
    s.renderer = *rc::gen::element<std::string>("accurate", "enhanced");
    s.video_scale = *rc::gen::element<std::string>("1", "2", "3", "4", "auto", "auto-integer");
    s.border = *rc::gen::inRange(0u, 161u);
    s.filter = *rc::gen::element<std::string>("nearest", "linear");
    s.interpolation = *rc::gen::element<std::string>("off", "linear", "fit");
    s.interpolation_fields = *rc::gen::element<std::string>("none", "geometry", "palette", "geometry,palette");
    s.postprocess = *rc::gen::element<std::string>("off", "crt", "user");
    s.user_shader = gen_clean_string(64);
    s.audio_backend = *rc::gen::element(f3rt::AudioBackend::Enhanced, f3rt::AudioBackend::Reference);
    s.volume = float(*rc::gen::inRange(0, 101)) / 100.0f;
    s.fast_boot = *rc::gen::arbitrary<bool>();
    s.boot_cache = *rc::gen::arbitrary<bool>();
    s.motion_interp = *rc::gen::arbitrary<bool>();

    for (unsigned p = 0; p < f3rt::local_player_count; ++p) {
        s.profiles[p].device_slot = *rc::gen::inRange(0, 16);
        for (unsigned c = 0; c < f3rt::local_control_count; ++c) {
            auto &b = s.profiles[p].controls[c];
            b.key = *rc::gen::elementOf(kValidScancodes);
            b.button = *rc::gen::inRange(-1, int(SDL_GAMEPAD_BUTTON_COUNT));
            b.axis = *rc::gen::inRange(-1, int(SDL_GAMEPAD_AXIS_COUNT));
            b.direction = *rc::gen::element(1, -1);
        }
    }

    ScopedTempFile file;
    std::string error;
    RC_ASSERT(f3rt::save_frontend_settings(file.path.string(), s, error));
    f3rt::FrontendSettings loaded;
    RC_ASSERT(f3rt::load_frontend_settings(file.path.string(), loaded, error));

    RC_ASSERT(loaded.renderer == s.renderer);
    RC_ASSERT(loaded.video_scale == s.video_scale);
    RC_ASSERT(loaded.border == s.border);
    RC_ASSERT(loaded.filter == s.filter);
    RC_ASSERT(loaded.interpolation == s.interpolation);
    RC_ASSERT(loaded.interpolation_fields == s.interpolation_fields);
    RC_ASSERT(loaded.postprocess == s.postprocess);
    RC_ASSERT(loaded.user_shader == s.user_shader);
    RC_ASSERT(loaded.audio_backend == s.audio_backend);
    RC_ASSERT(std::abs(loaded.volume - s.volume) < 0.001f);
    RC_ASSERT(loaded.fast_boot == s.fast_boot);
    RC_ASSERT(loaded.boot_cache == s.boot_cache);
    RC_ASSERT(loaded.motion_interp == s.motion_interp);

    for (unsigned p = 0; p < f3rt::local_player_count; ++p) {
        RC_ASSERT(loaded.profiles[p].device_slot == s.profiles[p].device_slot);
        for (unsigned c = 0; c < f3rt::local_control_count; ++c) {
            const auto &act = loaded.profiles[p].controls[c];
            const auto &exp = s.profiles[p].controls[c];
            RC_ASSERT(act.key == exp.key);
            RC_ASSERT(act.button == exp.button);
            RC_ASSERT(act.axis == exp.axis);
            RC_ASSERT(act.direction == exp.direction);
        }
    }
}

// RapidCheck property: Arbitrary or mutated config text never crashes the parser;
// on failure, settings remain completely unchanged and error is non-empty.
RC_GTEST_PROP(FrontendSettings, LoaderRobustnessOnArbitraryConfigText, ()) {
    f3rt::FrontendSettings baseline;
    baseline.profiles[0].device_slot = 3;
    baseline.border = 3;
    auto target = baseline;

    ScopedTempFile file;
    const auto mode = *rc::gen::inRange(0, 3);
    std::string text;
    if (mode == 0) {
        const size_t len = *rc::gen::inRange<size_t>(0, 500);
        for (size_t i = 0; i < len; ++i) {
            text.push_back(char(*rc::gen::inRange(0, 128)));
        }
    } else if (mode == 1) {
        const size_t lines = *rc::gen::inRange<size_t>(1, 15);
        for (size_t l = 0; l < lines; ++l) {
            const auto k = *rc::gen::element<std::string>(
                "renderer", "scale", "device0", "device4", "binding0.0", "binding3.99",
                "volume", "corrupt_key", "fast_boot", "audio", "interpolation", "motion_interp");
            const auto v = *rc::gen::element<std::string>(
                "bad_val", "99999", "-5", "???", "", "off\ninjection", "1 2 3", "accurate");
            const bool has_equals = *rc::gen::arbitrary<bool>();
            if (has_equals) text += k + "=" + v + "\n";
            else text += k + " " + v + "\n";
        }
    } else {
        f3rt::FrontendSettings valid_s;
        std::string err;
        file.write("");
        f3rt::save_frontend_settings(file.path.string(), valid_s, err);
        std::ifstream in(file.path.string());
        text.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        in.close();
        const size_t corruptions = *rc::gen::inRange<size_t>(1, 6);
        for (size_t c = 0; c < corruptions && !text.empty(); ++c) {
            const size_t pos = *rc::gen::inRange<size_t>(0, text.size());
            text[pos] = char(*rc::gen::inRange(0, 256));
        }
    }

    file.write(text);
    std::string error;
    const bool ok = f3rt::load_frontend_settings(file.path.string(), target, error);
    if (!ok) {
        RC_ASSERT(settings_equal(target, baseline));
        RC_ASSERT(!error.empty());
    } else {
        RC_ASSERT(error.empty());
    }
}

// RapidCheck property: Arbitrary key press and release sequences over bound keys correctly
// track active control words, and releasing all keys returns to neutral.
RC_GTEST_PROP(FrontendInputMapper, ArbitraryKeySequencesTrackActiveControls, ()) {
    f3rt::FrontendSettings settings;
    for (auto &prof : settings.profiles) {
        for (auto &b : prof.controls) {
            b.key = SDL_SCANCODE_UNKNOWN;
        }
    }

    std::vector<SDL_Scancode> unique_keys;
    for (int code = SDL_SCANCODE_A; code <= SDL_SCANCODE_Z; ++code)
        unique_keys.push_back(SDL_Scancode(code));
    for (int code = SDL_SCANCODE_1; code <= SDL_SCANCODE_0; ++code)
        unique_keys.push_back(SDL_Scancode(code));
    for (int code = SDL_SCANCODE_F2; code <= SDL_SCANCODE_F10; ++code)
        unique_keys.push_back(SDL_Scancode(code));
    unique_keys.push_back(SDL_SCANCODE_UP);
    unique_keys.push_back(SDL_SCANCODE_DOWN);
    unique_keys.push_back(SDL_SCANCODE_LEFT);
    unique_keys.push_back(SDL_SCANCODE_RIGHT);
    unique_keys.push_back(SDL_SCANCODE_TAB);
    unique_keys.push_back(SDL_SCANCODE_SPACE);
    unique_keys.push_back(SDL_SCANCODE_BACKSPACE);
    unique_keys.push_back(SDL_SCANCODE_COMMA);
    unique_keys.push_back(SDL_SCANCODE_PERIOD);
    unique_keys.push_back(SDL_SCANCODE_SLASH);
    unique_keys.push_back(SDL_SCANCODE_SEMICOLON);
    unique_keys.push_back(SDL_SCANCODE_EQUALS);
    unique_keys.push_back(SDL_SCANCODE_MINUS);

    struct BindingInfo {
        unsigned player;
        unsigned control;
        SDL_Scancode key;
    };
    std::vector<BindingInfo> bindings;
    size_t key_idx = 0;
    for (unsigned p = 0; p < f3rt::local_player_count; ++p) {
        for (unsigned c = 0; c < f3rt::local_control_count; ++c) {
            const auto key = unique_keys[key_idx++];
            settings.profiles[p].controls[c].key = key;
            bindings.push_back({p, c, key});
        }
    }

    f3rt::InputMapper mapper(settings);
    std::vector<bool> key_state(bindings.size(), false);

    const size_t num_events = *rc::gen::inRange<size_t>(10, 50);
    for (size_t i = 0; i < num_events; ++i) {
        const size_t b_idx = *rc::gen::inRange<size_t>(0, bindings.size());
        const bool press = *rc::gen::arbitrary<bool>();
        send_key(mapper, bindings[b_idx].key, press);
        key_state[b_idx] = press;

        for (unsigned p = 0; p < f3rt::local_player_count; ++p) {
            f3rt::LocalInputWord expected = 0;
            for (unsigned c = 0; c < f3rt::local_control_count; ++c) {
                const size_t idx = p * f3rt::local_control_count + c;
                if (key_state[idx]) expected |= f3rt::LocalInputWord(1u << c);
            }
            RC_ASSERT(mapper.word(p) == expected);
        }
    }

    for (size_t b_idx = 0; b_idx < bindings.size(); ++b_idx) {
        if (key_state[b_idx]) {
            send_key(mapper, bindings[b_idx].key, false);
            key_state[b_idx] = false;
        }
    }
    RC_ASSERT(is_neutral(mapper));
}

// RapidCheck property: release() and focus loss block held keys until physical release.
RC_GTEST_PROP(FrontendInputMapper, ReleaseBlocksHeldKeysUntilPhysicalRelease, ()) {
    f3rt::FrontendSettings settings;
    for (auto &prof : settings.profiles) {
        for (auto &b : prof.controls) {
            b.key = SDL_SCANCODE_UNKNOWN;
        }
    }
    std::vector<SDL_Scancode> keys = {
        SDL_SCANCODE_A, SDL_SCANCODE_B, SDL_SCANCODE_C, SDL_SCANCODE_D,
        SDL_SCANCODE_E, SDL_SCANCODE_F, SDL_SCANCODE_G, SDL_SCANCODE_H
    };
    for (size_t i = 0; i < keys.size(); ++i) {
        settings.profiles[0].controls[i].key = keys[i];
    }
    f3rt::InputMapper mapper(settings);

    std::vector<bool> pressed(keys.size(), false);
    bool any_pressed = false;
    for (size_t i = 0; i < keys.size(); ++i) {
        pressed[i] = *rc::gen::arbitrary<bool>();
        if (pressed[i]) {
            send_key(mapper, keys[i], true);
            any_pressed = true;
        }
    }
    if (!any_pressed) {
        pressed[0] = true;
        send_key(mapper, keys[0], true);
    }

    f3rt::LocalInputWord expected_before = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (pressed[i]) expected_before |= f3rt::LocalInputWord(1u << i);
    }
    RC_ASSERT(mapper.word(0) == expected_before);

    const bool use_focus_lost = *rc::gen::arbitrary<bool>();
    if (use_focus_lost) {
        SDL_Event focus{};
        focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
        mapper.process_event(focus, false);
    } else {
        mapper.release();
    }

    RC_ASSERT(is_neutral(mapper));

    for (size_t i = 0; i < keys.size(); ++i) {
        if (pressed[i]) send_key(mapper, keys[i], true);
    }
    RC_ASSERT(is_neutral(mapper));

    std::vector<bool> rearmed(keys.size(), false);
    for (size_t i = 0; i < keys.size(); ++i) {
        if (pressed[i] && *rc::gen::arbitrary<bool>()) {
            send_key(mapper, keys[i], false);
            send_key(mapper, keys[i], true);
            rearmed[i] = true;
        }
    }

    f3rt::LocalInputWord expected_after = 0;
    for (size_t i = 0; i < keys.size(); ++i) {
        if (rearmed[i]) expected_after |= f3rt::LocalInputWord(1u << i);
    }
    RC_ASSERT(mapper.word(0) == expected_after);

    for (size_t i = 0; i < keys.size(); ++i) {
        send_key(mapper, keys[i], false);
    }
    RC_ASSERT(is_neutral(mapper));
}
