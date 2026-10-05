#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/game_video.hpp"
#include "f3rt/video.hpp"
#include "f3rt/rom.hpp"
#include "gpu_video.hpp"
#include "gpu_motion.hpp"
#include "gameplay_inputs.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <limits>
#include <memory>
#include <span>
#include <stdexcept>
#include <string>
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
                "Short runs without visible paired ROM motion intentionally fail.\n";
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
        {f3_sound_excluded_ranges, f3_sound_excluded_count});
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
// Consumer-visible helper geometry: no machine writes, mocks, or implementation
// text assertions. Independently constructed submitted slots and packed rows.
void boundaries() {
    auto a = std::make_unique<f3rt::GpuScene>();
    auto b = std::make_unique<f3rt::GpuScene>();
    a->fallback = false; a->sprite_count = 1;
    const unsigned sp = f3rt::GpuScene::sprites;
    a->words[sp] = 100 * 256; a->words[sp + 1] = 80 * 256;
    a->words[sp + 2] = a->words[sp + 3] = 256;
    a->words[sp + 4] = 7; a->words[sp + 5] = 2;
    auto evaluate = [&](float alpha) {
        f3rt::GpuMotionHistory h; h.capture(*a, 10); h.capture(*b, 11);
        std::vector<uint32_t> out(b->words.begin(), b->words.end());
        h.apply(*b, alpha, out); return out;
    };
    *b = *a; b->words[sp] += 32 * 256;
    require(evaluate(.5f)[sp] == 116 * 256, "32px sprite boundary must interpolate");
    require(evaluate(0)[sp] == a->words[sp] && evaluate(1)[sp] == b->words[sp], "Sprite endpoints");
    b->words[sp] += 1;
    require(evaluate(.5f)[sp] == b->words[sp], "Over-32px sprite movement must snap");
    for (unsigned field : {2u, 3u, 4u, 5u, 6u}) {
        *b = *a; b->words[sp] += 4 * 256; ++b->words[sp + field];
        require(evaluate(.5f)[sp] == b->words[sp], "Sprite identity/scale/flip change must snap");
    }
    *b = *a; b->words[sp] += 4 * 256; b->sprite_count = 2;
    require(evaluate(.5f)[sp] == b->words[sp], "Submitted slot count change must snap");
    *b = *a; b->words[sp] += 4 * 256; b->pen_mask ^= 16;
    require(evaluate(.5f)[sp] == b->words[sp], "Pen mask change must snap");
    *b = *a; b->words[sp] += 4 * 256;
    require(evaluate(std::numeric_limits<float>::quiet_NaN()) == std::vector<uint32_t>(b->words.begin(), b->words.end()), "NaN alpha must be canonical");
    // One visible playfield row, with matching layer controls and valid zoom.
    const unsigned row = f3rt::GpuScene::rows + 24 * f3rt::GpuScene::row_stride;
    const unsigned pf = row + f3rt::GpuScene::row_pf;
    a->words[row + f3rt::GpuScene::row_layers + 1] = 1;
    a->words[row + f3rt::GpuScene::row_layers + 2] = 46;
    a->words[row + f3rt::GpuScene::row_layers + 3] = 366;
    a->words[row + f3rt::GpuScene::row_layers] = 64;
    a->words[pf] = 100 * 256; a->words[pf + 1] = 80;
    a->words[pf + 2] = a->words[pf + 3] = 256;
    *b = *a; b->words[pf] += 4 * 256; b->words[pf + 4] = 128;
    auto out = evaluate(.5f);
    require(out[pf] == 102 * 256 && out[pf + 1] == 80 && out[pf + 4] == 64, "PF fixed-point XY interpolation");
    for (unsigned field : {row + 1, row + 4, row + 5, row + f3rt::GpuScene::row_layers,
            row + f3rt::GpuScene::row_layers + 1, row + f3rt::GpuScene::row_layers + 2, pf + 2, pf + 3}) {
        *b = *a; b->words[pf] += 4 * 256; ++b->words[field];
        require(evaluate(.5f)[pf] == b->words[pf], "PF control/clip/order/zoom change must snap");
    }
    *b = *a; a->words[pf] = 1023 * 256; b->words[pf] = 1025 * 256;
    require(evaluate(.5f)[pf] == b->words[pf], "PF raw period crossing must snap");
    a->words[pf] = 100 * 256;
    *b = *a; b->words[pf] += 4 * 256; b->words[pf + 5] = 64;
    require(evaluate(.5f)[pf] == 102 * 256 && evaluate(.5f)[pf + 5] == 64,
        "Current discrete PF palette must survive geometry interpolation");
    b->reference_rows[24].bitmap = true;
    require(evaluate(.5f)[pf] == b->words[pf], "Bitmap row must snap");
    *b = *a; a->words[pf + 1] = 511; a->words[pf + 4] = 240;
    b->words[pf + 1] = 0; b->words[pf + 4] = 16;
    require(evaluate(.5f)[pf + 1] == 0 && evaluate(.5f)[pf + 4] == 16, "PF vertical wrap must snap");
    // Text uses separate motion-only fixed-point coordinates. The canonical
    // integer scroll and current glyph/cell data are never rewritten.
    const unsigned text_layer = row + f3rt::GpuScene::row_layers + 8 * f3rt::GpuScene::layer_stride;
    a->words[text_layer] = 64; a->words[text_layer + 1] = 1;
    a->words[text_layer + 2] = 46; a->words[text_layer + 3] = 366;
    a->words[row + 2] = 100; a->words[row + 3] = 80;
    *b = *a; b->words[row + 2] = 103; b->words[row + 3] = 81;
    out = evaluate(.5f);
    require(out[row + 14] == 101 * 256 + 128 && out[row + 15] == 80 * 256 + 128 &&
        out[row + 2] == 103 && out[row + 3] == 81, "Text fractional geometry and canonical scroll preservation");
    ++b->words[text_layer + 2];
    require(evaluate(.5f)[row + 14] == 103 * 256, "Text clipping change must snap");
    *b = *a; a->words[row + 2] = 511; b->words[row + 2] = 0;
    require(evaluate(.5f)[row + 14] == 0, "Text period wrap must snap");
    std::cout << "BOUNDARIES sprite_endpoints=exact max_delta=32 identity_scale_flip_count_pen= snap pf_fraction=exact pf_controls_wrap=snap invalid_alpha=canonical\n";
}
void text_sampling(f3rt::GpuVideo &gpu, const f3rt::GpuScene &source, f3rt::GameVideoOptions o) {
    if (o.scale % 2) return; // Half-native-pixel translation is an integer output shift at even scales.
    auto a = std::make_unique<f3rt::GpuScene>(source);
    uint32_t cell = 0; bool found = false;
    for (unsigned i = 0; i < 4096 && !found; ++i) {
        cell = source.words[f3rt::GpuScene::text_cells + i];
        const unsigned glyph = f3rt::GpuScene::glyphs + (cell & 255u) * 16;
        for (unsigned p = 0; p < 64; ++p) {
            const unsigned pen = (source.words[glyph + p / 4] >> ((p % 4) * 8)) & 255u;
            if (pen && source.words[f3rt::GpuScene::palette + ((((cell >> 8) & 255u) * 16 + pen) & 8191u)]) {
                found = true; break;
            }
        }
    }
    require(found, "Text pixel proof needs a visible captured ROM glyph");
    a->fallback = false; a->sprite_count = 0;
    a->words[f3rt::GpuScene::palette] = 0;
    std::fill_n(a->words.begin() + f3rt::GpuScene::text_cells, 4096, cell);
    std::fill(a->words.begin() + f3rt::GpuScene::rows, a->words.end(), 0u);
    a->reference_rows = {};
    for (unsigned y = 24; y < 256; ++y) {
        const unsigned row = f3rt::GpuScene::rows + y * f3rt::GpuScene::row_stride;
        a->words[row + 3] = y - 24; a->words[row + 4] = 0x08080808u;
        for (unsigned i = 0; i < 9; ++i) a->words[row + 5 + i] = i;
        const unsigned layer = row + f3rt::GpuScene::row_layers + 8 * f3rt::GpuScene::layer_stride;
        a->words[layer] = 64u | 16u | 7u; a->words[layer + 1] = 1;
        a->words[layer + 2] = 46; a->words[layer + 3] = 366;
    }
    auto b = std::make_unique<f3rt::GpuScene>();
    std::vector<uint32_t> before(size_t(o.width()) * o.height()), after(before.size()), middle(before.size());
    gpu.draw(*a, before, 256);
    for (unsigned axis = 0; axis < 2; ++axis) {
        *b = *a;
        for (unsigned y = 24; y < 256; ++y)
            ++b->words[f3rt::GpuScene::rows + y * f3rt::GpuScene::row_stride + 2 + axis];
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
}

int main(int argc, char **argv) try {
    auto o = parse(argc, argv); boundaries(); SdlLifetime sdl;
    auto owner = machine(o), reference = machine(o); auto &m = *owner;
    f3rt::GpuVideo gpu(nullptr, o.video, m.video->playfield_tiles(), m.video->sprite_tiles(),
        false, false, o.interp);
    if (!o.dump_dir.empty()) std::filesystem::create_directories(o.dump_dir);
    std::vector<uint32_t> canonical(size_t(o.video.width()) * o.video.height()), out(canonical.size()), repeat(out.size());
    f3rt::test::GameplaySchedule schedule(o.seed, f3rt::test::ScheduleConfig{.versus = false});
    uint64_t samples = 0, paired = 0, visible = 0, eligible = 0, supported = 0, fallback = 0, audio_samples = 0;
    uint64_t sprite_draws = 0, playfield_draws = 0, text_draws = 0;
    uint32_t audio_crc = 0xffffffffu; bool dumped = false;
    std::vector<uint8_t> replay_pre, replay_post; std::vector<int16_t> replay_audio;
    std::array<uint32_t, 320 * 232> replay_pixels{};
    uint32_t replay_sync_crc = 0;
    uint64_t half_visible = 0;
    std::cout << "driver=" << gpu.driver() << " seed=" << o.seed << " scale=" << o.video.scale << '\n';
    while (m.frame < o.frames) {
        auto word = schedule.step(m.frame)[0]; inputs(m, word); inputs(*reference, word);
        if (m.frame + 1 == o.frames) replay_pre = state(m);
        advance(m); advance(*reference);
        const auto &scene = m.game_video->gpu_scene();
        if (scene.fallback) ++fallback; else ++supported;
        gpu.capture_motion(scene, m.frame);
        auto sound = audio(m), sound_reference = audio(*reference);
        require(sound == sound_reference, "Native audio parity frame " + std::to_string(m.frame));
        audio_samples += sound.size();
        for (int16_t sample : sound) for (unsigned shift : {0u, 8u}) {
            audio_crc ^= uint8_t(uint16_t(sample) >> shift);
            for (unsigned bit = 0; bit < 8; ++bit) audio_crc = (audio_crc >> 1) ^ ((audio_crc & 1) ? 0xedb88320u : 0u);
        }
        require(m.pixels == reference->pixels, "Native pixel parity frame " + std::to_string(m.frame));
        if ((m.frame - 1) % o.every != 0 && m.frame != o.frames) continue;
        ++samples; const auto before = state(m); const auto pixels = m.pixels;
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
                png(o.dump_dir / ("frame_" + std::to_string(m.frame) + "_native.png"), m.pixels, f3rt::GameVideoOptions{});
            }
        }
        visible += frame_visible;
        require(before == state(m) && pixels == m.pixels, "GPU presentations mutated serialized state/native pixels");
        if (m.frame == o.frames) {
            replay_post = before; replay_audio = sound; replay_pixels = pixels;
            replay_sync_crc = m.sync_state_crc();
        }
    }
    if (!o.dump_dir.empty()) {
        const auto &final_scene = m.game_video->gpu_scene();
        for (unsigned phase = 0; phase < 3; ++phase) {
            gpu.draw_motion(final_scene, float(phase) / 2, out);
            png(o.dump_dir / ("final_" + std::to_string(m.frame) + "_phase_" + std::to_string(phase) + ".png"), out, o.video);
        }
        png(o.dump_dir / ("final_" + std::to_string(m.frame) + "_native.png"), m.pixels, f3rt::GameVideoOptions{});
    }
    require(paired && visible && half_visible && eligible, "No visible paired ROM midpoint motion; use a longer seeded gameplay run");
    const auto &scene = m.game_video->gpu_scene();
    text_sampling(gpu, scene, o.video);
    auto snap = [&](const std::string &label) {
        gpu.draw(scene, canonical); gpu.draw_motion(scene, .5f, out);
        require(out == canonical && !gpu.last_motion().paired, label + " must snap to canonical");
    };
    gpu.reset_motion(); gpu.capture_motion(scene, m.frame); snap("reset");
    gpu.capture_motion(scene, m.frame + 2); snap("gap");
    gpu.capture_motion(scene, m.frame + 2); snap("duplicate frame");
    gpu.capture_motion(scene, m.frame - 1); snap("rollback identity");
    auto fallback_scene = std::make_unique<f3rt::GpuScene>(scene); fallback_scene->fallback = true;
    gpu.capture_motion(*fallback_scene, m.frame); gpu.capture_motion(scene, m.frame + 1); snap("fallback recovery");
    m.load_state(replay_pre); gpu.reset_motion(); advance(m);
    require(audio(m) == replay_audio, "Rollback replay native audio mismatch");
    require(state(m) == replay_post && m.pixels == replay_pixels, "Rollback replay canonical state/native pixels mismatch");
    require(m.sync_state_crc() == replay_sync_crc, "Rollback replay sync state mismatch");
    gpu.capture_motion(m.game_video->gpu_scene(), m.frame); snap("state-load replay");
    std::cout << "SUCCESS frames=" << m.frame << " seed=" << o.seed << " samples=" << samples
        << " supported=" << supported << " fallback=" << fallback << " paired=" << paired
        << " visible_intermediate_frames=" << visible << " visible_midpoint_frames=" << half_visible
        << " eligible_geometry_draws=" << eligible
        << " sprite_geometry_draws=" << sprite_draws << " playfield_row_draws=" << playfield_draws
        << " text_row_draws=" << text_draws
        << " synthetic_phase_grids=halves,twelfths alpha1=exact repeated_draws=exact state_native_audio_parity=exact"
        << " reset_gap_duplicate_rollback_fallback=snap replay=exact native_blocks=" << m.native_blocks
        << " fallback_instructions=" << m.fallback_instructions << " audio_samples=" << audio_samples
        << " audio_crc=0x" << std::hex << (audio_crc ^ 0xffffffffu) << " native_crc=0x" << hash(m.pixels)
        << " state_crc=0x" << m.state_crc() << " sync_state_crc=0x" << m.sync_state_crc() << std::dec << '\n';
    return 0;
} catch (const std::exception &e) {
    std::cerr << "MOTION REGRESSION ERROR: " << e.what() << '\n'; return 1;
}
