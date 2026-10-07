#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/game_video.hpp"
#include "f3rt/video.hpp"
#include "f3rt/rom.hpp"
#include "renderer/gpu/video.hpp"
#include "renderer/gpu/motion.hpp"
#include "gameplay_inputs.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>
#ifdef F3RT_GENERATED
#include "program.h"
#endif
#ifdef F3RT_SOUND_GENERATED
#include "sound_program.h"
#endif

namespace {
struct Options {
    std::filesystem::path rom_dir, dump_dir;
    uint64_t frames = 4000, seed = 12345, every = 1;
    bool demo = false;
    unsigned demo_seconds = 30;
    f3rt::GameVideoOptions video;
    f3rt::VideoInterpolation interp = f3rt::VideoInterpolation::Off;
};
void require(bool ok, const std::string &why) { if (!ok) throw std::runtime_error(why); }
uint64_t number(const std::string &s) {
    require(!s.empty() && s[0] != '-', "Expected unsigned integer: " + s);
    size_t used = 0; auto n = std::stoull(s, &used, 0);
    require(used == s.size(), "Invalid integer: " + s); return n;
}
Options parse(int argc, char **argv) {
    Options o;
#ifdef F3RT_DEFAULT_ROM_DIR
    o.rom_dir = F3RT_DEFAULT_ROM_DIR;
#endif
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&]() { require(++i < argc, "Missing value for " + arg); return std::string(argv[i]); };
        if (arg == "--rom-dir") o.rom_dir = value();
        else if (arg == "--dump-dir") o.dump_dir = value();
        else if (arg == "--frames") o.frames = number(value());
        else if (arg == "--seed") o.seed = number(value());
        else if (arg == "--every") o.every = number(value());
        else if (arg == "--demo") o.demo = true;
        else if (arg == "--demo-seconds") {
            auto n = number(value()); require(n && n <= 3600, "Demo seconds must be 1..3600");
            o.demo_seconds = unsigned(n);
        }
        else if (arg == "--scale") {
            auto n = number(value()); require(n && n <= f3rt::GameVideoOptions::max_gpu_scale, "Scale must be 1..8");
            o.video.scale = unsigned(n);
        } else if (arg == "--interp") {
            auto v = value();
            if (v == "off") o.interp = f3rt::VideoInterpolation::Off;
            else if (v == "linear") o.interp = f3rt::VideoInterpolation::Linear;
            else if (v == "fit") o.interp = f3rt::VideoInterpolation::Fit;
            else throw std::runtime_error("Interpolation must be off, linear or fit");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Strict-native seeded Land Maker temporal GPU proof\n"
                "--rom-dir DIR --frames N (4000) --seed N (12345) --scale N (1..8)\n"
                "--every N (1) --dump-dir DIR --interp off|linear|fit\n"
                "--demo --demo-seconds N (30): one-window LEFT native / RIGHT motion comparison;\n"
                "warms up --frames emulated frames, then pans a frozen ROM playfield render-only.\n"
                "Short proof runs without visible paired ROM motion intentionally fail.\n";
            std::exit(0);
        } else throw std::runtime_error("Unknown argument: " + arg);
    }
    require(!o.rom_dir.empty() && o.frames && o.every, "ROM directory and positive frames/every required");
    return o;
}
std::unique_ptr<f3rt::Machine> machine(const Options &o) {
    auto m = std::make_unique<f3rt::Machine>(f3rt::RomSet::load(o.rom_dir, "landmakrj"));
    m->allow_main_fallback = false;
#ifdef F3RT_GENERATED
    require(f3_generated_register(&m->cpu), "Native main registration failed");
#else
    throw std::runtime_error("Requires generated strict-native main blocks");
#endif
#ifdef F3RT_SOUND_GENERATED
    m->use_native_sound(f3_sound_blocks, f3_sound_block_count,
        {f3_sound_excluded_ranges, f3_sound_excluded_count}, f3_sound_rom_crc32);
#else
    throw std::runtime_error("Requires generated native sound blocks");
#endif
    // Both machines retain the same canonical native 320x232 output regardless
    // of the diagnostic GPU scale. Only one machine is presented/interpolated.
    m->game_video = std::make_unique<f3rt::GameVideo>(*m, f3rt::GameVideoMode::Game, f3rt::GameVideoOptions{});
    m->game_video->enable_gpu_presentation();
    m->game_video->set_gpu_scale(o.video.scale);
    return m;
}
void inputs(f3rt::Machine &m, uint16_t word) {
    constexpr std::array<uint32_t, 7> masks{1,2,4,8,1,2,4};
    for (unsigned i = 0; i < 7; ++i) m.set_input(i < 4 ? 1 : 0, masks[i], (word & (1u << i)) != 0);
    m.set_input(0, 0x1000, (word & 0x80) != 0);
    if (word & 0x100) m.system_inputs &= ~0x10u; else m.system_inputs |= 0x10u;
}
void advance(f3rt::Machine &m) {
    require(m.run_frame(true) && !m.cpu.halted && !m.fallback_instructions,
        "Strict-native failure at frame " + std::to_string(m.frame));
}
std::vector<uint8_t> state(const f3rt::Machine &m) {
    std::vector<uint8_t> s(m.state_size()); m.save_state(s); return s;
}
std::vector<int16_t> audio(f3rt::Machine &m) {
    std::array<int16_t, 8192> block{}; std::vector<int16_t> result;
    while (auto count = m.audio->render(block.data(), block.size() / 2))
        result.insert(result.end(), block.begin(), block.begin() + count * 2);
    return result;
}
uint32_t hash(std::span<const uint32_t> pixels) {
    return f3rt::crc32(reinterpret_cast<const uint8_t *>(pixels.data()), pixels.size_bytes());
}
void png(const std::filesystem::path &path, std::span<const uint32_t> pixels, f3rt::GameVideoOptions o) {
    auto *s = SDL_CreateSurfaceFrom(int(o.width()), int(o.height()), SDL_PIXELFORMAT_ARGB8888,
        const_cast<uint32_t *>(pixels.data()), int(o.width() * 4));
    require(s != nullptr, SDL_GetError());
    bool ok = SDL_SavePNG(s, path.string().c_str()); SDL_DestroySurface(s);
    require(ok, "PNG write failed: " + path.string());
}
void text_sampling(f3rt::GpuVideo &gpu, const f3rt::CapturedFrame &source, f3rt::GameVideoOptions o) {
    if (o.scale % 2) return; // Half-native-pixel translation is an integer output shift at even scales.
    auto a = std::make_unique<f3rt::CapturedFrame>(source);
    uint16_t cell = 0; bool found = false;
    for (unsigned i = 0; i < 4096 && !found; ++i) {
        cell = source.text.map()[i];
        // Visible texel of this captured cell: opaque pen with a non-black palette color.
        const int cell_x = int(i % 64) * 8, cell_y = int(i / 64) * 8;
        for (unsigned p = 0; p < 64 && !found; ++p) {
            const auto pixel = source.text.pixel(cell_x + int(p % 8), cell_y + int(p / 8), false);
            found = (pixel.flags & 0x10) && source.colors[pixel.palette & 8191u];
        }
    }
    require(found, "Text pixel proof needs a visible captured ROM glyph");
    a->fallback = false; a->sprite_count = 0;
    a->colors[0] = 0;
    std::fill(a->text.map().begin(), a->text.map().end(), cell);
    a->rows = {};
    for (unsigned y = 24; y < 256; ++y) {
        auto &row = a->rows[y];
        row.text_y = int16_t(y - 24); row.blend = {8, 8, 8, 8};
        auto &layer = row.text;
        layer.enabled = true; layer.blend_mode = 1; layer.priority = 7;
        layer.clip_enabled = 1; layer.clip_inverse = true; row.clips[0] = {46, 366};
    }
    auto b = std::make_unique<f3rt::CapturedFrame>();
    std::vector<uint32_t> before(size_t(o.width()) * o.height()), after(before.size()), middle(before.size());
    gpu.draw(*a, before, 256);
    for (unsigned axis = 0; axis < 2; ++axis) {
        *b = *a;
        for (unsigned y = 24; y < 256; ++y) ++(axis ? b->rows[y].text_y : b->rows[y].text_x);
        gpu.reset_motion(); gpu.capture_motion(*a, 100); gpu.capture_motion(*b, 101);
        gpu.draw(*b, after, 256); gpu.draw_motion(*b, .5f, middle, 256);
        require(gpu.last_motion().text_rows == 232 && middle != before && middle != after,
            "Text half-pixel motion must be visibly distinct from both endpoints");
        const unsigned shift = o.scale / 2;
        for (unsigned y = 0; y < o.height() - (axis ? shift : 0); ++y)
            for (unsigned x = 0; x < o.width() - (axis ? 0 : shift); ++x)
                require(middle[size_t(y) * o.width() + x] ==
                    before[size_t(y + (axis ? shift : 0)) * o.width() + x + (axis ? 0 : shift)],
                    "Fractional text sampling must translate ROM glyph pixels exactly");
    }
    std::cout << "TEXT_SHADER horizontal_vertical_half_pixel=exact scale=" << o.scale << '\n';
}
struct SdlLifetime {
    SdlLifetime() { require(SDL_Init(SDL_INIT_VIDEO), SDL_GetError()); }
    ~SdlLifetime() { SDL_Quit(); }
};
// This diagnostic deliberately separates steady geometry motion from a game's
// stationary scenes and tile-animation identity changes. The machine is frozen.
int comparison(const Options &o) {
    using Clock = std::chrono::steady_clock;
    auto owner = machine(o); auto &m = *owner;
    f3rt::test::GameplaySchedule schedule(o.seed, f3rt::test::ScheduleConfig{.versus = false});
    std::array<int16_t, 8192> sound{};
    while (m.frame < o.frames) {
        inputs(m, schedule.step(m.frame)[0]); advance(m);
        while (m.audio->render(sound.data(), sound.size() / 2)) {}
    }
    const auto &base = m.game_video->captured_frame();
    require(!base.fallback, "Demo warmup ended in oracle fallback; use --frames 1560 --seed 5");
    const auto before = state(m); const auto pixels = m.native_pixels();
    const auto sync_crc = m.sync_state_crc();
    struct Window {
        SDL_Window *value;
        ~Window() { SDL_DestroyWindow(value); }
    } window{SDL_CreateWindow("Motion comparison — LEFT native ~59Hz | RIGHT interpolated | frozen ROM PF pan",
        1280, 464, SDL_WINDOW_RESIZABLE | SDL_WINDOW_HIGH_PIXEL_DENSITY)};
    require(window.value != nullptr, SDL_GetError());
    f3rt::GpuVideo gpu(window.value, o.video, m.video->playfield_tiles(), m.video->sprite_tiles(),
        false, true, o.interp);
    gpu.set_scale_mode(f3rt::VideoScaleMode::Auto);
    auto scene = std::make_unique<f3rt::CapturedFrame>(base);
    auto pan = [&](uint64_t step) {
        // +/-64 native pixels, 2px/native frame. A bounded triangle avoids
        // whole-map teleports; raw tilemap period crossings still snap.
        const int phase = int(step % 128);
        const int x = phase <= 64 ? -64 + phase * 2 : 192 - phase * 2;
        for (unsigned y = 24; y < 256; ++y) for (unsigned pf = 0; pf < 4; ++pf) {
            scene->rows[y].playfields[pf].source_x = base.rows[y].playfields[pf].source_x - x * 256;
        }
    };
    if (!o.dump_dir.empty()) {
        std::filesystem::create_directories(o.dump_dir);
        std::vector<uint32_t> native(size_t(o.video.width()) * o.video.height()), midpoint(native.size());
        pan(7);gpu.capture_motion(*scene, 7);pan(8);gpu.capture_motion(*scene, 8);
        gpu.draw(*scene, native);gpu.draw_motion(*scene, .5f, midpoint);
        require(native != midpoint, "Frozen ROM PF pan produced no visible interpolated pixels");
        png(o.dump_dir / "demo-native.png", native, o.video);
        png(o.dump_dir / "demo-interpolated.png", midpoint, o.video);
        std::vector<uint32_t> both(native.size() * 2);
        const unsigned width = o.video.width(), height = o.video.height();
        for (unsigned y = 0; y < height; ++y) {
            std::copy_n(native.begin() + y * width, width, both.begin() + y * width * 2);
            std::copy_n(midpoint.begin() + y * width, width, both.begin() + y * width * 2 + width);
        }
        auto *surface = SDL_CreateSurfaceFrom(int(width * 2), int(height), SDL_PIXELFORMAT_ARGB8888,
            both.data(), int(width * 8));
        require(surface != nullptr, SDL_GetError());
        const bool saved = SDL_SavePNG(surface, (o.dump_dir / "demo-side-by-side.png").string().c_str());
        SDL_DestroySurface(surface);require(saved, "Comparison PNG write failed");
        std::cout << "DEMO-IMAGE left=canonical right=alpha0.5 native_crc=0x" << std::hex << hash(native)
            << " interpolated_crc=0x" << hash(midpoint) << std::dec << '\n';
    }
    gpu.reset_motion();pan(0);gpu.capture_motion(*scene, 0);
    const auto period = std::chrono::nanoseconds(uint64_t(
        1e9 * f3rt::Machine::frame_pixels / f3rt::Machine::pixel_clock));
    const auto begin = Clock::now();
    auto frame_start = begin, next_frame = begin + period, report_start = begin, next_present = begin;
    const double display_hz = gpu.display_hz();
    auto present_period = display_hz > 0 ? std::chrono::nanoseconds(uint64_t(1e9 / display_hz)) : period;
    uint64_t step = 0, submitted = 0, interpolated = 0, moving = 0;
    uint64_t window_submitted = 0, window_interpolated = 0, window_moving = 0;
    float min_alpha = 1, max_alpha = 0;
    bool quit = false;
    bool capture_pending = !o.dump_dir.empty();
    const std::string capture_file = capture_pending ? (o.dump_dir / "demo-window.png").string() : std::string{};
    std::cout << "DEMO warmup_frame=" << m.frame << " seed=" << o.seed << " driver=" << gpu.driver()
        << " left=native right=motion diagnostic_render_only_pf_pan=2px/native"
        << " seconds=" << o.demo_seconds << '\n';
    while (!quit && Clock::now() - begin < std::chrono::seconds(o.demo_seconds)) {
        SDL_Event event;
        while (SDL_PollEvent(&event)) {
            if (event.type == SDL_EVENT_QUIT || (event.type == SDL_EVENT_KEY_DOWN &&
                event.key.scancode == SDL_SCANCODE_ESCAPE)) quit = true;
            if (event.type == SDL_EVENT_WINDOW_DISPLAY_CHANGED || event.type == SDL_EVENT_DISPLAY_CURRENT_MODE_CHANGED) {
                const double hz = gpu.display_hz();
                present_period = hz > 0 ? std::chrono::nanoseconds(uint64_t(1e9 / hz)) : period;
                next_present = Clock::now();
            }
        }
        if (quit) break;
        const bool display_paced = gpu.pace_motion();
        const auto now = Clock::now();
        while (now >= next_frame) {
            pan(++step);gpu.capture_motion(*scene, step);
            frame_start = next_frame;next_frame += period;
        }
        if (!display_paced && now < next_present) {
            std::this_thread::sleep_until(std::min(next_frame, next_present));
            continue;
        }
        const bool capture = capture_pending && step >= 16;
        gpu.draw_motion_comparison_timed(*scene, frame_start, period, capture ? capture_file.c_str() : nullptr);
        const auto &stats = gpu.last_motion();
        if (gpu.last_presented()) {
            if (capture) capture_pending = false;
            ++submitted;++window_submitted;
            const bool between = stats.paired && stats.alpha > 0 && stats.alpha < 1;
            const bool changed = stats.moving_playfield_rows != 0;
            const bool applied = stats.playfield_rows != 0;
            moving += between && changed;window_moving += between && changed;
            interpolated += between && applied;window_interpolated += between && applied;
            if (stats.paired) { min_alpha=std::min(min_alpha,stats.alpha);max_alpha=std::max(max_alpha,stats.alpha); }
        }
        if (Clock::now() - report_start >= std::chrono::seconds(2)) {
            const auto end = Clock::now();
            const double seconds = std::chrono::duration<double>(end - report_start).count();
            std::cout << std::fixed << std::setprecision(1)
                << "DEMO display_hz=" << gpu.display_hz() << " requested_display_hz=" << gpu.requested_display_hz()
                << " display_callback_hz=" << gpu.display_callback_hz() << " scanout_hz=unknown"
                << " drawable_submissions_s=" << double(window_submitted)/seconds
                << " interpolated_pct=" << (window_submitted?100.0*double(window_interpolated)/window_submitted:0.0)
                << " moving_interpolated_pct=" << (window_moving?100.0*double(window_interpolated)/window_moving:0.0)
                << '\n' << std::flush;
            report_start=end;window_submitted=window_interpolated=window_moving=0;
        }
        if (!display_paced) {
            next_present += present_period;
            const auto end = Clock::now();
            if (next_present <= end) next_present = end + present_period;
            std::this_thread::sleep_until(std::min(next_frame, next_present));
        }
    }
    require(before == state(m) && pixels == m.native_pixels() && sync_crc == m.sync_state_crc(),
        "Live comparison changed frozen machine/native state");
    std::cout << "DEMO-SUCCESS render_native_steps=" << step << " drawable_submissions=" << submitted
        << " interpolated_submissions=" << interpolated << " moving_inbetweens=" << moving
        << " alpha=" << min_alpha << ':' << max_alpha << " frozen_state_native_sync=exact"
        << " emulated_frame=" << m.frame << " fallback_instructions=" << m.fallback_instructions << '\n';
    return 0;
}
}

int main(int argc, char **argv) try {
    auto o = parse(argc, argv); SdlLifetime sdl;
    if (o.demo) return comparison(o);
    auto owner = machine(o), reference = machine(o); auto &m = *owner;
    f3rt::GpuVideo gpu(nullptr, o.video, m.video->playfield_tiles(), m.video->sprite_tiles(),
        false, false, o.interp);
    if (!o.dump_dir.empty()) std::filesystem::create_directories(o.dump_dir);
    std::vector<uint32_t> canonical(size_t(o.video.width()) * o.video.height()), out(canonical.size()), repeat(out.size());
    f3rt::test::GameplaySchedule schedule(o.seed, f3rt::test::ScheduleConfig{.versus = false});
    uint64_t samples = 0, paired = 0, visible = 0, eligible = 0, supported = 0, fallback = 0, audio_samples = 0;
    uint64_t sprite_draws = 0, playfield_draws = 0, text_draws = 0;
    uint64_t moving_samples = 0, accepted_moving_samples = 0, visible_moving_samples = 0;
    f3rt::MotionInterpolationStats census{};
    uint32_t audio_crc = 0xffffffffu; bool dumped = false;
    std::vector<uint8_t> replay_pre, replay_post; std::vector<int16_t> replay_audio;
    std::vector<uint32_t> replay_pixels;
    uint32_t replay_sync_crc = 0;
    uint64_t half_visible = 0;
    std::cout << "driver=" << gpu.driver() << " seed=" << o.seed << " scale=" << o.video.scale << '\n';
    while (m.frame < o.frames) {
        auto word = schedule.step(m.frame)[0]; inputs(m, word); inputs(*reference, word);
        if (m.frame + 1 == o.frames) replay_pre = state(m);
        advance(m); advance(*reference);
        const auto &scene = m.game_video->captured_frame();
        if (scene.fallback) ++fallback; else ++supported;
        gpu.capture_motion(scene, m.frame);
        auto sound = audio(m), sound_reference = audio(*reference);
        require(sound == sound_reference, "Native audio parity frame " + std::to_string(m.frame));
        audio_samples += sound.size();
        for (int16_t sample : sound) for (unsigned shift : {0u, 8u}) {
            audio_crc ^= uint8_t(uint16_t(sample) >> shift);
            for (unsigned bit = 0; bit < 8; ++bit) audio_crc = (audio_crc >> 1) ^ ((audio_crc & 1) ? 0xedb88320u : 0u);
        }
        require(m.native_pixels() == reference->native_pixels(), "Native pixel parity frame " + std::to_string(m.frame));
        if ((m.frame - 1) % o.every != 0 && m.frame != o.frames) continue;
        ++samples; const auto before = state(m); const auto pixels = m.native_pixels();
        require(before == state(*reference), "Seeded canonical state parity frame " + std::to_string(m.frame));
        require(m.sync_state_crc() == reference->sync_state_crc(), "Seeded sync state parity");
        gpu.draw(scene, canonical);
        gpu.draw(scene, repeat, ~0u);
        require(repeat == canonical, "Caller layer-mask high bits must not enable motion sampling");
        gpu.draw_motion(scene, 1, out);
        require(out == canonical, "Alpha1 != canonical GPU frame " + std::to_string(m.frame));
        gpu.draw_motion(scene, std::numeric_limits<float>::quiet_NaN(), out);
        require(out == canonical, "Nonfinite GPU alpha must snap to canonical");
        bool frame_visible = false;
        // Fixed phase coverage for approximately two / 2.4 presentations per
        // native frame; not a physical 120/144Hz display measurement.
        constexpr std::array<float, 9> phases{0, 1.f/12, .25f, 5.f/12, .5f, 2.f/3, .75f, 5.f/6, 1};
        for (float alpha : phases) {
            gpu.draw_motion(scene, alpha, out); auto stats = gpu.last_motion();
            eligible += stats.sprites + stats.playfield_rows + stats.text_rows;
            sprite_draws += stats.sprites; playfield_draws += stats.playfield_rows; text_draws += stats.text_rows;
            if (alpha == .5f && stats.paired) ++paired;
            if (alpha == .5f && stats.paired) {
                const bool moving = stats.moving_sprites || stats.moving_playfield_rows || stats.moving_text_rows;
                moving_samples += moving;
                accepted_moving_samples += moving && (stats.sprites || stats.playfield_rows || stats.text_rows);
                visible_moving_samples += moving && out != canonical;
                census.moving_sprites += stats.moving_sprites;
                census.moving_playfield_rows += stats.moving_playfield_rows;
                census.moving_text_rows += stats.moving_text_rows;
                census.sprite_identity_rejections += stats.sprite_identity_rejections;
                census.sprite_count_rejections += stats.sprite_count_rejections;
                census.sprite_transform_rejections += stats.sprite_transform_rejections;
                census.sprite_jump_rejections += stats.sprite_jump_rejections;
                census.row_control_rejections += stats.row_control_rejections;
                census.row_transform_rejections += stats.row_transform_rejections;
                census.row_jump_rejections += stats.row_jump_rejections;
            }
            gpu.draw_motion(scene, alpha, repeat);
            require(out == repeat, "Repeated frozen motion draw changed pixels");
            if (alpha > 0 && alpha < 1 && out != canonical) {
                require(stats.paired && stats.sprites + stats.playfield_rows + stats.text_rows > 0,
                    "Visible motion without eligible paired geometry");
                frame_visible = true;
                if (alpha == .5f) ++half_visible;
            }
            if (!dumped && frame_visible && !o.dump_dir.empty()) {
                // Save every phase of this ROM frame, not only the first difference.
                for (size_t p = 0; p < phases.size(); ++p) {
                    gpu.draw_motion(scene, phases[p], repeat);
                    png(o.dump_dir / ("frame_" + std::to_string(m.frame) + "_phase_" + std::to_string(p) + ".png"), repeat, o.video);
                    std::cout << "IMAGE frame=" << m.frame << " alpha=" << phases[p] << " crc=0x" << std::hex << hash(repeat) << std::dec << '\n';
                }
                png(o.dump_dir / ("frame_" + std::to_string(m.frame) + "_canonical.png"), canonical, o.video);
                dumped = true;
                png(o.dump_dir / ("frame_" + std::to_string(m.frame) + "_native.png"), m.native_pixels(), f3rt::GameVideoOptions{});
            }
        }
        visible += frame_visible;
        require(before == state(m) && pixels == m.native_pixels(), "GPU presentations mutated serialized state/native pixels");
        if (m.frame == o.frames) {
            replay_post = before; replay_audio = sound; replay_pixels = pixels;
            replay_sync_crc = m.sync_state_crc();
        }
    }
    if (!o.dump_dir.empty()) {
        const auto &final_scene = m.game_video->captured_frame();
        for (unsigned phase = 0; phase < 3; ++phase) {
            gpu.draw_motion(final_scene, float(phase) / 2, out);
            png(o.dump_dir / ("final_" + std::to_string(m.frame) + "_phase_" + std::to_string(phase) + ".png"), out, o.video);
        }
        png(o.dump_dir / ("final_" + std::to_string(m.frame) + "_native.png"), m.native_pixels(), f3rt::GameVideoOptions{});
    }
    require(paired && visible && half_visible && eligible, "No visible paired ROM midpoint motion; use a longer seeded gameplay run");
    const auto &scene = m.game_video->captured_frame();
    text_sampling(gpu, scene, o.video);
    auto snap = [&](const std::string &label) {
        gpu.draw(scene, canonical); gpu.draw_motion(scene, .5f, out);
        require(out == canonical && !gpu.last_motion().paired, label + " must snap to canonical");
    };
    gpu.reset_motion(); gpu.capture_motion(scene, m.frame); snap("reset");
    gpu.capture_motion(scene, m.frame + 2); snap("gap");
    gpu.capture_motion(scene, m.frame + 2); snap("duplicate frame");
    gpu.capture_motion(scene, m.frame - 1); snap("rollback identity");
    auto fallback_scene = std::make_unique<f3rt::CapturedFrame>(scene); fallback_scene->fallback = true;
    gpu.capture_motion(*fallback_scene, m.frame); gpu.capture_motion(scene, m.frame + 1); snap("fallback recovery");
    m.load_state(replay_pre); gpu.reset_motion(); advance(m);
    require(audio(m) == replay_audio, "Rollback replay native audio mismatch");
    require(state(m) == replay_post && m.native_pixels() == replay_pixels, "Rollback replay canonical state/native pixels mismatch");
    require(m.sync_state_crc() == replay_sync_crc, "Rollback replay sync state mismatch");
    gpu.capture_motion(m.game_video->captured_frame(), m.frame); snap("state-load replay");
    std::cout << "SUCCESS frames=" << m.frame << " seed=" << o.seed << " samples=" << samples
        << " supported=" << supported << " fallback=" << fallback << " paired=" << paired
        << " visible_intermediate_frames=" << visible << " visible_midpoint_frames=" << half_visible
        << " eligible_geometry_draws=" << eligible
        << " sprite_geometry_draws=" << sprite_draws << " playfield_row_draws=" << playfield_draws
        << " text_row_draws=" << text_draws
        << " moving_samples=" << moving_samples << " accepted_moving_samples=" << accepted_moving_samples
        << " visible_moving_samples=" << visible_moving_samples
        << " sampled_moving_sprites=" << census.moving_sprites
        << " sampled_moving_pf_rows=" << census.moving_playfield_rows
        << " sampled_moving_text_rows=" << census.moving_text_rows
        << " sprite_identity_rejections=" << census.sprite_identity_rejections
        << " sprite_count_subset=" << census.sprite_count_rejections
        << " sprite_transform_rejections=" << census.sprite_transform_rejections
        << " sprite_jump_rejections=" << census.sprite_jump_rejections
        << " row_control_rejections=" << census.row_control_rejections
        << " row_transform_rejections=" << census.row_transform_rejections
        << " row_jump_rejections=" << census.row_jump_rejections
        << " synthetic_phase_grids=halves,twelfths alpha1=exact repeated_draws=exact state_native_audio_parity=exact"
        << " reset_gap_duplicate_rollback_fallback=snap replay=exact native_blocks=" << m.native_blocks
        << " fallback_instructions=" << m.fallback_instructions << " audio_samples=" << audio_samples
        << " audio_crc=0x" << std::hex << (audio_crc ^ 0xffffffffu) << " native_crc=0x" << hash(m.native_pixels())
        << " state_crc=0x" << m.state_crc() << " sync_state_crc=0x" << m.sync_state_crc() << std::dec << '\n';
    return 0;
} catch (const std::exception &e) {
    std::cerr << "MOTION REGRESSION ERROR: " << e.what() << '\n'; return 1;
}
