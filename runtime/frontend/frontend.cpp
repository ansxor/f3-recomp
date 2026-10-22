#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/game_video.hpp"
#include "f3rt/game_boot.hpp"
#include "f3rt/input.hpp"
#include "f3rt/game_netplay.hpp"
#include "f3rt/netplay.hpp"
#include "f3rt/rom.hpp"
#include "generated_config/build_identity.hpp"
#include "netplay/recorder.hpp"
#include "ui.hpp"
#include "state.hpp"
#include "interpreter.hpp"
#include "capture_io.hpp"
#include "renderer/scale.hpp"
#include "renderer/gpu/interp.hpp"
#include "debug/profile.hpp"
#include "debug/input_script.hpp"
#include "sprites/units.hpp"
#include "debug/discovery.hpp"
#include "sprites/behaviour.hpp"
#ifdef F3RT_GPU
#include "renderer/gpu/video.hpp"
#include "f3rt/video.hpp"
#endif
#include <SDL3/SDL.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <iomanip>
#include <iostream>
#include <memory>
#include <optional>
#include <thread>
#include <unordered_set>
#ifdef __APPLE__
#include <pthread.h>
#endif
#include <atomic>
#include <condition_variable>
#include <exception>
#include <mutex>
#ifdef F3RT_GENERATED
#include "program.h"
#endif
#ifdef F3RT_SOUND_GENERATED
#include "sound_program.h"
#endif
namespace {
struct Sdl {
    SDL_Window *window = nullptr;
    SDL_Renderer *renderer = nullptr;
    SDL_Texture *texture = nullptr;
    SDL_AudioStream *audio = nullptr;
    std::unique_ptr<f3rt::InputMapper> input;
    std::unique_ptr<f3rt::FrontendUi> ui;
#ifdef F3RT_GPU
    std::unique_ptr<f3rt::GpuVideo> gpu;
#endif
    ~Sdl() {
        ui.reset();
        input.reset();
#ifdef F3RT_GPU
        gpu.reset();
#endif
        SDL_DestroyAudioStream(audio);
        SDL_DestroyTexture(texture);
        SDL_DestroyRenderer(renderer);
        SDL_DestroyWindow(window);
        SDL_Quit();
    }
};
#ifdef F3RT_GPU
struct MotionReadout {
    std::chrono::steady_clock::time_point begin;
    uint64_t native = 0, submissions = 0, intermediate = 0, moving_inbetween = 0;
    uint64_t moving_intermediate = 0, sampled_pairs = 0, moving_pairs = 0, resets = 0;
    f3rt::MotionInterpolationStats census{};
    float min_alpha = 1, max_alpha = 0;
    void observe(const f3rt::MotionInterpolationStats &s, bool new_pair) {
        ++submissions;
        const bool moving = s.moving_sprites || s.moving_playfield_rows || s.moving_text_rows;
        const bool accepted = s.sprites || s.playfield_rows || s.text_rows;
        const bool between = s.paired && s.alpha > 0 && s.alpha < 1;
        intermediate += between && accepted;
        moving_inbetween += between && moving;
        moving_intermediate += between && moving && accepted;
        if (s.paired) {
            min_alpha = std::min(min_alpha, s.alpha);
            max_alpha = std::max(max_alpha, s.alpha);
        }
        if (!new_pair) return;
        ++sampled_pairs;
        moving_pairs += moving;
        census.sprites += s.sprites;
        census.playfield_rows += s.playfield_rows;
        census.text_rows += s.text_rows;
        census.moving_sprites += s.moving_sprites;
        census.moving_playfield_rows += s.moving_playfield_rows;
        census.moving_text_rows += s.moving_text_rows;
        census.rejected_sprites += s.rejected_sprites;
        census.rejected_rows += s.rejected_rows;
        census.sprite_count_rejections += s.sprite_count_rejections;
        census.sprite_identity_rejections += s.sprite_identity_rejections;
        census.sprite_transform_rejections += s.sprite_transform_rejections;
        census.sprite_jump_rejections += s.sprite_jump_rejections;
        census.row_control_rejections += s.row_control_rejections;
        census.row_transform_rejections += s.row_transform_rejections;
        census.row_jump_rejections += s.row_jump_rejections;
    }
    void report(const f3rt::GpuVideo &gpu) {
        const auto now = std::chrono::steady_clock::now();
        const double seconds = std::chrono::duration<double>(now - begin).count();
        const auto percent = [](uint64_t n, uint64_t d) {
            return d ? 100.0 * double(n) / double(d) : 0.0;
        };
        const auto flags = std::cout.flags();
        const auto precision = std::cout.precision();
        std::cout << std::fixed << std::setprecision(1) << "MOTION display_hz=" << gpu.display_hz()
                  << " requested_display_hz=" << gpu.requested_display_hz()
                  << " display_callback_hz=" << gpu.display_callback_hz() << " scanout_hz=unknown"
                  << " drawable_submissions_s=" << double(submissions) / seconds
                  << " native_s=" << double(native) / seconds
                  << " interpolated_pct=" << percent(intermediate, submissions)
                  << " moving_interpolated_pct=" << percent(moving_intermediate, moving_inbetween)
                  << " moving_inbetweens=" << moving_inbetween
                  << " sampled_native_pairs=" << sampled_pairs
                  << " moving_native_pairs=" << moving_pairs << " history_resets=" << resets
                  << " alpha=" << min_alpha << ':' << max_alpha << '\n'
                  << "MOTION-CANDIDATES moving_sprites=" << census.moving_sprites
                  << " moving_pf_rows=" << census.moving_playfield_rows
                  << " moving_text_rows=" << census.moving_text_rows
                  << " accepted_sprites=" << census.sprites
                  << " accepted_pf_rows=" << census.playfield_rows
                  << " accepted_text_rows=" << census.text_rows
                  << " rejected_sprites=" << census.rejected_sprites
                  << " sprite_identity=" << census.sprite_identity_rejections
                  << " sprite_count_subset=" << census.sprite_count_rejections
                  << " sprite_transform=" << census.sprite_transform_rejections
                  << " sprite_jump=" << census.sprite_jump_rejections
                  << " rejected_rows=" << census.rejected_rows
                  << " row_control=" << census.row_control_rejections
                  << " row_transform=" << census.row_transform_rejections
                  << " row_jump=" << census.row_jump_rejections << '\n';
        std::cout.flags(flags);
        std::cout.precision(precision);
        std::cout.flush();
        *this = MotionReadout{.begin = now};
    }
};
#endif
void check(bool result) {
    if (!result) throw std::runtime_error(SDL_GetError());
}
void parse_scale(const std::string &scale, f3rt::VideoScaleMode &mode,
                 f3rt::GameVideoOptions &options) {
    if (scale == "auto" || scale == "auto-integer") {
        mode = scale == "auto" ? f3rt::VideoScaleMode::Auto : f3rt::VideoScaleMode::AutoInteger;
        options.scale = 1;
    } else {
        size_t consumed = 0;
        unsigned long numeric = 0;
        try {
            numeric = std::stoul(scale, &consumed);
        } catch (const std::exception &) {
            throw std::runtime_error("Video scale must be 1..4, auto or auto-integer");
        }
        if (consumed != scale.size() || !numeric || numeric > f3rt::GameVideoOptions::MAX_SCALE)
            throw std::runtime_error("Video scale must be 1..4, auto or auto-integer");
        options.scale = unsigned(numeric);
        mode = f3rt::VideoScaleMode::Fixed;
    }
}
// Settings and CLI values are validated before this point ("off" | "linear" | "fit").
f3rt::VideoInterpolation parse_interpolation_mode(const std::string &mode) {
    return mode == "fit"      ? f3rt::VideoInterpolation::Fit
           : mode == "linear" ? f3rt::VideoInterpolation::Linear
                              : f3rt::VideoInterpolation::Off;
}
enum Preference : uint32_t {
    Renderer = 1u << 0,
    Scale = 1u << 2,
    Border = 1u << 3,
    Filter = 1u << 4,
    Interpolation = 1u << 5,
    Fields = 1u << 6,
    Volume = 1u << 8,
    PostprocessMode = 1u << 13,
    UserShader = 1u << 14,
    SoundBackend = 1u << 15,
    FastBoot = 1u << 16,
    BootCache = 1u << 17,
    MotionInterp = 1u << 18
};
// Hand-off between the emulation thread (writer of every Machine access) and the video thread (SDL window,
// UI, GPU). Everything the video thread may read of the machine travels in a Snapshot; everything it may ask
// of the machine travels as a Command. One mutex guards the slots and queues; slots are triple-buffered and
// latest-wins, so a slow presenter drops stale frames and never delays emulation.
struct Snapshot {
    f3rt::CapturedFrame *gpu =
        nullptr; // GPU path: capture buffer Link::captures[capture], immutable while published
    size_t capture = 0;
    std::vector<uint32_t> pixels; // CPU/FDP path: presentation pixels
    uint64_t frame = 0,
             epoch =
                 0; // epoch changes on every emulation discontinuity (load, resume, clock resync)
    unsigned scale = 1; // GameVideo GPU scale this frame was captured at
    std::chrono::steady_clock::time_point
        start; // scheduled emulation start of the frame (motion interpolation clock)
};
struct NetSettings {
    std::string server, name;
    uint8_t delay = 0;
};
struct Command {
    enum Kind {
        Pause,
        Resume,
        Save,
        Load,
        SetScale,
        NetHost,
        NetJoin,
        NetCancel,
        NetLocal,
        Chat
    } kind;
    unsigned scale = 0;
    std::filesystem::path path;
    std::string text; // NetJoin: room code; Chat: message
    NetSettings net;  // NetHost/NetJoin: settings sampled when the button was pressed
};
struct Reply {
    std::string message;
    bool loaded = false;
};
// Netplay state owned by the emulation thread (lobby, session, match) as the UI shows it.
struct NetStatus {
    bool lobby_open = false, active = false, in_session = false;
    std::string status, error, room_code, peer_name, desync_path;
    uint32_t ping = 0;
    uint64_t stalls = 0;
    uint8_t delay = 0, player = 0;
    std::vector<std::pair<uint8_t, std::string>>
        chat; // received lines; the video thread drains them
    bool operator==(const NetStatus &) const = default;
};
struct Link {
    static constexpr int SLOT_COUNT = 3;
    std::mutex mutex;
    std::condition_variable cv;
    std::array<Snapshot, SLOT_COUNT> slots;
    std::array<f3rt::CapturedFrame *, SLOT_COUNT> captures{};
    int latest = -1, held = -1, writing = -1;
    uint64_t published = 0, seen = 0;
    std::atomic<uint64_t> frames_published{0};
    std::vector<Command> commands;
    std::vector<Reply> replies;
    bool stop = false, done = false;
    std::atomic<uint64_t> frame{0};
    std::array<std::atomic<uint16_t>, f3rt::LOCAL_PLAYER_COUNT> input{};
    NetStatus net;
    bool net_dirty = false;
    std::atomic<bool> chat_open{
        false}; // video thread: chat box has keyboard focus, so game input stays out of the match
    // Emulation thread: scheduled start of its next frame (steady_clock ns), 0 while paused or unthrottled. Lets the
    // video thread sample input just before the frame latches it.
    std::atomic<int64_t> deadline{0};
    // Emulation thread: a slot (and capture buffer) the presenter cannot be reading.
    Snapshot &begin_frame() {
        std::lock_guard lock(mutex);
        int slot = 0;
        while (slot == latest || slot == held) ++slot;
        Snapshot &s = slots[slot];
        s.gpu = nullptr;
        if (captures[0]) {
            size_t c = 0;
            while ((latest >= 0 && c == slots[latest].capture) ||
                   (held >= 0 && c == slots[held].capture))
                ++c;
            s.capture = c;
            s.gpu = captures[c];
        }
        writing = slot;
        return s;
    }
    void publish(bool frame_boundary = true) {
        {
            std::lock_guard lock(mutex);
            latest = writing;
            ++published;
            if (frame_boundary) frames_published.fetch_add(1, std::memory_order_relaxed);
        }
        cv.notify_all();
    }
    // Emulation thread, GPU path: re-issue the newest frame with a new scale while no new frame is coming.
    void restate(unsigned scale) {
        {
            std::lock_guard lock(mutex);
            if (latest < 0) return;
            int slot = 0;
            while (slot == latest || slot == held) ++slot;
            slots[slot].gpu = slots[latest].gpu;
            slots[slot].capture = slots[latest].capture;
            slots[slot].frame = slots[latest].frame;
            slots[slot].epoch = slots[latest].epoch;
            slots[slot].start = slots[latest].start;
            slots[slot].scale = scale;
            latest = slot;
            ++published;
        }
        cv.notify_all();
    }
    // Video thread: newest published snapshot (fresh when not yet seen) or the one it already holds.
    const Snapshot *take(bool &fresh) {
        std::lock_guard lock(mutex);
        fresh = latest >= 0 && published != seen;
        if (fresh) {
            held = latest;
            seen = published;
        }
        return held >= 0 ? &slots[held] : nullptr;
    }
    void post(Command command) {
        {
            std::lock_guard lock(mutex);
            commands.push_back(std::move(command));
        }
        cv.notify_all();
    }
    void reply(Reply value) {
        {
            std::lock_guard lock(mutex);
            replies.push_back(std::move(value));
        }
        cv.notify_all();
    }
    std::vector<Reply> take_replies() {
        std::lock_guard lock(mutex);
        return std::exchange(replies, {});
    }
    void request_stop() {
        {
            std::lock_guard lock(mutex);
            stop = true;
        }
        cv.notify_all();
    }
    void finish() {
        {
            std::lock_guard lock(mutex);
            done = true;
        }
        cv.notify_all();
    }
    bool finished() {
        std::lock_guard lock(mutex);
        return done;
    }
    // Emulation thread: hands the netplay state to the video thread; received chat lines move out of `value`.
    void publish_net(NetStatus &value) {
        {
            std::lock_guard lock(mutex);
            auto chat = std::move(net.chat);
            for (auto &line : value.chat) chat.push_back(std::move(line));
            value.chat.clear();
            net = value;
            net.chat = std::move(chat);
            net_dirty = true;
        }
        cv.notify_all();
    }
    // Video thread: the newest netplay state when it changed since the last call.
    bool take_net(NetStatus &out) {
        std::lock_guard lock(mutex);
        if (!net_dirty) return false;
        out = std::move(net);
        net.chat.clear();
        net_dirty = false;
        return true;
    }
    // Video thread idle wait: wakes for a new frame, a reply, emulation exit, shortly before the emulation thread's
    // next frame deadline (so the caller can pump events and publish fresh input), or at the timeout. The margin
    // covers timer wake lateness on both threads (measured 0-2 ms on macOS, even at user-interactive QoS) plus the
    // event pump and UI draw, so the sample reliably precedes the latch; inside it the wait falls back to the
    // timeout, since the frame itself publishes a snapshot and wakes us.
    void wait_for_work(std::chrono::milliseconds timeout) {
        constexpr auto INPUT_MARGIN = std::chrono::milliseconds(2);
        const auto now = std::chrono::steady_clock::now();
        auto until = now + timeout;
        if (const int64_t ns = deadline.load(std::memory_order_relaxed)) {
            const auto sample =
                std::chrono::steady_clock::time_point(std::chrono::steady_clock::duration(ns)) -
                INPUT_MARGIN;
            if (sample > now && sample < until) until = sample;
        }
        std::unique_lock lock(mutex);
        cv.wait_until(lock, until,
                      [&] { return done || published != seen || !replies.empty() || net_dirty; });
    }
};
// Joins the emulation thread on every exit path of the video loop.
struct EmulationThread {
    Link &link;
    std::thread thread;
    explicit EmulationThread(Link &l) : link(l) {}
    ~EmulationThread() {
        if (thread.joinable()) {
            link.request_stop();
            thread.join();
        }
    }
};
} // namespace
int main(int argc, char **argv) try {
    std::filesystem::path romdir, dumpdir, eeprom, wav_path, fallback_report, surface;
    std::filesystem::path profile_path, config_path, discovery_path, inputs_path, watch_path,
        indirect_path;
    std::vector<uint32_t> watch_entries;
    std::string set = F3RT_DEFAULT_SET;
    std::string renderer;
    std::string audio_backend = "enhanced";
    f3rt::GameVideoOptions video_options;
    f3rt::VideoScaleMode video_scale_mode = f3rt::VideoScaleMode::Fixed;
    std::string video_filter = "nearest";
    std::string video_interp = "off";
    std::string video_interp_fields = "geometry";
    std::string video_scale = "1";
    std::string postprocess = "off", user_shader;
    float volume = 1;
    uint32_t cli_preferences = 0;
    uint64_t frames = 0, dump_start = 1, dump_every = 1;
    bool headless = false, sound = true, translated = false, throttle = true;
    bool motion_interp = false;
    bool fast_boot = true, boot_cache = false;
#ifdef F3RT_GAME
    romdir = F3RT_DEFAULT_ROM_DIR;
    translated = true;
    bool allow_fallback = false;
#else
    bool allow_fallback = true;
#endif
    bool discovery_all = false;
    std::string netplay_auto;
    std::string netplay_server_cli;
    std::string netplay_name_cli;
    uint8_t netplay_delay_cli = 0;
    std::vector<uint64_t> screenshot_frames;
    uint64_t open_f1_frame = 0;
    uint64_t chat_open_frame = 0;
    uint64_t chat_close_frame = 0;
    std::vector<std::pair<uint64_t, std::string>> chat_send_frames;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> const char * {
            if (i + 1 >= argc) throw std::runtime_error("Missing value for " + arg);
            return argv[++i];
        };
        if (arg == "--rom-dir") romdir = value();
        else if (arg == "--set") set = value();
        else if (arg == "--frames") frames = std::stoull(value());
        else if (arg == "--dump-dir") dumpdir = value();
        else if (arg == "--dump-start") dump_start = std::stoull(value());
        else if (arg == "--dump-every") dump_every = std::stoull(value());
        else if (arg == "--eeprom") eeprom = value();
        else if (arg == "--fast-boot") {
            const std::string v = value();
            if (v == "on") fast_boot = true;
            else if (v == "off") fast_boot = false;
            else throw std::runtime_error("--fast-boot must be on or off");
            cli_preferences |= FastBoot;
        } else if (arg == "--boot-cache") {
            const std::string v = value();
            if (v == "on") boot_cache = true;
            else if (v == "off") boot_cache = false;
            else throw std::runtime_error("--boot-cache must be on or off");
            cli_preferences |= BootCache;
        } else if (arg == "--wav") wav_path = value();
        else if (arg == "--profile-out") profile_path = value();
        else if (arg == "--discovery-log") discovery_path = value();
        else if (arg == "--discovery-all") discovery_all = true;
        else if (arg == "--inputs") inputs_path = value();
        else if (arg == "--watch-log") watch_path = value();
        else if (arg == "--indirect-log") indirect_path = value();
        else if (arg == "--watch-entries") {
            std::string list = value();
            for (size_t start = 0; start <= list.size();) {
                const size_t comma = std::min(list.find(',', start), list.size());
                watch_entries.push_back(
                    uint32_t(std::stoul(list.substr(start, comma - start), nullptr, 16)));
                start = comma + 1;
            }
        } else if (arg == "--audio-backend") {
            audio_backend = value();
            cli_preferences |= SoundBackend;
        } else if (arg == "--config") config_path = value();
        else if (arg == "--volume") {
            const std::string text = value();
            size_t consumed = 0;
            volume = std::stof(text, &consumed) / 100;
            if (consumed != text.size() || !std::isfinite(volume) || volume < 0 || volume > 1)
                throw std::runtime_error("--volume must be 0..100");
            cli_preferences |= Volume;
        } else if (arg == "--fallback-report") fallback_report = value();
        else if (arg == "--surface") surface = value();
        else if (arg == "--renderer") {
            renderer = value();
            cli_preferences |= Renderer;
        } else if (arg == "--video-scale") {
            video_scale = value();
            cli_preferences |= Scale;
        } else if (arg == "--video-border") {
            const auto border = std::stoul(value());
            if (border > f3rt::GameVideoOptions::MAX_BORDER)
                throw std::runtime_error("--video-border must be 0..160");
            video_options.border = unsigned(border);
            cli_preferences |= Border;
        } else if (arg == "--video-filter") {
            video_filter = value();
            cli_preferences |= Filter;
        } else if (arg == "--video-interp") {
            video_interp = value();
            cli_preferences |= Interpolation;
        } else if (arg == "--video-interp-fields") {
            video_interp_fields = value();
            cli_preferences |= Fields;
        } else if (arg == "--motion-interp") {
            motion_interp = true;
            cli_preferences |= MotionInterp;
        } else if (arg == "--postprocess") {
            postprocess = value();
            cli_preferences |= PostprocessMode;
        } else if (arg == "--user-shader") {
            user_shader = value();
            cli_preferences |= UserShader;
        } else if (arg == "--headless") headless = true;
        else if (arg == "--no-audio") sound = false;
        else if (arg == "--translated") translated = true;
        else if (arg == "--allow-fallback") allow_fallback = true;
        else if (arg == "--unthrottled") throttle = false;
        else if (arg == "--netplay-auto") netplay_auto = value();
        else if (arg == "--netplay-server") netplay_server_cli = value();
        else if (arg == "--netplay-name") netplay_name_cli = value();
        else if (arg == "--netplay-delay") netplay_delay_cli = uint8_t(std::stoul(value()));
        else if (arg == "--screenshot-frame") {
            std::string list = value();
            for (size_t start = 0; start <= list.size();) {
                const size_t comma = std::min(list.find(',', start), list.size());
                screenshot_frames.push_back(std::stoull(list.substr(start, comma - start)));
                start = comma + 1;
            }
        } else if (arg == "--open-f1-frame") open_f1_frame = std::stoull(value());
        else if (arg == "--chat-open-frame") chat_open_frame = std::stoull(value());
        else if (arg == "--chat-close-frame") chat_close_frame = std::stoull(value());
        else if (arg == "--chat-send-frame") {
            std::string val = value();
            auto colon = val.find(':');
            if (colon != std::string::npos) {
                uint64_t fr = std::stoull(val.substr(0, colon));
                chat_send_frames.push_back({fr, val.substr(colon + 1)});
            }
        } else if (arg == "--help") {
            std::cout
                << argv[0]
                << " [--rom-dir DIR] [--set SET] [--frames N] [--headless] [--no-audio]\n"
                << "  [--translated] [--allow-fallback (diagnostic only)] [--unthrottled] "
                   "[--eeprom FILE] [--wav FILE] [--surface BMP]\n"
                << "  [--fast-boot on|off] [--boot-cache on|off] (host-only boot shortcut; default "
                   "fast boot on, cache off)\n"
                << "  [--dump-dir DIR --dump-start N --dump-every N] [--fallback-report TSV]\n"
                << "  [--config FILE] [--volume 0..100] (user preferences load first; CLI "
                   "overrides)\n"
                << "  [--profile-out FILE] (instrumented build: merged entry counts, atomic flush "
                   "every 30s and at exit)\n"
                << "  [--discovery-log FILE] (host-only: log unhandled video writers/fallbacks "
                   "seen during play; see docs)\n"
                << "  [--discovery-all] (with --discovery-log: also log known writers (known=1) "
                   "and unit-accounted sprite writes)\n"
                << "  [--inputs FILE] (host-only script: buttons OR'd with live input, main-RAM "
                   "pokes; format in runtime/debug/input_script.hpp; disables fast boot)\n"
                << "  [--watch-entries HEX,HEX --watch-log FILE] (instrumented build with "
                   "--profile-out: per-frame execution counts of those main-CPU instructions)\n"
                << "  [--indirect-log FILE] (instrumented build with --profile-out: log observed "
                   "targets of computed jmp/jsr sites)\n"
                << "  [--sound-trace FILE] (reference audio only)\n"
                << "  [--audio-backend enhanced|reference] (default enhanced; reference runs "
                   "cycle-accurate chips)\n"
                << "  [--renderer accurate|enhanced] (accurate: MAME-derived reference renderer; "
                   "enhanced: game-data GPU renderer, needs strict native execution and "
                   "games/<game>/video/)\n"
                << "  [--renderer game-cpu|compare-cpu|compare-gpu] (developer-only: game-data CPU "
                   "compositor; compare also checks the FDP reference every supported frame)\n"
                << "  [--video-scale 1..4|auto|auto-integer] [--video-border 0..160] "
                   "[--video-filter nearest|linear]\n"
                << "  [--video-interp off|linear|fit] (opt-in GPU line sampling; default off)\n"
                << "  [--video-interp-fields none|geometry|palette|geometry,palette] (default "
                   "geometry; native alpha stays discrete)\n"
                << "  [--motion-interp] (experimental GPU temporal motion; default off, one-frame "
                   "positional latency)\n"
                << "  [--postprocess off|crt|user] [--user-shader FILE.metal|FILE.spv] (GPU only, "
                   "default off)\n"
                << "  [--netplay-server HOST:PORT] (netplay relay server, default 127.0.0.1:7845)\n"
                << "  [--netplay-auto host|join:CODE] (automate 2P versus netplay trigger; host, "
                   "host:file:PATH, join:CODE, join:file:PATH)\n"
                << "  [--netplay-name NAME] [--netplay-delay 1..8] (player name and frame delay)\n"
                << "  Presentation options require game/compare; defaults: scale 1, border 0, "
                   "nearest.\n"
                << "  Auto scales follow window pixels (GPU only); auto-integer uses nearest "
                   "filtering.\n"
                << "Arrows: move/dial; Z/X/C: buttons 1/2/3; A/S/D: buttons 4/5/6.\n"
                << "1/2/3/4: local player start; 5/6/7/8: coin; F3: service; F2: test.\n"
                << "Players 3/4 and buttons 4/5/6 are offline only; other P3/P4 controls default "
                   "to unbound.\n"
                << "F1: menu (pauses solo); F12: screenshot; Escape: close menu or quit.\n"
                << "F11 or Alt+Enter: toggle fullscreen. Menu includes persisted keyboard/gamepad "
                   "remapping.\n";
            return 0;
        } else throw std::runtime_error("Unknown argument: " + arg);
    }
    if (romdir.empty() || !dump_every)
        throw std::runtime_error("--rom-dir required; --dump-every must be positive");
    if (headless && !frames) throw std::runtime_error("Headless execution requires --frames");
    f3rt::FrontendSettings settings;
#if defined(F3RT_GAME_VIDEO) && defined(F3RT_GPU)
    // Enhanced needs game-data video (strict native execution) and a GPU-enabled build; otherwise accurate.
    if (translated && !allow_fallback) settings.renderer = "enhanced";
#endif
    if (config_path.empty()) config_path = f3rt::default_config_path(set);
    std::string settings_error;
    if (!config_path.empty() &&
        !f3rt::load_frontend_settings(config_path.string(), settings, settings_error))
        std::cerr << "f3rt: ignoring config " << config_path << ": " << settings_error << '\n';
    auto preference = [&](auto &option, auto &saved, Preference field) {
        if (cli_preferences & field) saved = option;
        else option = saved;
    };
    // Developer renderers exist only on the command line; the settings file and UI hold accurate|enhanced.
    if (cli_preferences & Renderer) {
        if (renderer == "accurate" || renderer == "enhanced") settings.renderer = renderer;
    } else renderer = settings.renderer;
    std::string video_mode, video_backend;
    auto select_renderer = [&] {
        if (renderer == "accurate") {
            video_mode = "fdp";
            video_backend = "cpu";
        } else if (renderer == "enhanced") {
            video_mode = "game";
            video_backend = "gpu";
        } else if (renderer == "game-cpu") {
            video_mode = "game";
            video_backend = "cpu";
        } else if (renderer == "compare-cpu") {
            video_mode = "compare";
            video_backend = "cpu";
        } else if (renderer == "compare-gpu") {
            video_mode = "compare";
            video_backend = "gpu";
        } else
            throw std::runtime_error(
                "--renderer must be accurate, enhanced, game-cpu, compare-cpu or compare-gpu");
    };
    select_renderer();
#ifdef F3RT_GPU
    // A renderer that came from the default or the settings file must not leave the user unable to launch:
    // probe the GPU (hidden window + device + window claim) and fall back to accurate. An explicit
    // --renderer keeps failing hard in the normal start-up path.
    if (!(cli_preferences & Renderer) && !headless && video_backend == "gpu") {
        std::string reason;
        if (!SDL_Init(SDL_INIT_VIDEO)) {
            reason = SDL_GetError();
        } else {
            if (auto *probe = SDL_CreateWindow("f3rt gpu probe", 64, 64, SDL_WINDOW_HIDDEN)) {
                if (auto *device =
                        SDL_CreateGPUDevice(SDL_GPU_SHADERFORMAT_SPIRV | SDL_GPU_SHADERFORMAT_MSL |
                                                SDL_GPU_SHADERFORMAT_METALLIB,
                                            false, nullptr)) {
                    if (!SDL_ClaimWindowForGPUDevice(device, probe)) reason = SDL_GetError();
                    else SDL_ReleaseWindowFromGPUDevice(device, probe);
                    SDL_DestroyGPUDevice(device);
                } else {
                    reason = SDL_GetError();
                }
                SDL_DestroyWindow(probe);
            } else {
                reason = SDL_GetError();
            }
        }
        if (!reason.empty()) {
            std::cerr << "f3rt: warning: GPU renderer unavailable (" << reason
                      << "); falling back to --renderer accurate. "
                         "Pass --renderer enhanced to make this an error.\n";
            renderer = "accurate";
            select_renderer();
        }
    }
#endif
    preference(video_scale, settings.video_scale, Scale);
    preference(video_options.border, settings.border, Border);
    preference(video_filter, settings.filter, Filter);
    preference(video_interp, settings.interpolation, Interpolation);
    preference(video_interp_fields, settings.interpolation_fields, Fields);
    preference(postprocess, settings.postprocess, PostprocessMode);
    preference(user_shader, settings.user_shader, UserShader);
    preference(volume, settings.volume, Volume);
    preference(fast_boot, settings.fast_boot, FastBoot);
    preference(boot_cache, settings.boot_cache, BootCache);
    preference(motion_interp, settings.motion_interp, MotionInterp);
    if (!netplay_server_cli.empty()) settings.netplay_server = netplay_server_cli;
    if (!netplay_name_cli.empty()) settings.netplay_name = netplay_name_cli;
    if (netplay_delay_cli != 0) settings.netplay_delay = netplay_delay_cli;
    // Presentation options only exist for the game-data renderers. Saved values are ignored under accurate
    // (and stay in the settings file for enhanced); explicit command-line options are rejected below.
    if (video_mode == "fdp") {
        if (!(cli_preferences & Scale)) video_scale = "1";
        if (!(cli_preferences & Border)) video_options.border = 0;
        if (!(cli_preferences & Filter)) video_filter = "nearest";
        if (!(cli_preferences & Interpolation)) video_interp = "off";
        if (!(cli_preferences & Fields)) video_interp_fields = "geometry";
        if (!(cli_preferences & MotionInterp)) motion_interp = false;
    } else if (video_backend != "gpu") {
        if (!(cli_preferences & Scale)) video_scale = "1";
        if (!(cli_preferences & Interpolation)) video_interp = "off";
        if (!(cli_preferences & MotionInterp)) motion_interp = false;
    }
    if (cli_preferences & SoundBackend) {
        f3rt::Audio::Backend parsed;
        if (!f3rt::Audio::parse_backend(audio_backend, parsed))
            throw std::runtime_error("--audio-backend must be enhanced or reference");
        settings.audio_backend = parsed == f3rt::Audio::Backend::Enhanced
                                     ? f3rt::AudioBackend::Enhanced
                                     : f3rt::AudioBackend::Reference;
    }
    parse_scale(video_scale, video_scale_mode, video_options);
#ifdef F3RT_GAME
    if (set != F3RT_DEFAULT_SET)
        throw std::runtime_error("This generated executable requires " F3RT_DEFAULT_SET);
#endif
#ifndef F3RT_GAME_VIDEO
    if (video_mode != "fdp")
        throw std::runtime_error("Game-data video is unavailable for this game");
#else
    if (video_mode != "fdp" && (!translated || allow_fallback))
        throw std::runtime_error("Game-data video requires strict native execution");
#endif
    if (video_filter != "nearest" && video_filter != "linear")
        throw std::runtime_error("--video-filter must be nearest or linear");
    if (video_mode == "fdp" && (video_options.expanded() || video_filter != "nearest"))
        throw std::runtime_error("Scale, border and filter options require --renderer enhanced (or "
                                 "a developer game/compare renderer)");
    bool automatic_scale = video_scale_mode != f3rt::VideoScaleMode::Fixed;
    if (automatic_scale && video_backend != "gpu")
        throw std::runtime_error(
            "--video-scale auto/auto-integer requires --renderer enhanced or compare-gpu");
    if (video_interp != "off" && video_interp != "linear" && video_interp != "fit")
        throw std::runtime_error("--video-interp must be off, linear or fit");
    if (video_interp != "off" && video_backend != "gpu")
        throw std::runtime_error("--video-interp requires --renderer enhanced or compare-gpu");
    if (motion_interp && video_backend != "gpu")
        throw std::runtime_error("--motion-interp requires --renderer enhanced or compare-gpu");
    if (motion_interp && allow_fallback)
        throw std::runtime_error(
            "--motion-interp requires a strict-native title build with no interpreter fallback "
            "(--allow-fallback, and the f3rt-run default, are refused)");
    if (postprocess != "off" && postprocess != "crt" && postprocess != "user")
        throw std::runtime_error("--postprocess must be off, crt or user");
    if (postprocess != "off" && video_backend != "gpu")
        std::cerr
            << "f3rt: post-processing requires the enhanced renderer; preference is inactive\n";
    const auto interpolation_fields = f3rt::parse_interpolation_fields(video_interp_fields);
    if (!interpolation_fields)
        throw std::runtime_error(
            "--video-interp-fields must be none, geometry, palette or geometry,palette");
#ifndef F3RT_GPU
    if (video_backend == "gpu" && !headless)
        throw std::runtime_error("The GPU renderer requires F3RT_GPU build support");
#endif
    if (watch_entries.empty() != watch_path.empty())
        throw std::runtime_error("--watch-entries and --watch-log go together");
    std::optional<f3rt::InputScript> input_script;
    if (!inputs_path.empty()) {
        auto loaded = f3rt::InputScript::load(inputs_path.string());
        if (loaded.has_pokes() && (!netplay_auto.empty() || f3rt::game_netplay()))
            throw std::runtime_error(
                "Input scripts with memory poke rules cannot be used when netplay is enabled: " +
                inputs_path.string());
        input_script = std::move(loaded);
    }
    auto machine = std::make_unique<f3rt::Machine>(f3rt::RomSet::load(romdir, set));
    auto &m = *machine;
    f3rt::BlockProfileSession profile(m.roms, profile_path);
    std::unique_ptr<f3rt::EntryWatch> entry_watch;
    if (!watch_path.empty())
        entry_watch = std::make_unique<f3rt::EntryWatch>(watch_path, profile, watch_entries);
    std::unique_ptr<f3rt::IndirectWatch> indirect_watch;
    if (!indirect_path.empty())
        indirect_watch = std::make_unique<f3rt::IndirectWatch>(indirect_path, profile);
#ifdef F3RT_SOUND_GENERATED
    if (set == F3RT_DEFAULT_SET) {
        m.use_native_sound(f3_sound_blocks, f3_sound_block_count,
                           {f3_sound_excluded_ranges, f3_sound_excluded_count}, f3_sound_rom_crc32);
    }
#endif
    if (settings.audio_backend == f3rt::AudioBackend::Enhanced) {
        if (!m.audio->backend_available(f3rt::Audio::Backend::Enhanced)) {
            if (cli_preferences & SoundBackend)
                throw std::runtime_error("Enhanced audio is not supported for this game");
            settings.audio_backend = f3rt::AudioBackend::Reference;
        }
    }
    if (settings.audio_backend == f3rt::AudioBackend::Enhanced)
        m.audio->set_backend(f3rt::Audio::Backend::Enhanced);
    else m.audio->set_backend(f3rt::Audio::Backend::Reference);
    audio_backend = f3rt::audio_backend_name(settings.audio_backend);
#ifdef F3_PROFILE_SLIM_ENABLED
    if (allow_fallback || !translated ||
        (settings.audio_backend == f3rt::AudioBackend::Reference && !m.sound_native))
        throw std::runtime_error(
            "Profile-slim requires strict native main and sound CPUs; no interpreter fallback");
#endif
#ifdef F3_PROFILE_INSTRUMENT
    if (!profile_path.empty() &&
        (!translated || allow_fallback || settings.audio_backend != f3rt::AudioBackend::Reference ||
         !m.sound_native))
        throw std::runtime_error("Profiling requires strict native main and sound CPUs");
#endif
    m.allow_main_fallback = allow_fallback;
#ifdef F3RT_GAME_VIDEO
    if (video_mode != "fdp")
        m.game_video = std::make_unique<f3rt::GameVideo>(
            m, video_mode == "game" ? f3rt::GameVideoMode::Game : f3rt::GameVideoMode::Compare,
            video_options);
#endif
    {
        const auto &unit_table = f3rt::SpriteUnits::game_table();
        if (translated && !unit_table.units.empty()) {
            f3rt::SpriteUnits::Options options;
            const auto registered = f3rt::registered_sprite_behaviours();
            options.behaviours.assign(registered.begin(), registered.end());
            const auto shadows = f3rt::registered_flicker_shadows();
            options.flicker_shadows.assign(shadows.begin(), shadows.end());
            m.sprite_units = std::make_unique<f3rt::SpriteUnits>(m, unit_table, std::move(options));
            // Always on with the GPU presenter, which picks flicker shadow visibility per presented frame;
            // CPU and FDP output keeps the game's own alternate-frame emission.
            m.sprite_units->set_flicker_shadows(video_backend == "gpu" && m.game_video);
        }
    }
    if (!dumpdir.empty()) {
        m.sprite_writers = std::make_unique<std::array<uint32_t, 0x1000>>();
    }
    if (!discovery_path.empty()) {
        m.discovery = std::make_unique<f3rt::DiscoveryLog>(
            discovery_path.string(), m,
            "set=" + set + " renderer=" + (renderer.empty() ? "default" : renderer) +
                " exec=" + (translated ? "native" : "interpreted") + " emit_units=" +
                (m.sprite_units ? "yes" : "no") + " game_video=" + (m.game_video ? "yes" : "no"));
        m.discovery->set_sprites_accounted_by_units(m.sprite_units != nullptr);
        m.discovery->set_log_all(discovery_all);
        if (m.sprite_units) m.sprite_units->set_discovery(m.discovery.get());
    }
    const bool shadows_active = m.sprite_units && m.sprite_units->flicker_shadows_enabled();
    const auto snapshot_video_options = video_options;
    const unsigned frame_width = video_options.width();
    const unsigned frame_height =
        m.game_video ? video_options.height() : m.roms.video.visible_height;
    const unsigned rotation = m.roms.video.rotation;
    const bool portrait = rotation == 90 || rotation == 270;
    const unsigned display_width = portrait ? frame_height : frame_width;
    const unsigned display_height = portrait ? frame_width : frame_height;
    const SDL_FRect rotated_rect{(float(display_width) - float(frame_width)) / 2,
                                 (float(display_height) - float(frame_height)) / 2,
                                 float(frame_width), float(frame_height)};
    if (!eeprom.empty()) m.load_eeprom(eeprom);
    if (!fallback_report.empty()) m.fallback_hits.resize(0x800000);
    if (translated) {
#ifdef F3RT_GENERATED
        if (!f3_generated_register(&m.cpu))
            throw std::runtime_error("Generated block registration failed");
#else
        throw std::runtime_error("This binary was built without F3_GENERATED_DIR");
#endif
    }
    Sdl sdl;
    int pixel_width = 0, pixel_height = 0;
    uint32_t audio_rate = m.audio->sample_rate();
    int audio_target_bytes = 0; // steady-state SDL queue depth; set when the device opens
    if (!headless) {
        check(SDL_Init(SDL_INIT_VIDEO | SDL_INIT_GAMEPAD | (sound ? SDL_INIT_AUDIO : 0)));
#ifdef F3RT_GPU
        if (video_backend == "gpu") {
            const SDL_WindowFlags flags =
                SDL_WINDOW_RESIZABLE | (automatic_scale ? SDL_WINDOW_HIGH_PIXEL_DENSITY : 0);
            sdl.window = SDL_CreateWindow(("f3rt — " + set).c_str(),
                                          int((320 + video_options.border * 2) * 3), 696, flags);
            check(sdl.window != nullptr);
            m.game_video->enable_gpu_presentation();
            if (automatic_scale) {
                int pixel_width = 0, pixel_height = 0;
                check(SDL_GetWindowSizeInPixels(sdl.window, &pixel_width, &pixel_height));
                video_options.scale = f3rt::video_scale_for_window(
                    video_scale_mode, unsigned(std::max(pixel_width, 0)),
                    unsigned(std::max(pixel_height, 0)), video_options.border);
            }
            m.game_video->set_gpu_scale(video_options.scale);
            sdl.gpu = std::make_unique<f3rt::GpuVideo>(
                sdl.window, video_options, m.video->playfield_tiles(), m.video->sprite_tiles(),
                video_filter == "linear", throttle, parse_interpolation_mode(video_interp),
                *interpolation_fields);
            sdl.gpu->set_scale_mode(video_scale_mode);
            if (postprocess != "off")
                sdl.gpu->set_postprocess(postprocess == "crt" ? f3rt::Postprocess::Crt
                                                              : f3rt::Postprocess::User,
                                         user_shader);
        } else
#endif
        {
            check(SDL_CreateWindowAndRenderer(("f3rt — " + set).c_str(), int(display_width * 3),
                                              int(display_height * 3), SDL_WINDOW_RESIZABLE,
                                              &sdl.window, &sdl.renderer));
            check(SDL_SetRenderLogicalPresentation(sdl.renderer, int(display_width),
                                                   int(display_height),
                                                   SDL_LOGICAL_PRESENTATION_LETTERBOX));
            sdl.texture =
                SDL_CreateTexture(sdl.renderer, SDL_PIXELFORMAT_ARGB8888,
                                  SDL_TEXTUREACCESS_STREAMING, int(frame_width), int(frame_height));
            check(sdl.texture != nullptr);
            check(SDL_SetTextureScaleMode(sdl.texture, video_filter == "linear"
                                                           ? SDL_SCALEMODE_LINEAR
                                                           : SDL_SCALEMODE_NEAREST));
        }
        if (sound) {
            SDL_SetHint(SDL_HINT_AUDIO_DEVICE_SAMPLE_FRAMES, "512");
            SDL_AudioSpec spec{SDL_AUDIO_S16, 2, int(audio_rate)};
            sdl.audio = SDL_OpenAudioDeviceStream(SDL_AUDIO_DEVICE_DEFAULT_PLAYBACK, &spec, nullptr,
                                                  nullptr);
            check(sdl.audio != nullptr);
            // The device pulls whole periods while frames arrive every 1/59 s; the queue must hold a period plus a frame
            // at the worst phase of those two clocks, and one more period absorbs scheduling jitter.
            SDL_AudioSpec device_spec;
            int device_frames = 0;
            check(SDL_GetAudioDeviceFormat(SDL_GetAudioStreamDevice(sdl.audio), &device_spec,
                                           &device_frames));
            const uint64_t period_frames =
                uint64_t(device_frames) * audio_rate / uint64_t(device_spec.freq);
            const uint64_t emulated_frame_frames = uint64_t(audio_rate) *
                                                   f3rt::Machine::FRAME_PIXELS /
                                                   f3rt::Machine::PIXEL_CLOCK;
            audio_target_bytes = int((2 * period_frames + emulated_frame_frames) * 4);
            check(SDL_ResumeAudioStreamDevice(sdl.audio));
            check(SDL_SetAudioStreamGain(sdl.audio, volume));
        }
        check(SDL_GetWindowSizeInPixels(sdl.window, &pixel_width, &pixel_height));
        sdl.input = std::make_unique<f3rt::InputMapper>(settings);
        SDL_GPUDevice *ui_device = nullptr;
#ifdef F3RT_GPU
        if (sdl.gpu) ui_device = sdl.gpu->device();
#endif
        sdl.ui = std::make_unique<f3rt::FrontendUi>(sdl.window, sdl.renderer, ui_device, settings,
                                                    *sdl.input);
        std::cout << "window_open video_driver=" << SDL_GetCurrentVideoDriver()
                  << " renderer=" << renderer << " internal=" << display_width << 'x'
                  << display_height << " pixels=" << pixel_width << 'x' << pixel_height
                  << " scale=" << video_options.scale << " filter=" << video_filter
                  << " interp=" << video_interp
                  << " interp_fields=" << f3rt::interpolation_fields_name(*interpolation_fields)
                  << '\n';
        if (motion_interp) {
            const auto *display = SDL_GetCurrentDisplayMode(SDL_GetDisplayForWindow(sdl.window));
            std::cout << "motion_interp=on display_hz=" << (display ? display->refresh_rate : 0)
                      << " emulation_hz="
                      << double(f3rt::Machine::PIXEL_CLOCK) / f3rt::Machine::FRAME_PIXELS
                      << " temporal_pacing=" << (throttle ? "display" : "unthrottled-current")
                      << '\n';
        }
    }
    std::unique_ptr<f3rt::WavWriter> wav;
    if (!wav_path.empty()) wav = std::make_unique<f3rt::WavWriter>(wav_path, audio_rate);
    std::array<int16_t, 8192> samples{};
    uint64_t audio_frames = 0, nonzero_samples = 0;
    int audio_peak = 0;
    bool quit = false;
    f3rt::FrontendUiState ui_state;
#ifdef F3RT_GPU
    ui_state.gpu_available = true;
    if (sdl.gpu) ui_state.active_postprocess = postprocess;
#endif
    ui_state.active_renderer = renderer;
    ui_state.set = set;
    ui_state.enhanced_audio_available = m.audio->backend_available(f3rt::Audio::Backend::Enhanced);
    ui_state.message = settings_error;
    const auto *netplay_hook = f3rt::game_netplay();
    ui_state.netplay_supported = (netplay_hook != nullptr && netplay_hook->in_versus != nullptr);
    ui_state.netplay_has_state_sync =
        (netplay_hook != nullptr && netplay_hook->capture_sync != nullptr &&
         netplay_hook->apply_sync != nullptr);
    uint64_t executed_frames = 0;
    const auto frame_time = std::chrono::nanoseconds(
        uint64_t(1e9 * f3rt::Machine::FRAME_PIXELS / f3rt::Machine::PIXEL_CLOCK));
    constexpr int MAX_AUDIO_QUEUE_MS = 50, MAX_CATCHUP_MS = 50;
    uint64_t audio_queue_drops = 0, clock_resyncs = 0, audio_queue_sum = 0, audio_queue_samples = 0,
             audio_queue_max = 0;
    double audio_queue_average = audio_target_bytes;
    std::vector<int16_t> audio_silence;
    const auto start = std::chrono::steady_clock::now();
    // Fast boot: run the power-on frames unthrottled with no presentation and discarded
    // audio, from a locally generated initialised EEPROM and optionally a cached
    // post-boot state. Host-side only; the emulated machine executes the same frames.
    const f3rt::GameBoot *boot = f3rt::game_boot();
    const bool explicit_fast_boot = (cli_preferences & FastBoot) != 0 && fast_boot;
    const bool fast_boot_active =
        boot && ((!headless && settings.fast_boot) || explicit_fast_boot) && wav_path.empty() &&
        dumpdir.empty() && profile_path.empty() && fallback_report.empty() && inputs_path.empty();
    std::filesystem::path config_dir;
    if (!config_path.empty()) config_dir = std::filesystem::absolute(config_path).parent_path();
    bool eeprom_enabled = false, eeprom_loaded = false, eeprom_generated = false;
    std::filesystem::path eeprom_init_path;
    if (fast_boot_active && eeprom.empty() && m.roms.factory_eeprom.empty() &&
        !config_dir.empty()) {
        eeprom_enabled = true;
        eeprom_init_path = config_dir / "eeprom" / (set + "-init.bin");
        if (std::filesystem::exists(eeprom_init_path)) {
            try {
                m.load_eeprom(eeprom_init_path);
                eeprom_loaded = true;
            } catch (const std::exception &error) {
                std::cerr << "f3rt: ignoring initialised EEPROM " << eeprom_init_path << ": "
                          << error.what() << '\n';
            }
        }
    }
    std::filesystem::path boot_state_path;
    bool cache_loaded = false;
    if (fast_boot_active && settings.boot_cache && eeprom.empty() && !config_dir.empty()) {
        boot_state_path = config_dir / "boot" / (set + ".state");
        if (std::filesystem::exists(boot_state_path)) {
            try {
                f3rt::frontend_state_slot(m, boot_state_path, snapshot_video_options.scale,
                                          snapshot_video_options.border, false);
                cache_loaded = true;
            } catch (const std::exception &error) {
                std::cerr << "f3rt: boot cache unusable, using turbo boot: " << error.what()
                          << '\n';
            }
        }
    }
    if (cache_loaded) {
        if (sdl.input) sdl.input->release();
        if (sdl.audio) check(SDL_ClearAudioStream(sdl.audio));
        executed_frames = m.frame;
    }
    const char *boot_source = cache_loaded ? "cache" : "turbo";
    uint64_t turbo_frames = 0;
    double turbo_ms = 0;
    bool turbo_complete = false;
    if (fast_boot_active && !cache_loaded) {
        const auto turbo_start = std::chrono::steady_clock::now();
        while (!quit && (frames == 0 || executed_frames < frames) &&
               turbo_frames < boot->frame_limit) {
            if (boot->complete(m)) {
                turbo_complete = true;
                break;
            }
            f3rt::apply_local_inputs(m, {});
            if (!m.run_frame(translated))
                throw std::runtime_error("CPU halted at " + std::to_string(m.cpu.pc));
            ++executed_frames;
            ++turbo_frames;
            size_t count;
            while ((count = m.audio->render(samples.data(), samples.size() / 2)) != 0) {
                audio_frames += count;
                for (size_t i = 0; i < count * 2; ++i) {
                    audio_peak = std::max(audio_peak, std::abs(int(samples[i])));
                    nonzero_samples += samples[i] != 0;
                }
            }
            if (!headless && turbo_frames % 30 == 0) {
                SDL_PumpEvents();
                SDL_Event event;
                while (SDL_PollEvent(&event))
                    if (event.type == SDL_EVENT_QUIT) quit = true;
            }
        }
        turbo_ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() -
                                                             turbo_start)
                       .count();
        if (!turbo_complete && turbo_frames >= boot->frame_limit)
            std::cerr << "f3rt: fast boot reached the " << boot->frame_limit
                      << "-frame limit without a completion signal; continuing normally\n";
    }
    // Only a completed boot has written the game's defaults; a quit or frame limit mid-boot must not persist.
    if (fast_boot_active && eeprom_enabled && !eeprom_loaded && turbo_complete) {
        std::error_code ec;
        std::filesystem::create_directories(eeprom_init_path.parent_path(), ec);
        m.save_eeprom(eeprom_init_path);
        eeprom_generated = true;
    }
    if (fast_boot_active && settings.boot_cache && eeprom.empty() && !config_dir.empty() &&
        !cache_loaded && turbo_complete) {
        try {
            f3rt::frontend_state_slot(m, boot_state_path, snapshot_video_options.scale,
                                      snapshot_video_options.border, true);
        } catch (const std::exception &error) {
            std::cerr << "f3rt: cannot write boot cache " << boot_state_path << ": " << error.what()
                      << '\n';
        }
    }
    if (fast_boot_active)
        std::cout << "fast_boot frames=" << turbo_frames << " ms=" << turbo_ms
                  << " source=" << boot_source << " eeprom="
                  << (!eeprom_enabled    ? "none"
                      : eeprom_generated ? "generated"
                      : eeprom_loaded    ? "loaded"
                                         : "none")
                  << '\n';
    auto handle_audio_chunk = [&](size_t count) {
        audio_frames += count;
        for (size_t i = 0; i < count * 2; ++i) {
            audio_peak = std::max(audio_peak, std::abs(int(samples[i])));
            nonzero_samples += samples[i] != 0;
        }
        if (wav) wav->append(std::span(samples.data(), count * 2));
        if (sdl.audio) {
            int backlog = SDL_GetAudioStreamQueued(sdl.audio);
            check(backlog >= 0);
            if (throttle) {
                // A stalled window (tab-out, fullscreen switch) must not leave seconds of audio queued ahead of the
                // picture. Past the latency cap, drop the stale backlog so new sound plays now.
                if (backlog > audio_target_bytes + int(audio_rate * 4 * MAX_AUDIO_QUEUE_MS / 1000)) {
                    check(SDL_ClearAudioStream(sdl.audio));
                    ++audio_queue_drops;
                    backlog = 0;
                }
                // An empty queue (start, after a drop or an underrun) is already silent; lead the chunk with
                // silence up to the target instead of letting the device run dry on every period until it refills.
                const int chunk_bytes = int(count * 4);
                if (backlog == 0) audio_queue_average = audio_target_bytes;
                if (backlog == 0 && chunk_bytes < audio_target_bytes) {
                    audio_silence.assign(size_t(audio_target_bytes - chunk_bytes) / 2, 0);
                    check(SDL_PutAudioStreamData(sdl.audio, audio_silence.data(),
                                                 audio_target_bytes - chunk_bytes));
                }
            }
            check(SDL_PutAudioStreamData(sdl.audio, samples.data(), int(count * 4)));
            const uint64_t queued = uint64_t(SDL_GetAudioStreamQueued(sdl.audio));
            if (throttle) {
                // Frames are paced by the system clock and consumed by the device clock; hold the queue at the target
                // by nudging the playback rate (at most 0.5%, inaudible) against a ~1 s average of its depth.
                audio_queue_average += (double(queued) - audio_queue_average) / 64.0;
                const double error = std::clamp(
                    (audio_queue_average - audio_target_bytes) / audio_target_bytes, -1.0, 1.0);
                check(SDL_SetAudioStreamFrequencyRatio(sdl.audio, float(1.0 + 0.005 * error)));
            }
            audio_queue_sum += queued;
            ++audio_queue_samples;
            audio_queue_max = std::max(audio_queue_max, queued);
        }
    };
    const bool slots_supported = translated && !allow_fallback;
    bool motion_presentation = motion_interp && !headless;
    uint64_t motion_presentations = 0, motion_intermediates = 0, motion_history_resets = 0,
             scale_changes = 0;
    float motion_min_alpha = 1, motion_max_alpha = 0;
    // Emulation side. Everything below up to `emulate` runs on the emulation thread (or on main when headless);
    // the video thread never touches `m`, `samples` or the audio statistics until that thread is joined.
    Link link;
#ifdef F3RT_GPU
    std::array<std::unique_ptr<f3rt::CapturedFrame>, Link::SLOT_COUNT> capture_buffers;
    if (!headless && sdl.gpu)
        for (size_t i = 0; i < capture_buffers.size(); ++i)
            link.captures[i] = (capture_buffers[i] = std::make_unique<f3rt::CapturedFrame>()).get();
#endif
    unsigned emu_scale = video_options.scale;
    uint64_t epoch = 0;
    bool menu_paused = false, halted = false;
    const bool pace = !headless && throttle;
    auto next_frame = std::chrono::steady_clock::now();
    // Netplay (lobby, session, match, recorder) lives entirely on this thread, because the session callbacks run the
    // machine. The video thread sees it through NetStatus and drives it with the Net* commands.
    const bool has_hold = netplay_hook && netplay_hook->lobby_hold_frame;
    const NetSettings auto_net{settings.netplay_server, settings.netplay_name,
                               settings.netplay_delay};
    NetStatus net, net_sent;
    bool prev_in_versus = false, netplay_lobby_active = false, netplay_state_sync_done = false;
    std::vector<uint8_t> netplay_trigger_snapshot;
    f3rt::netplay::MatchMeta netplay_meta;
    std::unique_ptr<f3rt::netplay::MatchRecorder> netplay_recorder;
    std::chrono::steady_clock::time_point netplay_trigger_time;
    std::unique_ptr<f3rt::netplay::Session> netplay_session;
    std::string netplay_own_name, logged_room;
    uint32_t netplay_match_frame = 0, next_netplay_local_frame = 0;
    auto fill_snapshot = [&](Snapshot &s, std::chrono::steady_clock::time_point scheduled) {
        s.frame = m.frame;
        s.epoch = epoch;
        s.scale = emu_scale;
        s.start = scheduled;
        if (!s.gpu) {
            const auto pixels = m.game_video ? m.game_video->presentation()
                                             : std::span<const uint32_t>(m.native_pixels());
            s.pixels.assign(pixels.begin(), pixels.end());
        }
        link.frame.store(m.frame, std::memory_order_relaxed);
    };
    // A slot (and GPU capture buffer) the presenter cannot be reading, installed as the capture target. Anything that
    // redraws the capture (frames, state loads) must start here, or it rewrites the buffer the presenter holds.
    auto begin_unpublished = [&]() -> Snapshot * {
        if (headless) return nullptr;
        Snapshot *slot = &link.begin_frame();
        if (slot->gpu) m.game_video->set_capture_target(slot->gpu);
        return slot;
    };
    // After a state load or sync: the picture and clock jump, so the video thread resets motion history.
    auto publish_jump = [&](Snapshot *slot) {
        ++epoch;
        next_frame = std::chrono::steady_clock::now();
        if (slot) {
            fill_snapshot(*slot, next_frame);
            link.publish(false);
        }
    };
    // Restores the lobby-trigger snapshot and runs input-0 local frames to the in_versus edge: the exact frame
    // capture_sync/apply_sync fire on. No lobby_hold_frame / on_match_frame here (identical on both peers).
    auto sync_to_edge = [&]() {
        netplay_meta.lobby_hold_frames = m.frame - netplay_meta.trigger_machine_frame;
        netplay_meta.lobby_hold_ms = std::chrono::duration<double, std::milli>(
                                         std::chrono::steady_clock::now() - netplay_trigger_time)
                                         .count();
        Snapshot *slot = begin_unpublished();
        if (!netplay_trigger_snapshot.empty()) m.load_state(netplay_trigger_snapshot);
        while (netplay_hook->in_versus && !netplay_hook->in_versus(m)) {
            f3rt::apply_local_inputs(m, {});
            m.run_frame(translated);
        }
        netplay_meta.edge_machine_frame = netplay_meta.sync_machine_frame = m.frame;
        // From here until GO the machine must not run a single frame (see `halted` in the loop).
        netplay_state_sync_done = true;
        publish_jump(slot);
    };
    auto start_session = [&](bool host, const NetSettings &net_settings, const std::string &code) {
        if (input_script && input_script->has_pokes())
            throw std::runtime_error("Input scripts with RAM pokes cannot be used in netplay");
        std::string room = code;
        if (!host && room.starts_with("file:")) {
            const std::string p = room.substr(5);
            for (int i = 0; i < 50; ++i) {
                if (std::filesystem::exists(p)) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
            std::ifstream in(p);
            in >> room;
        }
        f3rt::netplay::SessionConfig cfg;
        std::string server = net_settings.server;
        uint16_t port = f3rt::netplay::DEFAULT_PORT;
        const auto colon = server.rfind(':');
        if (colon != std::string::npos) {
            port = static_cast<uint16_t>(std::stoul(server.substr(colon + 1)));
            server = server.substr(0, colon);
        }
        cfg.server_host = server;
        cfg.server_port = port;
        cfg.game_id = std::string(f3rt::netplay::default_game_id);
        cfg.rom_crcs = f3rt::rom_region_crcs(m.roms);
        cfg.build_hash = f3rt::netplay::build_hash;
        cfg.player_name = net_settings.name;
        cfg.is_host = host;
        cfg.crc_interval = 60;
        if (host) {
            cfg.input_delay = net_settings.delay;
            if (netplay_hook && netplay_hook->capture_sync) {
                cfg.capture_sync = [&]() -> std::vector<uint8_t> {
                    sync_to_edge();
                    std::vector<uint8_t> buf(2048);
                    size_t n = netplay_hook->capture_sync(m, buf);
                    buf.resize(n);
                    return buf;
                };
            }
        } else {
            cfg.room_code = room;
            if (netplay_hook && netplay_hook->apply_sync) {
                cfg.apply_sync = [&](std::span<const uint8_t> blob, uint64_t seed) {
                    sync_to_edge();
                    netplay_hook->apply_sync(m, blob, seed);
                };
            }
        }
        if (netplay_hook && netplay_hook->sync_hash) {
            cfg.sync_hash = [&]() -> uint32_t { return netplay_hook->sync_hash(m); };
        }
        netplay_session = std::make_unique<f3rt::netplay::Session>(cfg);
        netplay_session->start();
        netplay_own_name = net_settings.name;
        netplay_state_sync_done = false;
        net.active = false;
        netplay_match_frame = 0;
        next_netplay_local_frame = 0;
        net.status = host ? "Connecting to server..." : "Connecting to room " + code + "...";
        net.error.clear();
        net.desync_path.clear();
        net.room_code = host ? std::string() : code;
    };
    // Leaves the lobby or session: back to the trigger snapshot; `cancel` also un-commits the game's menu selection.
    auto leave_netplay = [&](bool cancel) {
        if (netplay_session) netplay_session->close();
        netplay_session.reset();
        net.active = false;
        netplay_state_sync_done = false;
        netplay_lobby_active = false;
        if (!netplay_trigger_snapshot.empty()) {
            Snapshot *slot = begin_unpublished();
            m.load_state(netplay_trigger_snapshot);
            netplay_trigger_snapshot.clear();
            publish_jump(slot);
        }
        if (cancel && netplay_hook && netplay_hook->lobby_cancel) netplay_hook->lobby_cancel(m);
        prev_in_versus = false;
        net.lobby_open = false;
        net.status.clear();
        net.room_code.clear();
        net.error.clear();
    };
    auto apply = [&](const Command &c) {
        try {
            switch (c.kind) {
            case Command::Pause:
                menu_paused = true;
                break;
            case Command::Resume:
                menu_paused = false;
                break;
            case Command::SetScale:
                m.game_video->set_gpu_scale(c.scale);
                emu_scale = c.scale;
                if (halted) link.restate(emu_scale);
                break;
            case Command::Save:
            case Command::Load: {
                if (net.active)
                    throw std::runtime_error("Save/load states are disabled during active netplay");
                const bool save = c.kind == Command::Save;
                // The load rewrites the newest capture in place unless redirected to a buffer nobody else holds.
                Snapshot *target = save ? nullptr : begin_unpublished();
                f3rt::frontend_state_slot(m, c.path, snapshot_video_options.scale,
                                          snapshot_video_options.border, save);
                if (!save) {
                    if (sdl.audio) check(SDL_ClearAudioStream(sdl.audio));
                    publish_jump(target);
                }
                link.reply({std::string(save ? "Saved " : "Loaded ") + c.path.string(), !save});
                break;
            }
            case Command::NetHost:
                start_session(true, c.net, {});
                break;
            case Command::NetJoin:
                start_session(false, c.net, c.text);
                break;
            case Command::NetCancel:
                leave_netplay(true);
                break;
            case Command::NetLocal:
                leave_netplay(false);
                break;
            case Command::Chat:
                if (netplay_session) netplay_session->send_chat(c.text);
                break;
            }
        } catch (const std::exception &error) {
            link.reply({error.what()});
        }
    };
    auto stopping = [&] {
        std::lock_guard lock(link.mutex);
        return link.stop;
    };
    auto poll_session = [&] {
        netplay_session->poll();
        using SState = f3rt::netplay::SessionState;
        switch (netplay_session->state()) {
        case SState::Connecting:
            net.status = "Connecting to server...";
            break;
        case SState::Lobby:
            if (!netplay_session->room_code().empty()) {
                if (logged_room != netplay_session->room_code()) {
                    logged_room = netplay_session->room_code();
                    std::cout << "NETPLAY_ROOM " << logged_room << '\n' << std::flush;
                    if (netplay_auto.starts_with("host:file:")) {
                        std::ofstream out(netplay_auto.substr(10));
                        out << logged_room << '\n' << std::flush;
                    }
                }
                net.room_code = netplay_session->room_code();
            }
            net.status = "Waiting for peer...";
            break;
        case SState::Registering:
            net.status = "Peer matched! Registering UDP...";
            break;
        case SState::Running:
            if (!net.active) {
                net.active = true;
                net.lobby_open = false;
                netplay_recorder = std::make_unique<f3rt::netplay::MatchRecorder>(netplay_hook);
                netplay_recorder->meta = netplay_meta;
                netplay_recorder->begin_match(m, netplay_session->seed());
                const auto frames_between = m.frame - netplay_meta.sync_machine_frame;
                std::cout << "NETPLAY_RUNNING match_frame=" << netplay_match_frame
                          << " machine_frame=" << m.frame
                          << " sync_machine_frame=" << netplay_meta.sync_machine_frame
                          << " frames_between_sync_and_start=" << frames_between << '\n'
                          << std::flush;
                if (frames_between != 0) {
                    std::cerr << "NETPLAY_INVARIANT_VIOLATION frames_between_sync_and_start="
                              << frames_between << " (the machine ran frames between sync and GO)"
                              << '\n'
                              << std::flush;
                    net.error = "Internal error: frames ran between sync and start";
                }
            }
            net.room_code = netplay_session->room_code();
            net.peer_name = netplay_session->peer_name();
            net.delay = netplay_session->input_delay();
            net.player = netplay_session->player_index();
            net.ping = netplay_session->stats().ping_ms;
            net.stalls = netplay_session->stats().stall_count;
            net.status = "Running";
            break;
        case SState::Ended: {
            std::string reason_str = "Ended";
            switch (netplay_session->end_reason()) {
            case f3rt::netplay::EndReason::Completed:
                reason_str = "Match completed";
                break;
            case f3rt::netplay::EndReason::PeerLeft:
                reason_str = "Peer left match";
                break;
            case f3rt::netplay::EndReason::Desync: {
                const auto &di = netplay_session->desync_info();
                reason_str = "DESYNC at frame " + std::to_string(di.frame);
                std::cerr << "DESYNC frame=" << di.frame << " p1=0x" << std::hex << di.crc_p1
                          << " p2=0x" << di.crc_p2 << std::dec << '\n'
                          << std::flush;
                if (netplay_recorder) {
                    f3rt::netplay::SessionFacts facts;
                    facts.session_id = netplay_session->session_id();
                    facts.seed = netplay_session->seed();
                    facts.player_index = netplay_session->player_index();
                    facts.input_delay = netplay_session->input_delay();
                    facts.own_name = netplay_own_name;
                    facts.peer_name = netplay_session->peer_name();
                    facts.room_code = netplay_session->room_code();
                    facts.build_hash = f3rt::netplay::build_hash;
                    facts.desync_frame = di.frame;
                    facts.desync_crc_p1 = di.crc_p1;
                    facts.desync_crc_p2 = di.crc_p2;
                    netplay_recorder->meta.stall_count = netplay_session->stats().stall_count;
                    netplay_recorder->meta.clock_resyncs = clock_resyncs;
                    netplay_recorder->meta.audio_queue_drops = audio_queue_drops;
                    try {
                        const auto base =
                            (config_dir.empty() ? std::filesystem::current_path() : config_dir) /
                            "netplay-desync";
                        net.desync_path = netplay_recorder->write_bundle(base, facts).string();
                        std::cerr << "DESYNC_BUNDLE " << net.desync_path << '\n' << std::flush;
                    } catch (const std::exception &e) {
                        std::cerr << "Could not write desync bundle: " << e.what() << '\n'
                                  << std::flush;
                    }
                }
                break;
            }
            case f3rt::netplay::EndReason::Error:
                reason_str = netplay_session->last_error();
                break;
            case f3rt::netplay::EndReason::Disconnected:
                reason_str = netplay_session->last_error().empty() ? "Disconnected from server"
                                                                   : netplay_session->last_error();
                break;
            default:
                break;
            }
            net.error = reason_str;
            net.status = "Ended: " + reason_str;
            net.active = false;
            netplay_state_sync_done = false;
            netplay_lobby_active = false;
            netplay_trigger_snapshot.clear();
            netplay_recorder.reset();
            net.lobby_open = true;
            netplay_session.reset();
            break;
        }
        default:
            break;
        }
        if (netplay_session)
            for (auto &line : netplay_session->drain_chat())
                net.chat.emplace_back(line.from_player, std::move(line.text));
    };
    // One emulated frame's bookkeeping after the machine ran it: publish, dump, drain audio, advance the pace clock.
    auto finish_frame = [&](Snapshot *slot, std::chrono::steady_clock::time_point scheduled) {
        ++executed_frames;
        if (entry_watch) entry_watch->frame(m.frame);
        if (slot) {
            // Predicted next deadline, so the video thread's wake-up for this publish already knows when to sample input.
            if (pace)
                link.deadline.store((next_frame + frame_time).time_since_epoch().count(),
                                    std::memory_order_relaxed);
            fill_snapshot(*slot, scheduled);
            link.publish();
        }
        if (!dumpdir.empty() && m.frame >= dump_start && (m.frame - dump_start) % dump_every == 0)
            f3rt::dump_machine(m, dumpdir);
        size_t count;
        while ((count = m.audio->render_ready(samples.data(), samples.size() / 2)) != 0)
            handle_audio_chunk(count);
        if (pace) {
            // Pace against a rolling deadline. After a stall longer than one catch-up window, resync to now instead of
            // running flat out to make up lost time (that burst is what delayed audio by the length of the stall).
            next_frame += frame_time;
            const auto now = std::chrono::steady_clock::now();
            if (now - next_frame > std::chrono::milliseconds(MAX_CATCHUP_MS)) {
                next_frame = now;
                ++clock_resyncs;
                ++epoch;
            }
        }
    };
    // Solo frame (also the lobby frames while a netplay lobby waits; those take no input and may pin game state).
    auto step_local = [&](bool in_lobby) -> bool {
        if (pace && std::chrono::steady_clock::now() < next_frame) return false;
        std::array<f3rt::LocalInputWord, f3rt::LOCAL_PLAYER_COUNT> local{};
        if (!in_lobby) {
            if (!headless)
                for (unsigned p = 0; p < local.size(); ++p)
                    local[p] = link.input[p].load(std::memory_order_relaxed);
            if (input_script) {
                const auto scripted = input_script->words(m.frame + 1);
                for (unsigned p = 0; p < local.size(); ++p) local[p] |= scripted[p];
            }
        }
        Snapshot *slot = begin_unpublished();
        const auto scheduled = pace ? next_frame : std::chrono::steady_clock::now();
        f3rt::apply_local_inputs(m, local);
        if (!in_lobby && input_script) input_script->poke(m, m.frame + 1);
        if (in_lobby && has_hold) netplay_hook->lobby_hold_frame(m);
        if (!m.run_frame(translated))
            throw std::runtime_error("CPU halted at " + std::to_string(m.cpu.pc));
        if (in_lobby && has_hold) netplay_hook->lobby_hold_frame(m);
        finish_frame(slot, scheduled);
        return true;
    };
    // Netplay lockstep frame: latches this thread's input for the match frame, then steps once the peer's input is in.
    auto step_net = [&]() -> bool {
        if (next_netplay_local_frame <= netplay_match_frame) {
            f3rt::LocalInputWord word = 0;
            if (!link.chat_open.load(std::memory_order_relaxed)) {
                if (!headless) word = link.input[0].load(std::memory_order_relaxed);
                if (input_script) word |= input_script->words(m.frame + 1)[0];
            }
            netplay_session->submit_local(next_netplay_local_frame, word);
            ++next_netplay_local_frame;
        }
        const auto now = std::chrono::steady_clock::now();
        if (pace && now - next_frame > std::chrono::milliseconds(MAX_CATCHUP_MS)) {
            next_frame = now;
            ++clock_resyncs;
            ++epoch;
        }
        if ((pace && now < next_frame) || !netplay_session->ready_to_step(netplay_match_frame))
            return false;
        Snapshot *slot = begin_unpublished();
        const auto scheduled = pace ? next_frame : now;
        if (!step_lockstep_frame(m, *netplay_session, netplay_match_frame, netplay_hook, 60,
                                 netplay_recorder.get()))
            throw std::runtime_error("CPU halted at " + std::to_string(m.cpu.pc));
        ++netplay_match_frame;
        finish_frame(slot, scheduled);
        return true;
    };
    // Versus-mode trigger: snapshot the machine at the 0 -> 1 edge and open the lobby.
    auto check_trigger = [&](bool stepped) {
        bool trig = false;
        if (netplay_hook) {
            if (netplay_hook->lobby_trigger) trig = netplay_hook->lobby_trigger(m);
            else if (netplay_hook->in_versus) trig = netplay_hook->in_versus(m);
        }
        if (stepped && !net.active && !netplay_session && !netplay_lobby_active &&
            !prev_in_versus && trig) {
            netplay_trigger_snapshot.resize(m.state_size());
            m.save_state(netplay_trigger_snapshot);
            netplay_meta = {};
            netplay_meta.trigger_machine_frame = m.frame;
            netplay_trigger_time = std::chrono::steady_clock::now();
            netplay_lobby_active = true;
            if (netplay_hook->lobby_hold_begin) netplay_hook->lobby_hold_begin(m);
            net.lobby_open =
                !headless; // the lobby is UI; a headless run has none (main: in_lobby needs sdl.ui or a session)
            if (!netplay_auto.empty()) {
                if (netplay_auto == "host" || netplay_auto.starts_with("host:"))
                    start_session(true, auto_net, {});
                else if (netplay_auto.starts_with("join:"))
                    start_session(false, auto_net, netplay_auto.substr(5));
            }
        }
        if (!trig) prev_in_versus = false;
        else if (stepped) prev_in_versus = true;
    };
    // Sleeps until the next frame deadline, a command or stop; wakes every millisecond to poll a live session.
    auto idle = [&] {
        const auto now = std::chrono::steady_clock::now();
        auto until = std::chrono::steady_clock::time_point::max();
        link.deadline.store(pace && !halted ? next_frame.time_since_epoch().count() : 0,
                            std::memory_order_relaxed);
        if (pace && !halted && now < next_frame) until = next_frame;
        if (netplay_session) until = std::min(until, now + std::chrono::milliseconds(1));
        std::unique_lock lock(link.mutex);
        const auto ready = [&] { return link.stop || !link.commands.empty(); };
        if (until == std::chrono::steady_clock::time_point::max()) link.cv.wait(lock, ready);
        else link.cv.wait_until(lock, until, ready);
    };
    // Commands only ever run between frames.
    auto emulate = [&] {
        bool was_halted = false;
        while ((headless || !stopping()) && (!frames || executed_frames < frames)) {
            profile.tick();
            std::vector<Command> batch;
            {
                std::lock_guard lock(link.mutex);
                batch.swap(link.commands);
            }
            for (const auto &command : batch) apply(command);
            if (netplay_session) poll_session();
            const bool in_lobby = net.lobby_open || (netplay_session && !net.active);
            halted =
                !net.active && (netplay_state_sync_done || menu_paused || (in_lobby && !has_hold));
            if (was_halted && !halted) {
                ++epoch;
                next_frame = std::chrono::steady_clock::now();
            }
            was_halted = halted;
            const bool stepped = !halted && (net.active ? step_net() : step_local(in_lobby));
            check_trigger(stepped);
            if (stepped)
                for (const auto &[frame, text] : chat_send_frames)
                    if (m.frame == frame && netplay_session) netplay_session->send_chat(text);
            net.in_session = netplay_session != nullptr;
            if (net != net_sent || !net.chat.empty()) {
                link.publish_net(net);
                net_sent = net;
            }
            if (!stepped) idle();
        }
    };
    EmulationThread emulation(link);
    std::exception_ptr emulation_error;
    if (headless) emulate();
    else if (!quit && (!frames || executed_frames < frames)) {
        // Video side: SDL events, UI, input sampling, GPU/SDL present and motion interpolation. Reads emulated
        // state only through Link snapshots and changes it only through Link commands.
        bool menu_sent = false, screenshot_pending = false, surface_saved = false,
             surface_valid = false;
        // Netplay lives on the emulation thread; this is its last published state. The test-automation options keyed on
        // the emulated frame (screenshots, F1, chat box) fire on the first presented snapshot at or past their frame.
        NetStatus net_view;
        bool chat_opened = false, chat_closed = false;
        std::unordered_set<uint64_t> screenshots_taken;
        bool motion_capture_pending = false;
#ifdef F3RT_GPU
        uint64_t seen_epoch = 0, native_counted = 0;
        unsigned gpu_scale = video_options.scale;
        MotionReadout motion_readout{.begin = start};
        bool motion_pair_pending = false;
        auto next_motion_present = start;
        auto motion_present_period = frame_time;
        // Recomputed at startup and whenever motion interpolation is switched in the settings UI.
        auto refresh_motion_period = [&]() {
            const double display_hz = motion_presentation && sdl.gpu ? sdl.gpu->display_hz() : 0;
            motion_present_period =
                display_hz > 0 ? std::chrono::nanoseconds(uint64_t(1e9 / display_hz)) : frame_time;
        };
        refresh_motion_period();
        int observed_pixel_width = pixel_width, observed_pixel_height = pixel_height;
        bool scale_pending = false;
        auto scale_pending_since = start, scale_last_resize = start;
#endif
        auto reset_motion = [&]() {
            if (!motion_presentation) return;
#ifdef F3RT_GPU
            if (sdl.gpu) {
                sdl.gpu->reset_motion();
                ++motion_history_resets;
                ++motion_readout.resets;
            }
#endif
            motion_capture_pending = true;
        };
        emulation.thread = std::thread([&] {
#ifdef __APPLE__
            // Default-QoS threads sleeping ~14 ms between frames wake 1-4 ms late under timer coalescing, which skews both
            // the frame start and the input sampling lead; user-interactive QoS wakes within a few hundred microseconds.
            pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#endif
            try {
                emulate();
            } catch (...) {
                emulation_error = std::current_exception();
            }
            link.finish();
        });
        while (!quit) {
            bool refresh = false;
            SDL_Event event;
            while (SDL_PollEvent(&event)) {
                if (event.type == SDL_EVENT_QUIT) quit = true;
                const bool was_open = sdl.ui->open();
                const bool consumed = sdl.ui->process_event(event);
                sdl.input->process_event(event, consumed || sdl.ui->open());
                refresh |= was_open != sdl.ui->open();
                if (event.type == SDL_EVENT_KEY_DOWN && !event.key.repeat) {
                    if (!consumed && event.key.scancode == SDL_SCANCODE_ESCAPE) quit = true;
                    const bool fullscreen_key = event.key.scancode == SDL_SCANCODE_F11 ||
                                                (event.key.scancode == SDL_SCANCODE_RETURN &&
                                                 (event.key.mod & SDL_KMOD_ALT));
                    if (fullscreen_key) {
                        check(SDL_SetWindowFullscreen(
                            sdl.window, !(SDL_GetWindowFlags(sdl.window) & SDL_WINDOW_FULLSCREEN)));
                        sdl.input->release();
                        refresh = true;
                    }
                }
                if (event.type == SDL_EVENT_WINDOW_EXPOSED ||
                    event.type == SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED)
                    refresh = true;
#ifdef F3RT_GPU
                if (motion_presentation && sdl.gpu &&
                    (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED ||
                     event.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED)) {
                    const double hz = sdl.gpu->display_hz();
                    motion_present_period =
                        hz > 0 ? std::chrono::nanoseconds(uint64_t(1e9 / hz)) : frame_time;
                    next_motion_present = std::chrono::steady_clock::now();
                }
#endif
            }
            if (sdl.ui->open() != menu_sent) {
                menu_sent = sdl.ui->open();
                link.post({menu_sent ? Command::Pause : Command::Resume});
            }
            for (const auto &reply : link.take_replies()) {
                ui_state.message = reply.message;
                if (reply.loaded) {
                    sdl.input->release();
                    refresh = true;
                }
            }
            {
                const bool was_lobby = net_view.lobby_open;
                if (link.take_net(net_view)) {
                    if (net_view.lobby_open != was_lobby)
                        sdl.ui->set_lobby_open(net_view.lobby_open);
                    for (auto &[player, text] : net_view.chat)
                        sdl.ui->receive_chat(player, std::move(text));
                    net_view.chat.clear();
                    ui_state.netplay_in_session = net_view.in_session;
                    ui_state.netplay_active = net_view.active;
                    ui_state.netplay_status = net_view.status;
                    ui_state.netplay_error = net_view.error;
                    ui_state.netplay_room_code = net_view.room_code;
                    ui_state.netplay_peer_name = net_view.peer_name;
                    ui_state.netplay_desync_path = net_view.desync_path;
                    ui_state.netplay_ping = net_view.ping;
                    ui_state.netplay_stall_count = net_view.stalls;
                    ui_state.netplay_delay = net_view.delay;
                    ui_state.netplay_player_index = net_view.player;
                }
            }
            ui_state.netplay_own_name = settings.netplay_name;
            for (const auto &out_msg : sdl.ui->take_outgoing_chat())
                link.post({Command::Chat, 0, {}, out_msg});
            const uint64_t emulated_frame = link.frame.load(std::memory_order_relaxed);
            if (open_f1_frame && emulated_frame >= open_f1_frame && !sdl.ui->open())
                sdl.ui->set_open(true);
            if (chat_open_frame && !chat_opened && emulated_frame >= chat_open_frame) {
                chat_opened = true;
                sdl.ui->open_chat();
            }
            if (chat_close_frame && !chat_closed && emulated_frame >= chat_close_frame) {
                chat_closed = true;
                sdl.ui->close_chat();
            }
            link.chat_open.store(sdl.ui->chat_input_open(), std::memory_order_relaxed);
            if (sdl.ui->open() && !net_view.active)
                ui_state.status = "Solo paused at frame " + std::to_string(emulated_frame);
            sdl.ui->draw(ui_state);
            for (const auto action : sdl.ui->take_actions()) try {
                    using Action = f3rt::UiActionKind;
                    switch (action.kind) {
                    case Action::Screenshot:
                        screenshot_pending = true;
                        break;
                    case Action::SaveState:
                    case Action::LoadState: {
                        if (!slots_supported)
                            throw std::runtime_error(
                                "Slots require offline strict-native execution");
                        if (config_path.empty())
                            throw std::runtime_error("No user config path; supply --config FILE");
                        // The emulation thread performs the transfer between frames and answers with the status message.
                        link.post({action.kind == Action::SaveState ? Command::Save : Command::Load,
                                   0,
                                   std::filesystem::absolute(config_path).parent_path() / "states" /
                                       (set + "-" + std::to_string(action.slot) + ".f3state")});
                        break;
                    }
                    case Action::ApplySettings:
                        sdl.input->configure(settings);
                        if (sdl.audio) check(SDL_SetAudioStreamGain(sdl.audio, settings.volume));
                        if (video_mode != "fdp") video_filter = settings.filter;
#ifdef F3RT_GPU
                        if (sdl.gpu) {
                            auto requested = video_options;
                            parse_scale(settings.video_scale, video_scale_mode, requested);
                            automatic_scale = video_scale_mode != f3rt::VideoScaleMode::Fixed;
                            if (automatic_scale) {
                                check(SDL_GetWindowSizeInPixels(sdl.window, &pixel_width,
                                                                &pixel_height));
                                requested.scale = f3rt::video_scale_for_window(
                                    video_scale_mode, unsigned(std::max(pixel_width, 0)),
                                    unsigned(std::max(pixel_height, 0)), video_options.border);
                            }
                            if (requested.scale != video_options.scale) {
                                // Applied to the GPU once a snapshot captured at this scale arrives.
                                link.post({Command::SetScale, requested.scale});
                                video_options.scale = requested.scale;
                                ++scale_changes;
                            }
                            sdl.gpu->set_scale_mode(video_scale_mode);
                            sdl.gpu->set_linear(video_filter == "linear");
                            // Presentation only: neither setting touches emulated state, so switching is safe between frames.
                            video_interp = settings.interpolation;
                            video_interp_fields = settings.interpolation_fields;
                            const auto fields =
                                f3rt::parse_interpolation_fields(video_interp_fields);
                            if (!fields) throw std::runtime_error("Invalid interpolation fields");
                            sdl.gpu->set_interpolation(parse_interpolation_mode(video_interp),
                                                       *fields);
                            if (settings.motion_interp != motion_interp) {
                                motion_interp = settings.motion_interp;
                                motion_presentation = motion_interp;
                                refresh_motion_period();
                                motion_pair_pending = false;
                                next_motion_present = std::chrono::steady_clock::now();
                                reset_motion();
                            }
                        } else
#endif
                            check(SDL_SetTextureScaleMode(
                                sdl.texture, video_filter == "linear" ? SDL_SCALEMODE_LINEAR
                                                                      : SDL_SCALEMODE_NEAREST));
                        refresh = true;
                        break;
                    case Action::ApplyPostprocess:
#ifdef F3RT_GPU
                        if (sdl.gpu) {
                            const auto preset =
                                settings.postprocess == "off"   ? f3rt::Postprocess::Off
                                : settings.postprocess == "crt" ? f3rt::Postprocess::Crt
                                                                : f3rt::Postprocess::User;
                            sdl.gpu->set_postprocess(preset, settings.user_shader);
                            ui_state.active_postprocess = settings.postprocess;
                            ui_state.message = "Post-processing applied: " + settings.postprocess;
                            refresh = true;
                            break;
                        }
#endif
                        throw std::runtime_error("Post-processing requires the enhanced renderer");
                    case Action::SavePreferences:
                        if (!f3rt::save_frontend_settings(config_path.string(), settings,
                                                          ui_state.message))
                            throw std::runtime_error(ui_state.message);
                        ui_state.message = "Preferences saved to " + config_path.string();
                        break;
                    case Action::NetplayHost:
                    case Action::NetplayJoin: {
                        // Settings are sampled here; the emulation thread starts the session between frames.
                        const NetSettings net_settings{
                            settings.netplay_server, settings.netplay_name, settings.netplay_delay};
                        if (action.kind == Action::NetplayHost)
                            link.post({Command::NetHost, 0, {}, {}, net_settings});
                        else
                            link.post({Command::NetJoin, 0, {}, sdl.ui->join_code(), net_settings});
                        break;
                    }
                    case Action::NetplayCancel:
                    case Action::NetplayLocal:
                        link.post({action.kind == Action::NetplayCancel ? Command::NetCancel
                                                                        : Command::NetLocal});
                        sdl.input->release();
                        refresh = true;
                        break;
                    }
                } catch (const std::exception &error) {
                    ui_state.message = error.what();
                }
            if (quit) break;
            // Input is latched by the emulation thread once per emulated frame. A lockstep match keeps running (and
            // taking input) behind the menu; the chat box takes the keyboard.
            const bool menu = sdl.ui->open() && !net_view.active;
            const bool draw_menu =
                sdl.ui->open() || sdl.ui->lobby_open() || net_view.active || sdl.ui->chat_visible();
            for (unsigned p = 0; p < f3rt::LOCAL_PLAYER_COUNT; ++p)
                link.input[p].store(menu ? 0 : sdl.input->word(p), std::memory_order_relaxed);
#ifdef F3RT_GPU
            // Display-paced motion: block on the display tick (or, without one, the display period) before taking
            // the newest snapshot, so the interpolation alpha is sampled at this refresh.
            const bool motion_paced =
                motion_presentation && throttle && sdl.gpu && !draw_menu && !screenshot_pending;
            if (motion_paced && !sdl.gpu->pace_motion()) {
                {
                    std::unique_lock lock(link.mutex);
                    link.cv.wait_until(lock, next_motion_present, [&] { return link.done; });
                }
                next_motion_present += motion_present_period;
                const auto now = std::chrono::steady_clock::now();
                if (next_motion_present <= now) next_motion_present = now + motion_present_period;
            }
#else
            const bool motion_paced = false;
#endif
            const bool finished = link.finished();
            bool fresh = false;
            const Snapshot *shot = link.take(fresh);
            if (finished && emulation_error) break;
            if (shot && fresh)
                for (const uint64_t screenshot_frame : screenshot_frames)
                    if (shot->frame >= screenshot_frame &&
                        screenshots_taken.insert(screenshot_frame).second) {
                        screenshot_pending = true;
                        break;
                    }
            const bool capture_final = finished && shot && !surface_saved && !surface.empty() &&
                                       frames && executed_frames == frames;
#ifdef F3RT_GPU
            if (shot && fresh && sdl.gpu) {
                if (shot->epoch != seen_epoch) {
                    seen_epoch = shot->epoch;
                    reset_motion();
                }
                if (shot->scale != gpu_scale) {
                    sdl.gpu->set_scale(shot->scale);
                    gpu_scale = shot->scale;
                }
            }
            if (motion_presentation && sdl.gpu && shot && (fresh || motion_capture_pending)) {
                sdl.gpu->capture_motion(*shot->gpu, shot->frame);
                motion_capture_pending = false;
                motion_pair_pending = true;
            }
            if (motion_presentation) {
                const uint64_t published = link.frames_published.load(std::memory_order_relaxed);
                motion_readout.native += published - native_counted;
                native_counted = published;
            }
#endif
            if (shot && (fresh || refresh || draw_menu || screenshot_pending || capture_final ||
                         (motion_paced && surface_valid))) {
                std::filesystem::path screenshot;
                if (screenshot_pending) {
                    const auto stamp = std::chrono::system_clock::now().time_since_epoch().count();
                    screenshot = std::filesystem::absolute(config_path).parent_path() /
                                 "screenshots" /
                                 (set + "-" + std::to_string(shot->frame) + "-" +
                                  std::to_string(stamp) + ".png");
                    std::filesystem::create_directories(screenshot.parent_path());
                }
#ifdef F3RT_GPU
                if (sdl.gpu) {
                    if (automatic_scale) {
                        // Poll physical pixels, not logical resize events: display-density/fullscreen changes count too.
                        // Debounce geometry until 100ms quiet, but never defer a live drag more than 250ms.
                        check(SDL_GetWindowSizeInPixels(sdl.window, &pixel_width, &pixel_height));
                        const auto now = std::chrono::steady_clock::now();
                        if (pixel_width != observed_pixel_width ||
                            pixel_height != observed_pixel_height) {
                            observed_pixel_width = pixel_width;
                            observed_pixel_height = pixel_height;
                            if (!scale_pending) scale_pending_since = now;
                            scale_pending = true;
                            scale_last_resize = now;
                        }
                        if (scale_pending &&
                            (now - scale_last_resize >= std::chrono::milliseconds(100) ||
                             now - scale_pending_since >= std::chrono::milliseconds(250))) {
                            scale_pending = false;
                            const unsigned target = f3rt::video_scale_for_window(
                                video_scale_mode, unsigned(std::max(pixel_width, 0)),
                                unsigned(std::max(pixel_height, 0)), video_options.border);
                            const unsigned previous = video_options.scale;
                            if (target != previous) {
                                // The GPU follows when the first snapshot at the new scale arrives.
                                link.post({Command::SetScale, target});
                                video_options.scale = target;
                                ++scale_changes;
                            }
                            std::cout << "video_scale pixels=" << pixel_width << 'x' << pixel_height
                                      << " previous=" << previous << " scale=" << target
                                      << " changes=" << scale_changes << " debounce_ms="
                                      << std::chrono::duration<double, std::milli>(
                                             now - scale_pending_since)
                                             .count()
                                      << '\n';
                        }
                    }
                    sdl.gpu->set_overlay(draw_menu ? +[](void *user,SDL_GPUCommandBuffer *command,
                        SDL_GPUTexture *texture,Uint32 width,Uint32 height) {
                            static_cast<f3rt::FrontendUi *>(user)->render_gpu(command,texture,width,height);
                        } : nullptr,sdl.ui.get());
                    if (motion_presentation) {
                        bool presented = false;
                        const std::string screenshot_str = screenshot.string();
                        const char *capture_path =
                            screenshot_pending ? screenshot_str.c_str() : nullptr;
                        if (throttle && !draw_menu && !capture_final && !screenshot_pending)
                            presented = sdl.gpu->present_motion(*shot->gpu, shot->start, frame_time,
                                                                false, nullptr);
                        else
                            presented = sdl.gpu->present_motion(*shot->gpu, shot->start, frame_time,
                                                                true, capture_path);
                        if (presented) {
                            ++motion_presentations;
                            const auto &stats = sdl.gpu->last_motion();
                            motion_readout.observe(stats, motion_pair_pending);
                            motion_pair_pending = false;
                            if (stats.paired) {
                                motion_min_alpha = std::min(motion_min_alpha, stats.alpha);
                                motion_max_alpha = std::max(motion_max_alpha, stats.alpha);
                                motion_intermediates +=
                                    stats.alpha > 0 && stats.alpha < 1 &&
                                    (stats.sprites || stats.playfield_rows || stats.text_rows);
                            }
                        }
                    } else {
                        if (screenshot_pending)
                            sdl.gpu->present(*shot->gpu, screenshot.string().c_str());
                        else if (capture_final || !throttle) sdl.gpu->draw(*shot->gpu);
                        else sdl.gpu->present(*shot->gpu);
                        if (capture_final) sdl.gpu->save_surface(surface.string().c_str());
                    }
                } else
#endif
                {
                    if (fresh || refresh || !surface_valid)
                        check(SDL_UpdateTexture(sdl.texture, nullptr, shot->pixels.data(),
                                                int(video_options.width() * 4)));
                    check(SDL_RenderClear(sdl.renderer));
                    if (rotation)
                        check(SDL_RenderTextureRotated(sdl.renderer, sdl.texture, nullptr,
                                                       &rotated_rect, rotation, nullptr,
                                                       SDL_FLIP_NONE));
                    else check(SDL_RenderTexture(sdl.renderer, sdl.texture, nullptr, nullptr));
                    sdl.ui->render_cpu();
                    if (capture_final || screenshot_pending) {
                        SDL_Surface *shot_surface = SDL_RenderReadPixels(sdl.renderer, nullptr);
                        check(shot_surface != nullptr);
                        const bool saved = (!capture_final ||
                                            SDL_SaveBMP(shot_surface, surface.string().c_str())) &&
                                           (!screenshot_pending ||
                                            SDL_SavePNG(shot_surface, screenshot.string().c_str()));
                        SDL_DestroySurface(shot_surface);
                        check(saved);
                    }
                    check(SDL_RenderPresent(sdl.renderer));
                }
                surface_valid = true;
                if (capture_final) surface_saved = true;
                if (screenshot_pending) {
                    ui_state.message = "Screenshot saved to " + screenshot.string();
                    std::cout << "SCREENSHOT_SAVED " << screenshot.string() << '\n' << std::flush;
                    screenshot_pending = false;
                }
            }
#ifdef F3RT_GPU
            if (motion_interp && motion_presentation && sdl.gpu &&
                std::chrono::steady_clock::now() - motion_readout.begin >=
                    std::chrono::seconds(2)) {
                motion_readout.report(*sdl.gpu);
            }
#endif
            if (finished) break;
            if (!motion_paced) link.wait_for_work(std::chrono::milliseconds(8));
        }
        link.request_stop();
        emulation.thread.join();
        if (emulation_error) std::rethrow_exception(emulation_error);
    }
    {
        size_t count;
        while ((count = m.audio->render(samples.data(), samples.size() / 2)) != 0)
            handle_audio_chunk(count);
    }
    if (!eeprom.empty()) m.save_eeprom(eeprom);
    if (!fallback_report.empty()) {
        std::ofstream report(fallback_report);
        report << "pc\tcount\n";
        for (size_t i = 0; i < m.fallback_hits.size(); ++i)
            if (m.fallback_hits[i])
                report << "0x" << std::hex << i * 2 << std::dec << '\t' << m.fallback_hits[i]
                       << '\n';
        if (!report) throw std::runtime_error("Fallback report write failed");
    }
    if (m.game_video) m.game_video->report(std::cout);
    if (m.sprite_units) m.sprite_units->write_summary(std::cout);
    if (m.discovery) m.discovery->finish(m);
    profile.flush();
    std::cout << "set=" << set << " frames=" << m.frame << " pc=0x" << std::hex << m.cpu.pc
              << " sound_pc=0x" << m.sound_pc()
              << " audio_backend=" << f3rt::audio_backend_name(settings.audio_backend)
              << " sound_driver="
              << (settings.audio_backend == f3rt::AudioBackend::Enhanced
                      ? "none"
                      : (m.sound_native ? "native" : "oracle"))
              << " frame_crc=0x"
              << f3rt::crc32(reinterpret_cast<const uint8_t *>(m.native_pixels().data()),
                             m.native_pixels().size() * 4)
              << std::dec << " cycles=" << m.cpu.cycles << " native_blocks=" << m.native_blocks
              << " fallback_instructions=" << m.fallback_instructions
              << " audio_frames=" << audio_frames << " audio_peak=" << audio_peak
              << " nonzero_samples=" << nonzero_samples << '\n';
#ifdef F3RT_GPU
    if (sdl.gpu && m.sprite_units && m.sprite_units->flicker_shadows_available()) {
        const auto report = m.sprite_units->report();
        std::cout << "FLICKER-SHADOWS active=" << shadows_active
                  << " native_frames=" << executed_frames
                  << " presented_frames=" << sdl.gpu->presented_frames()
                  << " shadowed_objects=" << report.flicker.invocations
                  << " tagged_entries=" << report.flicker.replay_entries
                  << " frames_with_shadow=" << report.flicker.frames << '\n';
    }
#endif
    if (motion_interp && motion_presentation)
        std::cout << "MOTION-TOTAL native_frames=" << executed_frames
                  << " drawable_submissions=" << motion_presentations
                  << " interpolated_submissions=" << motion_intermediates
                  << " history_resets=" << motion_history_resets << " interpolated_pct="
                  << (motion_presentations
                          ? 100.0 * double(motion_intermediates) / double(motion_presentations)
                          : 0.0)
                  << " alpha_min=" << motion_min_alpha << " alpha_max=" << motion_max_alpha
                  << " elapsed_seconds="
                  << std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count()
                  << '\n';
    if (settings.audio_backend == f3rt::AudioBackend::Enhanced) {
        const auto stats = m.audio->enhanced_stats();
        std::cout << "hle_commands=" << stats.commands
                  << " hle_rendered_frames=" << stats.rendered_frames << '\n';
    }
    if (audio_queue_samples)
        std::cerr << "f3rt: pacing clock_resyncs=" << clock_resyncs
                  << " audio_queue_drops=" << audio_queue_drops << " queued_ms mean="
                  << 1000.0 * double(audio_queue_sum) / double(audio_queue_samples) /
                         (4.0 * audio_rate)
                  << " max=" << 1000.0 * double(audio_queue_max) / (4.0 * audio_rate) << '\n';
    std::cout << "FINAL state_crc=0x" << std::hex << std::setw(8) << std::setfill('0')
              << m.state_crc() << std::dec << '\n';
    if (netplay_session && netplay_session->state() == f3rt::netplay::SessionState::Running) {
        netplay_session->send_result(netplay_match_frame, 0xFF, m.state_crc());
        netplay_session->poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        netplay_session->send_bye();
        netplay_session->poll();
    }
    return 0;
} catch (const std::exception &e) {
    std::cerr << "f3rt: " << e.what() << '\n';
    return 1;
}
