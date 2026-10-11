#include "commands.hpp"
#include "../runner/runner.hpp"
#include "../runner/inputs.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/audio.hpp"
#include "f3rt/game_video.hpp"
#include "f3rt/video.hpp"
#include "f3rt/rom.hpp"
#include "renderer/gpu/video.hpp"
#include "renderer/gpu/interp.hpp"
#include "capture_io.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <memory>
#include <numeric>
#include <fstream>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#ifdef F3RT_GENERATED
#include "program.h"
#endif
#ifdef F3RT_SOUND_GENERATED
#include "sound_program.h"
#endif

namespace f3rt::tool {
namespace {
using Clock = std::chrono::steady_clock;
constexpr std::array<const char *, f3rt::layer_count + 1> names = [] {
    std::array<const char *, f3rt::layer_count + 1> result{};
    for (unsigned i = 0; i < f3rt::layer_count; ++i) result[i] = f3rt::layer_info[i].name;
    result[f3rt::layer_count] = "composite";
    return result;
}();
struct Options {
    std::filesystem::path rom_dir, dump_dir;
    uint64_t seed = 12345, frames = 4000, every = 1, inject_frame = 1407;
    f3rt::GameVideoOptions video;
    bool layers = false, bench = false;
    f3rt::VideoInterpolation interpolation = f3rt::VideoInterpolation::Off;
    f3rt::InterpolationFields fields = f3rt::InterpolationFields::Geometry;
    std::vector<uint64_t> capture_frames;
    std::vector<std::pair<uint64_t, unsigned>> scale_changes;
    std::array<bool, 4> injections{};
    bool json_output = false;
};
uint64_t number(const std::string &text) {
    if (text.empty() || text.front() == '-') throw std::runtime_error("Expected unsigned integer: " + text);
    size_t used = 0;
    auto value = std::stoull(text, &used, 0);
    if (used != text.size()) throw std::runtime_error("Invalid integer: " + text);
    return value;
}
Options parse(int argc, char **argv) {
    Options o;
#ifdef F3RT_DEFAULT_ROM_DIR
    o.rom_dir = F3RT_DEFAULT_ROM_DIR;
#endif
    if (const char *seed = std::getenv("SEED"); seed && *seed) o.seed = number(seed);
    for (int i = 0; i < argc; ++i) {
        const std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (++i >= argc) throw std::runtime_error("Missing value for " + arg);
            return argv[i];
        };
        if (arg == "--rom-dir") o.rom_dir = value();
        else if (arg == "--json") o.json_output = true;
        else if (arg == "--dump-dir") o.dump_dir = value();
        else if (arg == "--seed") o.seed = number(value());
        else if (arg == "--frames") o.frames = number(value());
        else if (arg == "--every") o.every = number(value());
        else if (arg == "--capture-frame") o.capture_frames.push_back(number(value()));
        else if (arg == "--change-scale") {
            const auto change = value();
            const auto colon = change.find(':');
            if (colon == std::string::npos) throw std::runtime_error("Scale change must be FRAME:SCALE");
            const auto frame = number(change.substr(0, colon)), scale = number(change.substr(colon + 1));
            if (!frame || !scale || scale > f3rt::GameVideoOptions::max_gpu_scale)
                throw std::runtime_error("Scale change needs a positive frame and scale 1..8");
            o.scale_changes.emplace_back(frame, unsigned(scale));
        }
        else if (arg == "--interp") {
            const auto mode = value();
            if (mode == "off") o.interpolation = f3rt::VideoInterpolation::Off;
            else if (mode == "linear") o.interpolation = f3rt::VideoInterpolation::Linear;
            else if (mode == "fit") o.interpolation = f3rt::VideoInterpolation::Fit;
            else throw std::runtime_error("Interpolation must be off, linear or fit");
        }
        else if (arg == "--interp-fields") {
            const auto fields = f3rt::parse_interpolation_fields(value());
            if (!fields) throw std::runtime_error("--interp-fields must be none, geometry, palette or geometry,palette");
            o.fields = *fields;
        }
        else if (arg == "--scale") {
            auto n = number(value());
            if (!n || n > f3rt::GameVideoOptions::max_gpu_scale) throw std::runtime_error("Diagnostic GPU scale must be 1..8");
            o.video.scale = unsigned(n);
        } else if (arg == "--border") {
            auto n = number(value());
            if (n > f3rt::GameVideoOptions::max_border) throw std::runtime_error("Border must be 0..160");
            o.video.border = unsigned(n);
        } else if (arg == "--layers") o.layers = true;
        else if (arg == "--bench") o.bench = true;
        else if (arg == "--inject-frame") o.inject_frame = number(value());
        else if (arg == "--inject-bitmap") o.injections[0] = true;
        else if (arg == "--inject-trails") o.injections[1] = true;
        else if (arg == "--inject-globalflip") o.injections[2] = true;
        else if (arg == "--inject-sprite-boundaries") o.injections[3] = true;
        else if (arg == "--sound-driver") {
            if (value() != "native") throw std::runtime_error("GPU regression requires --sound-driver native");
        } else if (arg == "--help" || arg == "-h") {
            std::cout << "Strict-native GPU parity for the configured game (injection scenarios are Land Maker scenes); optional presentation-only interpolation.\n"
                "--rom-dir DIR --seed N --frames N (4000) --scale N (1..8 diagnostic) --border N (0..160)\n"
                "--every N (1) --layers (all nine isolated contributions plus composite)\n"
                "--bench (600-frame varied-scene warmup; 100 repeats on final supported snapshot)\n"
                "--dump-dir DIR (external PNG captures) --sound-driver native\n"
                "--interp off|linear|fit (off) --capture-frame N (repeatable; requires --dump-dir)\n"
                "--interp-fields none|geometry|palette|geometry,palette (geometry; native alpha stays discrete)\n"
                "--change-scale FRAME:SCALE (repeatable; constructor/canonical scale remains 1)\n"
                "--inject-frame N (1407) --inject-bitmap --inject-trails --inject-globalflip\n"
                "--inject-sprite-boundaries (ROM-texel row order, flips, collapsed spans, overlap, edge clipping/cull)\n"
                "Run each scale 1..4 with border 0 and 48 for the parity matrix.\n";
            std::exit(0);
        } else throw std::runtime_error("Unknown argument: " + arg);
    }
    if (o.rom_dir.empty() || !o.frames || !o.every) throw std::runtime_error("ROM directory, positive frames and positive every are required");
    if (std::any_of(o.injections.begin(), o.injections.end(), [](bool b) { return b; }) &&
        (!o.inject_frame || o.inject_frame > o.frames)) throw std::runtime_error("Injection frame must be inside run (1..frames)");
    for (const auto frame : o.capture_frames)
        if (!frame || frame > o.frames || o.dump_dir.empty())
            throw std::runtime_error("Capture frames must be inside run and require external --dump-dir");
    std::sort(o.scale_changes.begin(), o.scale_changes.end());
    for (size_t i = 0; i < o.scale_changes.size(); ++i) {
        if (o.scale_changes[i].first > o.frames ||
            (i && o.scale_changes[i].first == o.scale_changes[i - 1].first))
            throw std::runtime_error("Scale change frames must be unique and inside run");
    }
    return o;
}
void advance(f3rt::Machine &m) {
    if (!m.run_frame(true) || m.cpu.halted || m.fallback_instructions)
        throw std::runtime_error("Strict-native frame failed at frame " + std::to_string(m.frame) + " PC " + std::to_string(m.cpu.pc));
}
std::vector<uint8_t> snapshot(const f3rt::Machine &m) {
    std::vector<uint8_t> state(m.state_size());
    m.save_state(state);
    return state;
}
void capture_png(const std::filesystem::path &path, std::span<const uint32_t> pixels, f3rt::GameVideoOptions o) {
    auto *surface = SDL_CreateSurfaceFrom(int(o.width()), int(o.height()), SDL_PIXELFORMAT_ARGB8888,
        const_cast<uint32_t *>(pixels.data()), int(o.width() * 4));
    if (!surface) throw std::runtime_error(SDL_GetError());
    const bool saved = SDL_SavePNG(surface, path.string().c_str());
    SDL_DestroySurface(surface);
    if (!saved) throw std::runtime_error("PNG write failed: " + path.string() + ": " + SDL_GetError());
}
const char *mode_name(f3rt::VideoInterpolation mode) {
    return mode == f3rt::VideoInterpolation::Fit ? "fit" :
        mode == f3rt::VideoInterpolation::Linear ? "linear" : "off";
}
void require_exact(std::span<const uint32_t> a, std::span<const uint32_t> b, const std::string &context) {
    for (size_t i = 0; i < a.size(); ++i)
        if (a[i] != b[i]) throw std::runtime_error(context + " differs at pixel " + std::to_string(i));
}
// Deterministic scene-copy boundaries with visible ROM texels, not machine mutations.
// Returns false while the live scene has no enabled playfield row yet (boot frames
// decode line RAM before the first playfield is turned on), so the caller retries
// on a later sample instead of failing on an empty boot scene.
bool interpolation_boundaries(f3rt::GpuVideo &off, f3rt::GpuVideo &selected, f3rt::GpuVideo &geometry,
        const f3rt::CapturedFrame &scene, f3rt::GameVideoOptions options, uint64_t frame,
        std::span<const uint8_t> tiles) {
    auto base = std::make_unique<f3rt::CapturedFrame>(scene);
    auto copy = std::make_unique<f3rt::CapturedFrame>(scene);
    unsigned plane = 0, anchor = 256;
    for (unsigned pf = 0; pf < 4 && anchor == 256; ++pf)
        for (unsigned y = 24; y < 256; ++y) {
            const auto &p = scene.rows[y].playfields[pf];
            if (p.layer.enabled && !p.layer.mosaic && !scene.rows[y].bitmap) {
                plane = pf; anchor = y; break;
            }
        }
    if (anchor == 256) return false;
    unsigned tile = 0;
    for (; tile < 32768; ++tile) {
        bool varying = false;
        for (unsigned p = 1; p < 256; ++p)
            if (p % 16 && (tiles[tile * 256 + p] & 63) != (tiles[tile * 256 + p - 1] & 63)) {
                varying = true; break;
            }
        if (varying) break;
    }
    if (tile == 32768) throw std::runtime_error("No horizontally varying ROM tile for visible guard fixture");
    for (unsigned cell = 0; cell < 2048; ++cell) {
        // Raw cell: palette 0, extra planes 3 (pen mask 63), no flips/blend.
        base->tiles.cells(plane)[cell] = (0x0C00u << 16) | (tile & 65535u);
    }
    for (unsigned y = 0; y < 256; ++y) {
        base->rows[y] = scene.rows[anchor];
        auto &p = base->rows[y].playfields[plane];
        p.source_x = int32_t(y * 256); p.source_y = int32_t(y);
        p.x_step = 128; p.y_step = 256; p.y_fraction = 0; p.palette_add = uint16_t(y / 8 * 64);
    }
    for (unsigned i = 0; i < 8192; ++i) {
        const unsigned channel = 16 + (i % 64) * 2 + (i / 64) % 32;
        base->colors[i] = channel * 0x010101;
    }
    std::vector<uint32_t> reference(size_t(options.width()) * options.height()), result(reference.size()),
        geometry_result(reference.size());
    off.draw(*base, reference, 1u << plane);
    geometry.draw(*base, geometry_result, 1u << plane);
    selected.draw(*base, result, 1u << plane);
    const auto accepted = result;
    if (geometry_result == reference) throw std::runtime_error("Safe source fixture had no visible geometry sampling");
    if (result == geometry_result) throw std::runtime_error("Compatible fixture had no visible palette blending");
    for (unsigned sy : {24u, 255u}) {
        const size_t at = size_t(sy - 24) * options.scale * options.width();
        require_exact(std::span(reference).subspan(at, options.width() * options.scale),
            std::span(result).subspan(at, options.width() * options.scale), "Raw fixture run endpoint");
    }
    constexpr std::array labels{"disabled-endpoint", "zero-endpoint", "garbage-endpoint",
        "hard-source-jump", "hard-control-jump", "column-phase-jump", "unsafe-palette-safe-geometry"};
    for (unsigned kind = 0; kind < labels.size(); ++kind) {
        *copy = *base;
        auto &p = copy->rows[80].playfields[plane];
        if (kind == 0) p.layer.enabled = false;
        else if (kind < 3) p.x_step = kind == 1 ? 0 : int32_t(0x7fffffff);
        else if (kind == 3) {
            for (unsigned y = 80; y < 256; ++y) copy->rows[y].playfields[plane].source_x += 16384;
        } else if (kind == 4) p.layer.priority ^= 1;
        else if (kind == 5) {
            unsigned phase = 0;
            for (unsigned y = 0; y < 256; ++y) {
                auto &v = copy->rows[y].playfields[plane];
                v.y_step = int32_t(64 + y); v.source_y = int32_t((phase >> 8) & 511); v.y_fraction = uint8_t(phase);
                phase += unsigned(v.y_step);
            }
            p.source_y += 32;
        } else {
            for (unsigned i = 0; i < 8192; ++i) {
                const unsigned channel = (((i / 64) & 1) ? 180 : 20) + i % 64;
                copy->colors[i] = channel * 0x010101;
            }
        }
        off.draw(*copy, reference, 1u << plane); selected.draw(*copy, result, 1u << plane);
        geometry.draw(*copy, geometry_result, 1u << plane);
        const auto stats = selected.last_interpolation();
        if (kind != 6) for (unsigned sy : {79u, 80u}) {
            const size_t at = size_t(sy - 24) * options.scale * options.width();
            require_exact(std::span(reference).subspan(at, options.width() * options.scale),
                std::span(geometry_result).subspan(at, options.width() * options.scale), labels[kind]);
        }
        if (kind == 6) {
            require_exact(geometry_result, result, "Unsafe palette must preserve geometry without RGB blending");
            if (geometry_result == reference) throw std::runtime_error("Unsafe palette disabled visible safe geometry");
        }
        for (unsigned y = 0; y < options.height(); ++y) {
            const unsigned sy = y / options.scale + 24;
            const size_t at = size_t(y) * options.width();
            if (y % options.scale == 0 || !stats.row_fields[plane][sy])
                require_exact(std::span(reference).subspan(at, options.width()),
                    std::span(result).subspan(at, options.width()), labels[kind]);
            if (kind < 3 && sy >= 100)
                require_exact(std::span(accepted).subspan(at, options.width()),
                    std::span(result).subspan(at, options.width()), "Invalid row leaked beyond neighbors");
        }
        std::cout << "INTERP induced_boundary=" << labels[kind] << " frame=" << frame
            << " pf=" << plane << " native_and_unflagged=exact\n";
    }
    return true;
}
struct Harness {
    f3rt::Machine &m;
    f3rt::GpuVideo &gpu;
    Options &o;
    const f3rt::GameVideoOptions canonical_video;
    std::vector<uint32_t> cpu, device;
    std::vector<uint8_t> state_before, state_after;
    std::vector<double> cpu_frame_ms, gpu_frame_ms;
    std::vector<double> cpu_budget_ms, gpu_budget_ms;
    double native_ms = 0;
    std::array<uint64_t, 10> samples{}, mismatches{};
    uint64_t sampled_frames = 0, fallback_samples = 0, supported_samples = 0;
    uint64_t injected_samples = 0, captures = 0;
    std::unique_ptr<f3rt::GpuVideo> interpolated_gpu;
    std::unique_ptr<f3rt::CapturedFrame> canonical_guard;
    std::vector<uint32_t> interpolated, sprite_off, sprite_selected;
    std::vector<double> interpolation_ms;
    std::array<uint64_t, size_t(f3rt::InterpolationReason::Applied) + 1> interpolation_reasons{};
    std::array<std::array<uint64_t, size_t(f3rt::InterpolationReason::Applied) + 1>, 4> layer_reasons{};
    std::array<std::array<uint64_t, 7>, 4> layer_totals{};
    f3rt::InterpolationStats previous_interpolation{};
    bool have_interpolation = false, boundaries_checked = false;
    uint64_t sprite_checks = 0;
    std::vector<double> scale_change_ms;
    uint64_t scale_change_frames = 0;
    Harness(f3rt::Machine &machine, f3rt::GpuVideo &video, Options &options, f3rt::GameVideoOptions canonical)
        : m(machine), gpu(video), o(options), canonical_video(canonical),
          cpu(size_t(o.video.width()) * o.video.height()), device(cpu.size()),
          state_before(m.state_size()), state_after(state_before.size()) {
        if (o.bench) {
            cpu_frame_ms.reserve(o.frames + 1); gpu_frame_ms.reserve(o.frames + 1);
            cpu_budget_ms.reserve(o.frames); gpu_budget_ms.reserve(o.frames);
        }
        if (o.interpolation != f3rt::VideoInterpolation::Off) {
            interpolated_gpu = std::make_unique<f3rt::GpuVideo>(nullptr, o.video,
                m.video->playfield_tiles(), m.video->sprite_tiles(), false, true, o.interpolation, o.fields);
            canonical_guard = std::make_unique<f3rt::CapturedFrame>();
            interpolated.resize(cpu.size()); sprite_off.resize(cpu.size()); sprite_selected.resize(cpu.size());
        }
    }
    void set_scale(unsigned scale) {
        m.save_state(state_before);
        const auto start = Clock::now();
        m.game_video->set_gpu_scale(scale);
        gpu.set_scale(scale);
        if (interpolated_gpu) interpolated_gpu->set_scale(scale);
        const double ms = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        const unsigned previous = o.video.scale;
        o.video.scale = scale;
        const size_t count = size_t(o.video.width()) * o.video.height();
        cpu.resize(count); device.resize(count);
        if (interpolated_gpu) {
            interpolated.resize(count); sprite_off.resize(count); sprite_selected.resize(count);
        }
        m.save_state(state_after);
        if (state_before != state_after)
            throw std::runtime_error("Scale change mutated canonical state or snapshot layout");
        if (previous != scale) { scale_change_ms.push_back(ms); ++scale_change_frames; }
        std::cout << "SCALE frame=" << m.frame << " previous=" << previous << " selected=" << scale
                  << " change_ms=" << ms << " canonical_bytes=unchanged\n";
    }
    void capture(const std::string &tag) {
        if (o.dump_dir.empty()) return;
        const auto stem = "frame_" + std::to_string(m.frame) + "_s" + std::to_string(o.video.scale) + "_b" + std::to_string(o.video.border) + "_" + tag;
        capture_png(o.dump_dir / (stem + "_cpu.png"), cpu, o.video);
        capture_png(o.dump_dir / (stem + "_gpu.png"), device, o.video);
        f3rt::write_bmp(o.dump_dir / (stem + "_native.bmp"), m.native_pixels());
    }
    void compare(unsigned index, const std::string &tag, bool injected) {
        const unsigned mask = index == f3rt::layer_count ? f3rt::all_layers : 1u << index;
        m.save_state(state_before);
        const auto cpu_start = Clock::now();
        m.game_video->render_reference(cpu, o.video, mask);
        const double cpu_ms = std::chrono::duration<double, std::milli>(Clock::now() - cpu_start).count();
        m.save_state(state_after);
        if (state_after != state_before) throw std::runtime_error("CPU reference mutated native state at frame " + std::to_string(m.frame));
        const auto gpu_start = Clock::now();
        gpu.draw(m.game_video->captured_frame(), device, mask);
        const double gpu_ms = std::chrono::duration<double, std::milli>(Clock::now() - gpu_start).count();
        if (o.bench && m.frame > 600 && index == 9 && !injected && !m.game_video->captured_frame().fallback) {
            cpu_frame_ms.push_back(cpu_ms); gpu_frame_ms.push_back(gpu_ms);
            if (native_ms > 0) {
                cpu_budget_ms.push_back(native_ms + cpu_ms);
                gpu_budget_ms.push_back(native_ms + gpu_ms);
            }
        }
        m.save_state(state_after);
        if (state_after != state_before) throw std::runtime_error("GPU presentation mutated native state at frame " + std::to_string(m.frame));
        if (index == 9 && m.game_video->captured_frame().fallback) {
            // Independently check the fallback contract, not just two implementations
            // that might both expand an incoherent or incorrectly bordered snapshot.
            const unsigned left = o.video.border * o.video.scale;
            for (unsigned y = 0; y < o.video.height(); ++y)
                for (unsigned x = 0; x < o.video.width(); ++x) {
                    const uint32_t expected = x < left || x >= left + 320 * o.video.scale
                        ? 0xff000000u : m.native_pixels()[(y / o.video.scale) * 320 + (x - left) / o.video.scale];
                    if (cpu[size_t(y) * o.video.width() + x] != expected)
                        throw std::runtime_error("CPU fallback reference disagrees with actual native oracle at frame " +
                            std::to_string(m.frame) + " x=" + std::to_string(x) + " y=" + std::to_string(y));
                }
        }
        uint64_t count = 0;
        size_t first = 0;
        for (size_t i = 0; i < cpu.size(); ++i) if (cpu[i] != device[i]) {
            if (!count) first = i;
            ++count;
        }
        ++samples[index]; mismatches[index] += count;
        if (count) {
            std::cerr << "FIRST MISMATCH seed=" << o.seed << " frame=" << m.frame << " layer=" << names[index]
                << " scenario=" << tag << " injected=" << injected << " fallback=" << m.game_video->captured_frame().fallback
                << " count=" << count << " x=" << first % o.video.width() << " y=" << first / o.video.width()
                << " cpu_argb=0x" << std::hex << std::setw(8) << std::setfill('0') << cpu[first]
                << " gpu_argb=0x" << std::setw(8) << device[first] << std::dec << std::setfill(' ') << '\n';
            capture(tag + "_mismatch_" + names[index]);
            throw std::runtime_error("GPU parity mismatch");
        }
    }
    bool requested_capture() const {
        return std::find(o.capture_frames.begin(), o.capture_frames.end(), m.frame) != o.capture_frames.end();
    }
    void capture_interpolation() {
        const auto &scene = m.game_video->captured_frame();
        const auto stem = "frame_" + std::to_string(m.frame) + "_s" + std::to_string(o.video.scale) +
            "_b" + std::to_string(o.video.border);
        capture_png(o.dump_dir / (stem + "_off.png"), device, o.video);
        capture_png(o.dump_dir / (stem + "_" + mode_name(o.interpolation) + ".png"),
            interpolated_gpu ? std::span<const uint32_t>(interpolated) : std::span<const uint32_t>(device), o.video);
        std::ofstream csv(o.dump_dir / (stem + "_" + mode_name(o.interpolation) + "_rows.csv"));
        if (!csv) throw std::runtime_error("Cannot open playfield capture CSV");
        csv << "frame,pf,screen_y,visible_y,field_mask,enabled,bitmap,mosaic,source_x,source_y,x_step,y_step,y_fraction,palette_add,priority,blend_mode,clip_enabled,clip_inverted\n";
        const auto stats = interpolated_gpu ? interpolated_gpu->last_interpolation() : f3rt::InterpolationStats{};
        sprite_off.resize(device.size()); sprite_selected.resize(device.size());
        for (unsigned layer = 0; layer < 9; ++layer) {
            gpu.draw(scene, sprite_off, 1u << layer);
            if (interpolated_gpu) interpolated_gpu->draw(scene, sprite_selected, 1u << layer);
            capture_png(o.dump_dir / (stem + "_off_" + names[layer] + ".png"), sprite_off, o.video);
            capture_png(o.dump_dir / (stem + "_" + mode_name(o.interpolation) + "_" + names[layer] + ".png"),
                interpolated_gpu ? std::span<const uint32_t>(sprite_selected) : std::span<const uint32_t>(sprite_off), o.video);
        }
        for (unsigned pf = 0; pf < 4; ++pf) {
            for (unsigned y = 0; y < 256; ++y) {
                const auto &r = scene.rows[y];
                const auto &p = r.playfields[pf];
                csv << m.frame << ',' << pf << ',' << y << ',' << int(y) - 24 << ',' << unsigned(stats.row_fields[pf][y])
                    << ',' << p.layer.enabled << ',' << r.bitmap << ',' << p.layer.mosaic << ',' << p.source_x
                    << ',' << p.source_y << ',' << p.x_step << ',' << p.y_step << ',' << unsigned(p.y_fraction)
                    << ',' << p.palette_add << ',' << unsigned(p.layer.priority) << ',' << unsigned(p.layer.blend_mode)
                    << ',' << unsigned(p.layer.clip_enabled) << ',' << unsigned(p.layer.clip_inverted) << '\n';
            }
        }
        if (!csv) throw std::runtime_error("Playfield capture CSV write failed");
    }
    void check_interpolation(const std::string &tag, bool injected) {
        if (!interpolated_gpu) return;
        const auto &scene = m.game_video->captured_frame();
        std::memcpy(static_cast<void *>(canonical_guard.get()), &scene, sizeof(scene));
        m.save_state(state_before);
        const auto crc = m.state_crc();
        const auto start = Clock::now();
        interpolated_gpu->draw(scene, interpolated);
        const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
        const auto stats = interpolated_gpu->last_interpolation();
        ++interpolation_reasons[size_t(stats.reason)];
        for (unsigned pf = 0; pf < 4; ++pf) {
            const auto &s = stats.layers[pf];
            ++layer_reasons[pf][size_t(s.reason)];
            const std::array<unsigned, 7> values{s.source_rows, s.zoom_rows, s.vertical_rows, s.palette_rows,
                s.invalid_rows, s.discontinuities, s.unsafe_palette_pairs};
            for (unsigned i = 0; i < values.size(); ++i) layer_totals[pf][i] += values[i];
        }
        if (o.bench && m.frame > 600 && !injected) interpolation_ms.push_back(elapsed);
        for (unsigned y = 0; y < o.video.height(); ++y) {
            const unsigned sy = y / o.video.scale + 24;
            bool flagged = false;
            for (unsigned pf = 0; pf < 4; ++pf) flagged |= stats.row_fields[pf][sy] != 0;
            if (y % o.video.scale == 0 || !flagged) {
                const size_t at = size_t(y) * o.video.width();
                require_exact(std::span(device).subspan(at, o.video.width()),
                    std::span(interpolated).subspan(at, o.video.width()), "Composite native-anchor/unflagged frame " + std::to_string(m.frame));
            }
        }
        for (unsigned pf = 0; pf < 4; ++pf) {
            gpu.draw(scene, sprite_off, 1u << pf);
            interpolated_gpu->draw(scene, sprite_selected, 1u << pf);
            for (unsigned y = 0; y < o.video.height(); ++y)
                if (y % o.video.scale == 0 || !stats.row_fields[pf][y / o.video.scale + 24]) {
                    const size_t at = size_t(y) * o.video.width();
                    require_exact(std::span(sprite_off).subspan(at, o.video.width()),
                        std::span(sprite_selected).subspan(at, o.video.width()),
                        std::string(names[pf]) + " native-anchor/unflagged frame " + std::to_string(m.frame));
                }
        }
        for (unsigned layer = 4; layer < 9; ++layer) {
            gpu.draw(scene, sprite_off, 1u << layer);
            interpolated_gpu->draw(scene, sprite_selected, 1u << layer);
            require_exact(sprite_off, sprite_selected, std::string(names[layer]) + " exact frame " + std::to_string(m.frame));
        }
        ++sprite_checks;
        if (!boundaries_checked && !scene.fallback && o.video.scale > 1) {
            f3rt::GpuVideo guards(nullptr, o.video, m.video->playfield_tiles(), m.video->sprite_tiles(),
                false, true, o.interpolation, f3rt::InterpolationFields::All);
            f3rt::GpuVideo geometry(nullptr, o.video, m.video->playfield_tiles(), m.video->sprite_tiles(),
                false, true, o.interpolation, f3rt::InterpolationFields::Geometry);
            boundaries_checked = interpolation_boundaries(gpu, guards, geometry, scene, o.video, m.frame,
                m.video->playfield_tiles());
        }
        m.save_state(state_after);
        if (state_before != state_after || crc != m.state_crc() ||
            std::memcmp(canonical_guard.get(), &scene, sizeof(scene)) != 0)
            throw std::runtime_error("Interpolation mutated canonical scene/native bytes at frame " + std::to_string(m.frame));
        if (!have_interpolation || previous_interpolation.reason != stats.reason ||
            previous_interpolation.layers != stats.layers || requested_capture()) {
            std::cout << "INTERP scene_frame=" << m.frame << " scenario=" << tag << " injected=" << injected
                << " mode=" << mode_name(o.interpolation) << " fields=" << f3rt::interpolation_fields_name(o.fields)
                << " reason=" << f3rt::interpolation_reason_name(stats.reason)
                << " native_unflagged_text_sprites=exact canonical_bytes=unchanged\n";
            for (unsigned pf = 0; pf < 4; ++pf) {
                const auto &s = stats.layers[pf];
                std::cout << "  pf=" << pf << " reason=" << f3rt::interpolation_reason_name(s.reason)
                    << " source_rows=" << s.source_rows << " zoom_rows=" << s.zoom_rows
                    << " vertical_rows=" << s.vertical_rows << " palette_rows=" << s.palette_rows
                    << " invalid_rows=" << s.invalid_rows << " discontinuities=" << s.discontinuities
                    << " unsafe_palette_pairs=" << s.unsafe_palette_pairs << '\n';
            }
        }
        previous_interpolation = stats; have_interpolation = true;
    }
    void sample(const std::string &tag = "gameplay", bool injected = false, bool force_capture = false) {
        ++sampled_frames;
        if (injected) ++injected_samples;
        if (m.game_video->captured_frame().fallback) ++fallback_samples;
        else ++supported_samples;
        if (o.layers && !m.game_video->captured_frame().fallback)
            for (unsigned i = 0; i < 9; ++i) compare(i, tag, injected);
        compare(9, tag, injected);
        check_interpolation(tag, injected);
        if (!injected && requested_capture()) capture_interpolation();
        if (force_capture || (captures < 3 && !m.game_video->captured_frame().fallback)) { capture(tag); ++captures; }
    }
    void report() const {
        std::cout << "PARITY sampled_frames=" << sampled_frames << " supported=" << supported_samples
            << " fallback=" << fallback_samples << " injected=" << injected_samples << '\n';
        for (unsigned i = 0; i < 10; ++i)
            std::cout << "  " << names[i] << " samples=" << samples[i] << " mismatching_pixels=" << mismatches[i] << '\n';
        std::cout << "SCALE summary changes=" << scale_change_frames << " canonical_scale=" << canonical_video.scale << '\n';
        if (interpolated_gpu) {
            std::cout << "INTERP summary mode=" << mode_name(o.interpolation) << " sprite_checks=" << sprite_checks
                << " induced_boundaries_checked=" << boundaries_checked << '\n';
            for (unsigned pf = 0; pf < 4; ++pf) {
                const auto &t = layer_totals[pf];
                std::cout << "  pf=" << pf << " total_source_rows=" << t[0] << " total_zoom_rows=" << t[1]
                    << " total_vertical_rows=" << t[2] << " total_palette_rows=" << t[3]
                    << " total_invalid_rows=" << t[4] << " total_discontinuities=" << t[5]
                    << " total_unsafe_palette_pairs=" << t[6] << '\n';
                for (size_t i = 0; i < layer_reasons[pf].size(); ++i)
                    if (layer_reasons[pf][i])
                        std::cout << "    reason=" << f3rt::interpolation_reason_name(f3rt::InterpolationReason(i))
                            << " scenes=" << layer_reasons[pf][i] << '\n';
            }
            for (size_t i = 0; i < interpolation_reasons.size(); ++i)
                if (interpolation_reasons[i])
                    std::cout << "  reason=" << f3rt::interpolation_reason_name(f3rt::InterpolationReason(i))
                        << " scenes=" << interpolation_reasons[i] << '\n';
        }
    }
};
void put16(std::span<uint8_t> bytes, size_t offset, uint16_t value) {
    bytes[offset] = uint8_t(value >> 8); bytes[offset + 1] = uint8_t(value);
}
// One sprite display-list descriptor decoded by runtime/renderer/decode.cpp. `x`/`y`
// are native scanout coordinates (0,0 = the first visible pixel); the encoder adds
// the 46/24 scanout origin. Scale is 1..256 with 256 = 1:1.
struct SpriteEntry {
    uint32_t tile = 0;
    int x = 0, y = 0;
    unsigned scale_x = 256, scale_y = 256;
    bool flip_x = false, flip_y = false;
    uint8_t color = 0; // Low 6 bits palette, high 2 bits priority group.
};
// 16-byte display-list entry in hardware byte order (big-endian words 0..6).
void put_sprite_entry(std::span<uint8_t> graphics, size_t offset, const SpriteEntry &s, uint16_t command) {
    put16(graphics, offset + 0, uint16_t(s.tile));
    put16(graphics, offset + 2, uint16_t(((256 - s.scale_y) & 0xff) << 8 | ((256 - s.scale_x) & 0xff)));
    // Scroll mode 8 keeps each descriptor absolute (the block accumulator ignores
    // the persistent global/subglobal scroll words).
    put16(graphics, offset + 4, uint16_t(0x8000 | ((s.x + 46) & 0x0fff)));
    put16(graphics, offset + 6, uint16_t(command ? (0x8000 | ((s.y + 24) & 0x0fff)) : ((s.y + 24) & 0x0fff)));
    put16(graphics, offset + 8, uint16_t((unsigned(s.flip_x ? 1 : 0) | unsigned(s.flip_y ? 2 : 0)) << 8 | s.color));
    // Word 5 carries the command bits (flipscreen/trails/bank) and the tile high bit.
    put16(graphics, offset + 10, uint16_t(command ? command : ((s.tile >> 16) & 1)));
    put16(graphics, offset + 12, 0);
}
// Replace both sprite-RAM banks with a fresh display list: a leading command entry
// that resets the retained sprite state, the descriptors, and a self-jump
// terminator. The game writes the same RAM, so this is the same data path.
void write_sprite_list(f3rt::Machine &m, std::span<const SpriteEntry> list, uint16_t command = 0) {
    std::fill(m.graphics.begin(), m.graphics.begin() + 0x10000, 0);
    for (size_t bank : {size_t(0), size_t(0x8000)}) {
        size_t entry = 0;
        put_sprite_entry(m.graphics, bank + entry * 16, SpriteEntry{}, command);
        ++entry;
        for (const auto &s : list) {
            put_sprite_entry(m.graphics, bank + entry * 16, s, 0);
            ++entry;
        }
        put16(m.graphics, bank + entry * 16 + 12, uint16_t(0x8000 | entry));
    }
}
void inject(Harness &h, unsigned kind) {
    auto &m = h.m;
    if (kind == 0) {
        // Line RAM bitmap pivot: latch section-2 sub-0 for the visible rows and
        // set the pivot control bits the decoder reads as bitmap mode.
        for (unsigned y = 24; y < 256; ++y) {
            put16(m.graphics, 0x20400 + y * 2, 1);
            put16(m.graphics, 0x26000 + y * 2, 0x2000);
        }
    } else {
        // Sprite command entry: word 3 bit 15 marks it, word 5 carries trails
        // (bit 1) or flipscreen (bit 13). Both banks are patched because the
        // active bank is retained hardware state.
        const uint16_t command = kind == 1 ? 0x0002 : 0x2000;
        for (size_t bank : {size_t(0), size_t(0x8000)}) {
            put16(m.graphics, bank + 6, 0x8000);
            put16(m.graphics, bank + 10, command);
            for (size_t slot = 1; slot < f3rt::max_hardware_sprites; ++slot) {
                const size_t descriptor = bank + slot * 16;
                if (m.graphics[descriptor + 6] & 0x80)
                    put16(m.graphics, descriptor + 10, command);
            }
        }
        m.game_video->render_frame(); // Latch command; current scanout remains old.
    }
    m.game_video->render_frame();
    if (!m.game_video->captured_frame().fallback) throw std::runtime_error("Injected producer did not reach oracle fallback");
}
// One native sprite descriptor: integral fields relative to the (46, 24)
// sprite origin with scroll zero and no global flip.
struct BoundarySprite {
    int x = 0, y = 0;
    unsigned sx = 256, sy = 256; // Producer scale = 256 - zoom byte.
    bool fx = false, fy = false, must_show = false;
    uint16_t palette = 0xc0;
};
SpriteEntry to_entry(const BoundarySprite &d, uint32_t tile) {
    return {.tile = tile, .x = d.x, .y = d.y, .scale_x = d.sx, .scale_y = d.sy,
        .flip_x = d.fx, .flip_y = d.fy, .color = uint8_t(d.palette)};
}
// CPU reference spans (GameSprites::raster) in selected output coordinates.
// Used only to place and classify witnesses; pixels are judged by the
// independent CPU reference and the exact CPU/GPU comparisons.
struct BoundarySpans {
    std::array<int, 16> x0{}, x1{}, y0{}, y1{};
    bool culled = false;
    std::array<bool, 4> clipped{}; // left, right, top, bottom
};
BoundarySpans boundary_spans(const BoundarySprite &d, f3rt::GameVideoOptions v) {
    const int s = int(v.scale), b = int(v.border), sx = int(d.sx), sy = int(d.sy);
    const int x = (d.x + 46) * 256, y = (d.y + 24) * 256;
    BoundarySpans r;
    r.culled = x + sx * 16 <= (46 - b) * 256 || x > (365 + b) * 256 || y + sy * 16 <= 24 * 256 || y > 255 * 256;
    for (int t = 0; t < 16; ++t) {
        const int px = (x + t * sx) * s + 128, py = (y + t * sy) * s + 255;
        r.x0[t] = (px >> 8) - (46 - b) * s;
        r.x1[t] = ((px + sx * s) >> 8) - (46 - b) * s;
        r.y0[t] = (py >> 8) - 24 * s;
        r.y1[t] = std::max(r.y0[t] + 1, ((py + sy * s) >> 8) - 24 * s);
    }
    r.clipped = {r.x0[0] < 0, r.x1[15] > int(v.width()), r.y0[0] < 0, r.y1[15] > int(v.height())};
    return r;
}
struct BoundarySample { unsigned pen = 0, rows = 0; bool transparent_first = false, reordered = false; };
// Pen mask 15 matches sprite command 0. Earliest opaque logical row wins.
BoundarySample boundary_sample(const BoundarySpans &r, std::span<const uint8_t> pens, bool fx, bool fy, int px, int py) {
    BoundarySample out;
    int column = -1;
    for (int t = 0; t < 16; ++t) if (r.x0[t] <= px && px < r.x1[t]) column = t;
    if (r.culled || column < 0) return out;
    unsigned first = 0;
    for (int t = 0; t < 16; ++t) {
        if (py < r.y0[t] || py >= r.y1[t]) continue;
        const unsigned pen = pens[(t ^ (fy ? 15 : 0)) * 16 + (column ^ (fx ? 15 : 0))] & 15u;
        if (!out.rows++) first = pen;
        if (!out.pen) out.pen = pen;
        else if (pen && pen != out.pen) out.reordered = true;
    }
    out.transparent_first = out.rows > 1 && !first && out.pen;
    return out;
}
struct BoundaryStats {
    size_t visible = 0, hidden = 0, collapsed = 0, overlapped = 0, transparent_first = 0, reordered = 0,
        flipped = 0, shown_through = 0;
    std::array<size_t, 4> edges{};
};
// Classifies every CPU-reference sprite pixel of one injected batch. Line
// clip/enable state may hide predicted texels, but the reference must never
// show an unpredicted sprite pixel, and within each output row every
// (palette, pen) must map to one distinct ARGB value. That ties each witness
// (first-opaque row order, flip, show-through) to the reference's choice.
BoundaryStats boundary_witnesses(const std::vector<BoundarySprite> &list, std::span<const uint8_t> pens,
        f3rt::GameVideoOptions v, std::span<const uint32_t> reference, std::span<const uint32_t> blank,
        const std::string &tag) {
    enum : uint8_t { overlapped = 1, transparent_first = 2, reordered = 4, flipped = 8, shown_through = 16 };
    const int width = int(v.width()), height = int(v.height());
    std::vector<uint16_t> key(size_t(width) * height), owner(key.size());
    std::vector<uint8_t> flags(key.size());
    std::vector<BoundarySpans> spans;
    BoundaryStats stats;
    for (size_t i = 0; i < list.size(); ++i) {
        const auto &d = list[i];
        const auto &r = spans.emplace_back(boundary_spans(d, v));
        if (r.culled) throw std::runtime_error(tag + " descriptor " + std::to_string(i) + " is nominally culled");
        for (int t = 0; t < 16; ++t) {
            if (r.x0[t] != r.x1[t] || r.x0[t] <= 0 || r.x0[t] >= width) continue;
            bool opaque = false;
            for (int row = 0; row < 16; ++row) opaque |= (pens[row * 16 + (t ^ (d.fx ? 15 : 0))] & 15u) != 0;
            stats.collapsed += opaque;
        }
        for (int py = std::max(0, r.y0[0]); py < std::min(height, r.y1[15]); ++py)
            for (int px = std::max(0, r.x0[0]); px < std::min(width, r.x1[15]); ++px) {
                const auto sample = boundary_sample(r, pens, d.fx, d.fy, px, py);
                const size_t at = size_t(py) * width + px;
                if (!sample.rows) continue;
                if (sample.rows > 1 && d.sy * v.scale >= 256)
                    throw std::runtime_error(tag + " witness model overlapped rows at or above one output pixel");
                if (!sample.pen) {
                    if (key[at]) flags[at] |= shown_through;
                    continue;
                }
                uint8_t f = uint8_t((sample.rows > 1 ? overlapped : 0) | (sample.transparent_first ? transparent_first : 0) |
                    (sample.reordered ? reordered : 0));
                if ((d.fx || d.fy) && boundary_sample(r, pens, false, false, px, py).pen != sample.pen) f |= flipped;
                key[at] = uint16_t((d.palette & 0xff) << 4 | sample.pen);
                flags[at] = f; owner[at] = uint16_t(i);
            }
    }
    std::vector<size_t> visible(list.size());
    std::vector<std::pair<uint16_t, uint32_t>> colors;
    for (int py = 0; py < height; ++py) {
        colors.clear();
        for (int px = 0; px < width; ++px) {
            const size_t at = size_t(py) * width + px;
            const auto where = [&] { return " x=" + std::to_string(px) + " y=" + std::to_string(py); };
            if (!key[at]) {
                if (reference[at] != blank[at]) throw std::runtime_error(tag + " CPU reference shows an unpredicted sprite pixel" + where());
                continue;
            }
            if (reference[at] == blank[at]) { ++stats.hidden; continue; }
            const auto known = std::find_if(colors.begin(), colors.end(), [&](const auto &c) { return c.first == key[at]; });
            if (known == colors.end()) {
                if (std::any_of(colors.begin(), colors.end(), [&](const auto &c) { return c.second == reference[at]; }))
                    throw std::runtime_error(tag + " CPU reference merged distinct sprite pens" + where());
                colors.emplace_back(key[at], reference[at]);
            } else if (known->second != reference[at]) {
                throw std::runtime_error(tag + " CPU reference resolved a different texel row/flip/owner" + where());
            }
            ++visible[owner[at]]; ++stats.visible;
            const auto f = flags[at];
            stats.overlapped += (f & overlapped) != 0;
            stats.transparent_first += (f & transparent_first) != 0;
            stats.reordered += (f & reordered) != 0;
            stats.flipped += (f & flipped) != 0;
            stats.shown_through += (f & shown_through) != 0;
            const auto &clipped = spans[owner[at]].clipped;
            stats.edges[0] += px == 0 && clipped[0];
            stats.edges[1] += px == width - 1 && clipped[1];
            stats.edges[2] += py == 0 && clipped[2];
            stats.edges[3] += py == height - 1 && clipped[3];
        }
    }
    for (size_t i = 0; i < list.size(); ++i)
        if (list[i].must_show && !visible[i])
            throw std::runtime_error(tag + " descriptor " + std::to_string(i) + " has no visible reference pixel");
    return stats;
}
// Native single-sprite producer branches with actual decoded ROM texels.
// The caller replays the original pre-scanout frame to restore host/native state.
void verify_sprite_boundaries(Harness &h) {
    auto &m = h.m;
    const auto baseline = snapshot(m);
    const auto tiles = m.video->sprite_tiles();
    // Fresh supported sprite list: distinguishable palettes 0xc0/0xc1 (sp3),
    // scroll zero, and sprite command 0 (pen mask 15, no trails/global flip).
    auto begin_list = [&] {
        m.load_state(baseline);
        // Make opaque ROM pens and descriptor ownership distinguishable even
        // when gameplay has not populated these two palette banks.
        for (unsigned bank = 0; bank < 2; ++bank)
            for (unsigned pen = 1; pen < 16; ++pen) {
                const unsigned at = (0x1c00 + bank * 16 + pen) * 4;
                m.palette[at + 1] = uint8_t(bank ? 32 : 224);
                m.palette[at + 2] = uint8_t(pen * 15);
                m.palette[at + 3] = uint8_t(bank ? 224 : 32);
            }
    };
    unsigned tile = 0;
    for (unsigned candidate = 1; candidate < std::min<size_t>(32768, tiles.size() / 256); ++candidate) {
        const auto pens = tiles.subspan(candidate * 256, 256);
        bool collision = false, bottom = false;
        for (unsigned x = 0; x < 16; ++x) {
            unsigned first = 0;
            for (unsigned y = 0; y < 16; ++y) {
                const unsigned pen = pens[y * 16 + x] & 15;
                if (pen && !first) first = pen;
                else if (pen && pen != first) collision = true;
            }
            bottom |= (pens[15 * 16 + x] & 15) != 0;
        }
        if (collision && bottom) { tile = candidate; break; }
    }
    if (!tile) throw std::runtime_error("ROM has no sprite tile witnessing crushed-row ordering and top-edge leakage");
    constexpr std::array<const char *, 3> tags{
        "sprite_crushed_first_opaque_overlap", "sprite_mirrored_sampled_zoom", "sprite_nominal_top_cull"};
    std::vector<uint32_t> blank(h.device.size());
    for (unsigned kind = 0; kind < tags.size(); ++kind) {
        begin_list();
        std::vector<SpriteEntry> entries;
        if (kind == 0) {
            // The later descriptor owns the overlap; its crushed earliest opaque
            // ROM row must win when 16 texel rows land on one output row.
            entries.push_back(to_entry({.x = 40, .y = 40, .palette = 0xc0}, tile));
            entries.push_back(to_entry({.x = 42, .y = 41, .sy = 2, .palette = 0xc1}, tile));
        } else if (kind == 1) {
            entries.push_back(to_entry({.x = 120, .y = 60, .sx = 83, .sy = 112,
                .fx = true, .fy = true, .palette = 0xc0}, tile));
        } else {
            entries.push_back(to_entry({.x = 60, .y = -40, .palette = 0xc0}, tile));
        }
        write_sprite_list(m, entries);
        m.game_video->render_frame(); // Scanout precedes the native sprite latch.
        m.game_video->render_frame(); // Present the injected, now-latched list.
        if (m.game_video->captured_frame().fallback)
            throw std::runtime_error(std::string(tags[kind]) + " unexpectedly left supported native producers");
        // Compare the isolated consumer even without --layers, then composite
        // and the existing exact whole-sprite line-mode comparisons.
        h.compare(7, tags[kind], true);
        m.game_video->render_reference(blank, h.o.video, 0, true);
        size_t visible = 0;
        for (size_t i = 0; i < blank.size(); ++i) visible += h.device[i] != blank[i];
        if ((kind == 2 && visible) || (kind != 2 && !visible))
            throw std::runtime_error(std::string(tags[kind]) + " did not witness its visible/cull boundary");
        h.capture(std::string(tags[kind]) + "_sp3");
        h.sample(tags[kind], true, true);
        std::cout << "SPRITE_BOUNDARY scenario=" << tags[kind] << " tile=" << tile
                  << " scale=" << h.o.video.scale << " visible_pixels=" << visible
                  << " isolated_and_composite=exact (native producer branch)\n";
    }

    // Batched consumer boundaries: spatially separated descriptors per group,
    // one replay each. Tile: a column with two leading and one with two
    // trailing transparent rows above/below distinct opaque pens (crushed
    // first-opaque order under both Y flips), opaque texels on every border
    // (one-line edge witnesses), dense, and asymmetric under both flips.
    unsigned batch_tile = 0;
    for (unsigned candidate = 1; candidate < std::min<size_t>(32768, tiles.size() / 256) && !batch_tile; ++candidate) {
        const auto pens = tiles.subspan(candidate * 256, 256);
        auto pen = [&](unsigned row, unsigned col) { return pens[row * 16 + col] & 15u; };
        auto distinct = [&](unsigned col, unsigned from, unsigned to) {
            unsigned first = 0;
            for (unsigned row = from; row < to; ++row) {
                const unsigned p = pen(row, col);
                if (p && !first) first = p;
                else if (p && p != first) return true;
            }
            return false;
        };
        unsigned opaque = 0;
        bool leading = false, trailing = false, mirror_x = true, mirror_y = true;
        std::array<bool, 4> border{};
        for (unsigned a = 0; a < 16; ++a) {
            border[0] |= pen(a, 0) != 0; border[1] |= pen(a, 15) != 0;
            border[2] |= pen(0, a) != 0; border[3] |= pen(15, a) != 0;
            for (unsigned b = 0; b < 16; ++b) {
                opaque += pen(a, b) != 0;
                mirror_x &= pen(a, b) == pen(a, 15 - b);
                mirror_y &= pen(a, b) == pen(15 - a, b);
            }
            leading |= !pen(0, a) && !pen(1, a) && distinct(a, 2, 16);
            trailing |= !pen(15, a) && !pen(14, a) && distinct(a, 0, 14);
        }
        if (opaque >= 96 && leading && trailing && !mirror_x && !mirror_y &&
            std::all_of(border.begin(), border.end(), [](bool b) { return b; }))
            batch_tile = candidate;
    }
    if (!batch_tile) throw std::runtime_error("ROM has no sprite tile witnessing batched sprite boundaries");
    const auto batch_pens = tiles.subspan(batch_tile * 256, 256);
    auto emit = [&](std::vector<SpriteEntry> &entries, const BoundarySprite &d) {
        entries.push_back(to_entry(d, batch_tile));
    };
    const unsigned s = h.o.video.scale;
    const int L = -int(h.o.video.border), R = 320 + int(h.o.video.border);
    // Horizontal steps below one output pixel: some texel columns collapse to zero width.
    const unsigned wide_collapse = std::max(1u, 200 / s), narrow_collapse = std::max(1u, 72 / s);
    // Y steps: crushed, below, at (when 256 % scale == 0), and above one output pixel.
    std::vector<unsigned> steps{1, 7, std::max(1u, 160 / s), std::max(1u, 255 / s), 256};
    if (256 % s == 0) steps.push_back(256 / s);
    if (256 / s < 256) steps.push_back(256 / s + 1);
    std::sort(steps.begin(), steps.end());
    steps.erase(std::unique(steps.begin(), steps.end()), steps.end());

    std::vector<BoundarySprite> thresholds;
    for (unsigned step : steps)
        for (unsigned sx : {256u, wide_collapse, narrow_collapse})
            for (unsigned flip = 0; flip < 4; ++flip) {
                const int cell = int(thresholds.size());
                thresholds.push_back({.x = 2 + cell % 16 * 20, .y = 2 + cell / 16 * 20, .sx = sx, .sy = step,
                    .fx = (flip & 1) != 0, .fy = (flip & 2) != 0,
                    .must_show = sx == 256, .palette = uint16_t(0xc0 | (flip >> 1))});
            }
    // Later descriptors own overlap; their transparent (and crushed-away) texels
    // must leave the earlier descriptor visible. Rows 0..2 of each earlier one stay exposed.
    std::vector<BoundarySprite> overlaps;
    for (unsigned step : {256u, std::max(1u, 160 / s), 1u})
        for (unsigned sx : {256u, wide_collapse})
            for (unsigned flip = 0; flip < 4; ++flip) {
                const int cell = int(overlaps.size() / 2), x = 2 + cell % 13 * 24, y = 2 + cell / 13 * 24;
                overlaps.push_back({.x = x, .y = y, .must_show = true});
                overlaps.push_back({.x = x + 5, .y = y + 3, .sx = sx, .sy = step, .fx = (flip & 1) != 0,
                    .fy = (flip & 2) != 0, .must_show = sx == 256, .palette = 0xc1});
            }
    // Every descriptor is clipped by at least one target edge (border-aware).
    std::vector<BoundarySprite> edges;
    auto edge = [&](int x, int y, unsigned sx, unsigned sy, bool must_show) {
        const unsigned flip = unsigned(edges.size()) & 3;
        edges.push_back({.x = x, .y = y, .sx = sx, .sy = sy, .fx = (flip & 1) != 0, .fy = (flip & 2) != 0,
            .must_show = must_show, .palette = uint16_t(0xc0 | (flip & 1))});
    };
    const unsigned below = std::max(1u, 160 / s);
    edge(L - 8, 30, 256, 256, true); edge(L - 15, 50, 256, 256, true); edge(L - 1, 70, wide_collapse, 256, false);
    edge(L - 8, 90, 256, 1, true); edge(L - 15, 110, 256, below, true);
    edge(R - 8, 30, 256, 256, true); edge(R - 1, 50, 256, 256, true); edge(R - 1, 70, wide_collapse, 256, false);
    edge(R - 8, 90, 256, 1, true); edge(R - 1, 110, 256, below, true);
    edge(30, -8, 256, 256, true); edge(50, -15, 256, 256, true); edge(70, -1, 256, 17, true);
    edge(90, -2, wide_collapse, 33, false); edge(110, -4, 256, 80, false);
    edge(30, 224, 256, 256, true); edge(50, 231, 256, 256, true); edge(70, 231, 256, 17, true);
    edge(90, 228, narrow_collapse, 256, false); edge(110, 230, 256, 80, true);
    edge(L - 8, -8, 256, 256, false); edge(R - 8, -8, 256, 256, false);
    edge(L - 8, 224, 256, 256, false); edge(R - 8, 224, 256, 256, false);
    for (const auto &d : edges) {
        const auto clipped = boundary_spans(d, h.o.video).clipped;
        if (std::none_of(clipped.begin(), clipped.end(), [](bool c) { return c; }))
            throw std::runtime_error("sprite_batch_edge_clip descriptor is not clipped by the target");
    }

    const std::array<std::pair<const char *, const std::vector<BoundarySprite> *>, 3> batches{{
        {"sprite_batch_y_threshold_flip_collapse", &thresholds},
        {"sprite_batch_transparent_overlap", &overlaps},
        {"sprite_batch_edge_clip", &edges}}};
    for (const auto &[tag, list] : batches) {
        begin_list();
        std::vector<SpriteEntry> entries;
        for (const auto &d : *list) emit(entries, d);
        write_sprite_list(m, entries);
        m.game_video->render_frame(); // Scanout precedes the native sprite latch.
        m.game_video->render_frame(); // Present the injected, now-latched list.
        if (m.game_video->captured_frame().fallback)
            throw std::runtime_error(std::string(tag) + " unexpectedly left supported native producers");
        h.compare(7, tag, true); // Exact isolated sp3 CPU/GPU comparison.
        blank.resize(h.device.size());
        m.game_video->render_reference(blank, h.o.video, 0, true);
        const auto st = boundary_witnesses(*list, batch_pens, h.o.video, h.cpu, blank, tag);
        const bool thresholds_batch = list == &thresholds, overlap_batch = list == &overlaps;
        const bool witnessed = st.visible && st.flipped &&
            (thresholds_batch ? st.collapsed && st.overlapped && st.transparent_first && st.reordered
             : overlap_batch ? st.shown_through && st.transparent_first
             : std::all_of(st.edges.begin(), st.edges.end(), [](size_t n) { return n != 0; }));
        if (!witnessed) throw std::runtime_error(std::string(tag) + " did not witness its consumer-visible boundaries");
        h.capture(std::string(tag) + "_sp3");
        h.sample(tag, true, true); // Composite (and all layers with --layers).
        std::cout << "SPRITE_BOUNDARY scenario=" << tag << " tile=" << batch_tile << " scale=" << s
                  << " border=" << h.o.video.border << " descriptors=" << list->size()
                  << " visible_pixels=" << st.visible << " line_hidden=" << st.hidden
                  << " collapsed_columns=" << st.collapsed << " overlapped=" << st.overlapped
                  << " transparent_first=" << st.transparent_first << " reordered=" << st.reordered
                  << " flipped=" << st.flipped << " shown_through=" << st.shown_through
                  << " edges=" << st.edges[0] << '/' << st.edges[1] << '/' << st.edges[2] << '/' << st.edges[3]
                  << " isolated_and_composite=exact (native producer batch)\n";
    }
}
// Saving after several trail frames must retain every intervening sprite list.
void verify_trail_history(Harness &h) {
    auto &m = h.m;
    const auto baseline = snapshot(m);
    const auto baseline_crc = m.state_crc();
    auto peer_owner = std::make_unique<f3rt::Machine>(m.roms);
    auto &peer = *peer_owner;
#ifdef F3RT_SOUND_GENERATED
    peer.use_native_sound(f3_sound_blocks, f3_sound_block_count,
                          {f3_sound_excluded_ranges, f3_sound_excluded_count}, f3_sound_rom_crc32);
#endif
    peer.game_video = std::make_unique<f3rt::GameVideo>(peer, f3rt::GameVideoMode::Game, h.canonical_video);
    peer.load_state(baseline);
    const unsigned original_scale = h.o.video.scale;
    constexpr std::array<unsigned, 4> trail_scales{3, 2, 4, 1};
    for (unsigned frame = 0; frame < 4; ++frame) {
        if (!h.o.scale_changes.empty()) h.set_scale(trail_scales[frame]);
        for (auto *machine : {&m, &peer}) {
            // Trails command entry (word 3 bit 15, word 5 bit 1) plus a moving
            // sprite, so each trailed list differs and history must be retained.
            const SpriteEntry s{.tile = 1, .x = 40, .y = int(40 + frame * 8), .color = uint8_t(0xc0 | frame)};
            write_sprite_list(*machine, std::span<const SpriteEntry>(&s, 1), 0x0002);
            machine->game_video->render_frame();
        }
        h.sample("trails_history_scale", true);
    }
    const auto gpu_state = snapshot(m), cpu_state = snapshot(peer);
    size_t differences = 0;
    for (size_t i = 0; i < gpu_state.size(); ++i) differences += gpu_state[i] != cpu_state[i];
    m.load_state(baseline);
    if (!h.o.scale_changes.empty()) h.set_scale(original_scale);
    if (m.state_crc() != baseline_crc) throw std::runtime_error("Trail branch restore changed canonical state");
    if (differences) throw std::runtime_error("Deferred GPU trail snapshots differ from CPU: " + std::to_string(differences) + " bytes");
    std::cout << "SNAPSHOT trails_history_frames=4 deferred_save=1 byte_mismatches=0 (induced branch)\n";
}
// Real supported producer captures, with no native/save observation between
// scanouts. Separate peers keep gameplay, harness samples and diagnostics intact.
void verify_deferred_native(Harness &h) {
    const auto baseline = snapshot(h.m);
    const auto baseline_blocks = h.m.native_blocks, baseline_fallbacks = h.m.fallback_instructions;
    auto eager_owner = std::make_unique<f3rt::Machine>(h.m.roms);
    auto lazy_owner = std::make_unique<f3rt::Machine>(h.m.roms);
    auto &eager = *eager_owner, &lazy = *lazy_owner;
    for (auto *m : {&eager, &lazy}) {
#ifdef F3RT_SOUND_GENERATED
        m->use_native_sound(f3_sound_blocks, f3_sound_block_count,
                            {f3_sound_excluded_ranges, f3_sound_excluded_count}, f3_sound_rom_crc32);
#endif
        m->game_video = std::make_unique<f3rt::GameVideo>(*m, f3rt::GameVideoMode::Game, h.canonical_video);
    }
    // The independent control stays CPU-eager for the entire proof.
    lazy.game_video->enable_gpu_presentation();
    const auto tiles = h.m.video->sprite_tiles();
    unsigned tile = 0;
    for (unsigned candidate = 1; candidate < std::min<size_t>(32768, tiles.size() / 256); ++candidate) {
        const auto pens = tiles.subspan(candidate * 256, 256);
        unsigned opaque = 0;
        bool asymmetric = false;
        for (unsigned y = 0; y < 16; ++y)
            for (unsigned x = 0; x < 16; ++x) {
                opaque += (pens[y * 16 + x] & 15) != 0;
                asymmetric |= (pens[y * 16 + x] & 15) != (pens[(15 - y) * 16 + x] & 15);
            }
        if (opaque >= 96 && asymmetric) { tile = candidate; break; }
    }
    if (!tile) throw std::runtime_error("Deferred native proof needs a visible asymmetric ROM sprite");
    auto descriptor = [](unsigned frame) {
        return BoundarySprite{.x = int(40 + frame * 56), .y = 56,
            .fx = (frame & 1) != 0, .fy = (frame & 2) != 0,
            .must_show = true, .palette = uint16_t(0xc0 | (frame & 1))};
    };
    auto capture = [&](f3rt::Machine &m, unsigned frame) {
        // Distinct, nonsaturating colors make both stale palettes and descriptor
        // ownership visible, even when gameplay has not filled these banks.
        for (unsigned color = 0; color < 8192; ++color) {
            m.palette[color * 4 + 1] = uint8_t(20 + frame * 12);
            m.palette[color * 4 + 2] = uint8_t(30 + frame * 16);
            m.palette[color * 4 + 3] = uint8_t(40 + frame * 11);
        }
        for (unsigned bank = 0; bank < 2; ++bank)
            for (unsigned pen = 1; pen < 16; ++pen) {
                const unsigned at = (0x1c00 + bank * 16 + pen) * 4;
                m.palette[at + 1] = uint8_t(bank ? 32 : 200);
                m.palette[at + 2] = uint8_t(64 + frame * 16);
                m.palette[at + 3] = uint8_t(pen * 11);
            }
        const auto d = descriptor(frame);
        const SpriteEntry s = to_entry(d, tile);
        write_sprite_list(m, std::span<const SpriteEntry>(&s, 1));
        m.game_video->render_frame(); // Capture frame N, then latch list N for N+1.
    };
    auto mutate_live = [](f3rt::Machine &m) {
        for (auto &byte : m.palette) byte ^= 0xff;
        for (size_t i = 0; i < m.graphics.size(); ++i) m.graphics[i] ^= uint8_t(0x5b + i * 17);
    };
    // All save destinations are allocated once; each observer is exercised first
    // on a fresh pending capture, rather than after an earlier save flushed it.
    std::vector<uint8_t> eager_state(baseline.size()), lazy_state(baseline.size());
    auto exact_bytes = [](const auto &a, const auto &b, const char *tag, const char *kind) {
        const auto mismatch = std::mismatch(a.begin(), a.end(), b.begin(), b.end());
        if (mismatch.first != a.end())
            throw std::runtime_error(std::string("Deferred native ") + tag + " differs in " + kind +
                                     " at byte " + std::to_string(mismatch.first - a.begin()));
    };
    auto compare = [&](const char *tag) {
        require_exact(eager.native_pixels(), lazy.native_pixels(), std::string("Deferred native ") + tag);
        eager.save_state(eager_state); lazy.save_state(lazy_state);
        exact_bytes(eager_state, lazy_state, tag, "state");
    };
    auto native_video = h.canonical_video;
    native_video.scale = 1;
    std::vector<uint32_t> sprite(size_t(native_video.width()) * native_video.height()), blank(sprite.size());
    const std::vector<BoundarySprite> visible_list{descriptor(2)};
    constexpr std::array tags{"pixels_first", "save_first", "fallback_successor", "scale_pending",
        "disable_enable_pending", "enable_pending", "load_pending", "reset_pending"};
    size_t visible_pixels = 0, composite_pixels = 0;
    for (unsigned kind = 0; kind < tags.size(); ++kind) {
        lazy.game_video->enable_gpu_presentation();
        lazy.game_video->set_gpu_scale(h.o.video.scale);
        for (auto *m : {&eager, &lazy})
            m->load_state(baseline);
        for (unsigned frame = 0; frame < 4; ++frame) {
            capture(eager, frame); capture(lazy, frame);
            if (lazy.game_video->captured_frame().fallback)
                throw std::runtime_error(std::string("Deferred native ") + tags[kind] + " left supported producers");
        }
        // No pixels, CRCs, saves or references have observed these four frames.
        // Frame 3 must use palette 3 and list 2, NOT live memory or next list 3.
        mutate_live(eager); mutate_live(lazy);
        if (kind == 1) {
            eager.save_state(eager_state); lazy.save_state(lazy_state);
            exact_bytes(eager_state, lazy_state, tags[kind], "first save");
        } else if (kind == 2) {
            // A trails command entry is the remaining genuine oracle fallback:
            // the deferred capture must survive it without consulting live RAM.
            for (auto *m : {&eager, &lazy}) {
                write_sprite_list(*m, {}, 0x0002);
                m->game_video->render_frame(); // Latch the trails command.
                m->game_video->render_frame(); // Render the oracle fallback.
            }
            if (!lazy.game_video->captured_frame().fallback)
                throw std::runtime_error("Deferred native fallback successor did not reach the oracle");
        } else if (kind == 3) {
            lazy.game_video->set_gpu_scale(h.o.video.scale == 1 ? 3 : 1);
        } else if (kind == 4) {
            lazy.game_video->enable_gpu_presentation(false);
            compare("disable_pending");
            lazy.game_video->enable_gpu_presentation();
        } else if (kind == 5) {
            lazy.game_video->enable_gpu_presentation();
        } else if (kind == 6) {
            for (auto *m : {&eager, &lazy}) m->load_state(baseline);
        } else if (kind == 7) {
            eager.reset(); lazy.reset();
        }
        compare(tags[kind]);
        if (kind == 0) {
            // Independent ROM/position witnesses prove that the deferred frame
            // visibly contains list 2, not the now-latched list 3 or a blank plane.
            lazy.game_video->set_gpu_scale(1);
            lazy.game_video->render_reference(sprite, native_video, 1u << 7, true);
            lazy.game_video->render_reference(blank, native_video, 0, true);
            visible_pixels = boundary_witnesses(visible_list, tiles.subspan(tile * 256, 256),
                native_video, sprite, blank, "deferred_native_sprite_lag").visible;
            lazy.game_video->render_reference(sprite, native_video, f3rt::all_layers, true);
            lazy.game_video->render_reference(blank, native_video, f3rt::all_layers ^ f3rt::layer_bit(f3rt::LayerId::Sp3), true);
            const auto &pixels = eager.native_pixels();
            for (unsigned y = 0; y < 232; ++y)
                for (unsigned x = 0; x < 320; ++x) {
                    const size_t at = size_t(y) * native_video.width() + native_video.border + x;
                    if (pixels[y * 320 + x] != sprite[at])
                        throw std::runtime_error("Deferred native composite witness differs from the eager consumer");
                    composite_pixels += sprite[at] != blank[at];
                }
            if (!visible_pixels || !composite_pixels)
                throw std::runtime_error("Deferred native fixture has no visible isolated/composite sprite witness");
        }
        if (kind == 6) {
            // A subsequent real capture must not resurrect the discarded branch
            // after a state load.
            capture(eager, 4); capture(lazy, 4);
            if (lazy.game_video->captured_frame().fallback)
                throw std::runtime_error("Deferred native snapshot recovery left supported producers");
            mutate_live(eager); mutate_live(lazy);
            compare("load_supported_successor");
        }
    }
    h.m.save_state(lazy_state);
    exact_bytes(baseline, lazy_state, "branch restore", "gameplay state");
    if (h.m.native_blocks != baseline_blocks || h.m.fallback_instructions != baseline_fallbacks)
        throw std::runtime_error("Deferred native branch changed gameplay diagnostic counters");
    std::cout << "SNAPSHOT deferred_native_supported_frames=4 cases=" << tags.size()
              << " live_palette_graphics_mutation=1 sprite_lag_visible_pixels=" << visible_pixels
              << " composite_sprite_pixels=" << composite_pixels
              << " native_state_mismatches=0 gameplay_and_diagnostics=unchanged (independent branches)\n";
}
void verify_cpu_backend(Harness &h, std::span<const uint8_t> pre) {
    auto peer_owner = std::make_unique<f3rt::Machine>(h.m.roms);
    auto &peer = *peer_owner;
    peer.allow_main_fallback = false;
#ifdef F3RT_GENERATED
    if (!f3_generated_register(&peer.cpu)) throw std::runtime_error("CPU peer registration failed");
#endif
#ifdef F3RT_SOUND_GENERATED
    peer.use_native_sound(f3_sound_blocks, f3_sound_block_count,
                          {f3_sound_excluded_ranges, f3_sound_excluded_count}, f3_sound_rom_crc32);
#endif
    peer.game_video = std::make_unique<f3rt::GameVideo>(peer, f3rt::GameVideoMode::Game, h.canonical_video);
    const bool selected_geometry = h.canonical_video.scale != h.o.video.scale;
    if (selected_geometry) {
        peer.game_video->enable_gpu_presentation();
        peer.game_video->set_gpu_scale(h.o.video.scale);
    }
    peer.load_state(pre);
    advance(peer);
    const auto gpu_state = snapshot(h.m), cpu_state = snapshot(peer);
    size_t bytes = 0, pixels = 0;
    for (size_t i = 0; i < gpu_state.size(); ++i) bytes += gpu_state[i] != cpu_state[i];
    std::vector<uint32_t> selected_cpu;
    if (selected_geometry) {
        selected_cpu.resize(h.device.size());
        peer.game_video->render_reference(selected_cpu, h.o.video);
    }
    const auto original_cpu = selected_geometry ? std::span<const uint32_t>(selected_cpu) : peer.game_video->presentation();
    for (size_t i = 0; i < h.device.size(); ++i) pixels += h.device[i] != original_cpu[i];
    if (bytes || pixels)
        throw std::runtime_error("Independent CPU backend differs: " + std::to_string(bytes) +
                                 " canonical bytes / " + std::to_string(pixels) + " GPU pixels");
    std::cout << "SNAPSHOT supported_cpu_backend frame=" << peer.frame
              << " byte_mismatches=0 gpu_argb_mismatches=0"
              << " selected_geometry_reference=" << selected_geometry << '\n';
}

void timing(const char *name, std::vector<double> values) {
    if (values.empty()) {
        std::cout << "BENCH " << name << " repeats=0 (no supported post-warmup samples)\n";
        return;
    }
    const double mean = std::accumulate(values.begin(), values.end(), 0.0) / values.size();
    std::sort(values.begin(), values.end());
    std::cout << "BENCH " << name << " repeats=" << values.size() << " mean_ms=" << mean
        << " p95_ms=" << values[size_t(std::ceil(values.size() * 0.95)) - 1] << " worst_ms=" << values.back() << '\n';
}
void benchmark(Harness &h) {
    constexpr unsigned repeats = 100;
    const auto crc = h.m.state_crc();
    std::array<std::vector<double>, 4> times;
    for (unsigned mode = 0; mode < (h.interpolated_gpu ? 4u : 3u); ++mode) times[mode].reserve(repeats);
    for (unsigned i = 0; i < repeats + 5; ++i) {
        for (unsigned mode = 0; mode < (h.interpolated_gpu ? 4u : 3u); ++mode) {
            const auto start = Clock::now();
            if (mode < 2) h.m.game_video->render_reference(h.cpu, h.o.video, f3rt::all_layers, mode == 0);
            else if (mode == 2) h.gpu.draw(h.m.game_video->captured_frame(), h.device);
            else h.interpolated_gpu->draw(h.m.game_video->captured_frame(), h.interpolated);
            const auto elapsed = std::chrono::duration<double, std::milli>(Clock::now() - start).count();
            if (i >= 5) times[mode].push_back(elapsed);
        }
    }
    if (h.m.state_crc() != crc) throw std::runtime_error("Benchmark mutated native state");
    h.compare(9, "benchmark", false);
    if (h.interpolated_gpu) h.check_interpolation("benchmark", false);
    std::cout << "BENCH snapshot_frame=" << h.m.frame << " supported=1 warmup=5\n";
    timing("CPU_serial_reference", times[0]); timing("CPU_threaded_reference", times[1]);
    timing("GPU_submit_fence_readback_including_driver_and_transfer", times[2]);
    if (h.interpolated_gpu) timing("GPU_interpolation_submit_fence_readback_including_driver_and_transfer", times[3]);
}
struct SdlLifetime {
    SdlLifetime() { if (!SDL_Init(SDL_INIT_VIDEO)) throw std::runtime_error(std::string("SDL video initialization: ") + SDL_GetError()); }
    ~SdlLifetime() { SDL_Quit(); }
};
} // namespace

int cmd_gpu_compare(int argc, char **argv) try {
    auto o = parse(argc, argv);
    SdlLifetime sdl;
    auto canonical_video = o.video;
    if (!o.scale_changes.empty() || o.video.scale > f3rt::GameVideoOptions::max_scale) canonical_video.scale = 1;
    auto owner = runner::create_strict_machine(o.rom_dir, std::string(f3rt::game_config::id), true, canonical_video.scale);
    auto &m = *owner;
    m.game_video->set_gpu_scale(o.video.scale);
    f3rt::GpuVideo gpu(nullptr, o.video, m.video->playfield_tiles(), m.video->sprite_tiles());
    if (!o.dump_dir.empty()) std::filesystem::create_directories(o.dump_dir);
    std::cout << "GPU driver=" << gpu.driver() << " seed=" << o.seed << " scale=" << o.video.scale << " border=" << o.video.border << '\n';
    Harness h(m, gpu, o, canonical_video);
    runner::SeededScheduleInputSource schedule(o.seed, runner::ScheduleConfig{.versus = false});
    std::array<int16_t, 8192> audio{};
    uint64_t audio_frames = 0, nonzero = 0, fallback_frames = 0, supported_frames = 0, transitions = 0;
    uint32_t audio_crc = 0xffffffffu;
    int peak = 0;
    bool previous_fallback = true;
    std::vector<double> native_frame_ms;
    if (o.bench) native_frame_ms.reserve(o.frames);
    std::vector<uint8_t> last_supported_pre;
    constexpr std::array<const char *, 4> scenarios{"bitmap", "trails", "globalflip", "sprite-boundaries"};
    const auto start = Clock::now();
    size_t next_scale_change = 0;
    try {
        while (m.frame < o.frames) {
            runner::apply_gameplay_word(m, schedule.step(m.frame)[0]);
            const bool injection_frame = m.frame + 1 == o.inject_frame && std::any_of(o.injections.begin(), o.injections.end(), [](bool b) { return b; });
            auto pre = (o.bench || injection_frame) ? snapshot(m) : std::vector<uint8_t>{};
            const auto native_start = Clock::now();
            advance(m);
            h.native_ms = o.bench ? std::chrono::duration<double, std::milli>(Clock::now() - native_start).count() : 0;
            bool scale_changed = false;
            if (next_scale_change < o.scale_changes.size() && o.scale_changes[next_scale_change].first == m.frame) {
                h.set_scale(o.scale_changes[next_scale_change++].second);
                scale_changed = true;
            }
            const bool fallback = m.game_video->captured_frame().fallback;
            if (o.bench && !fallback && m.frame > 600) native_frame_ms.push_back(h.native_ms);
            if (fallback) ++fallback_frames; else ++supported_frames;
            if (fallback != previous_fallback) ++transitions;
            previous_fallback = fallback;
            if (o.bench && !fallback) last_supported_pre = pre;
            if ((m.frame - 1) % o.every == 0 || m.frame == o.frames || injection_frame || h.requested_capture() || scale_changed)
                h.sample(scale_changed ? "scale_change" : "gameplay", false, m.frame == o.frames || scale_changed);
            if (injection_frame) {
                if (fallback) throw std::runtime_error("Scheduled injection needs a supported baseline; choose --inject-frame in gameplay");
                const auto baseline = snapshot(m);
                const uint32_t baseline_crc = m.state_crc();
                const auto baseline_blocks = m.native_blocks;
                if (o.injections[1]) verify_deferred_native(h);
                if (o.injections[1]) {
                    verify_trail_history(h);
                    m.load_state(pre); advance(m);
                    if (m.state_crc() != baseline_crc) throw std::runtime_error("Trail history recovery changed native state");
                }
                for (unsigned kind = 0; kind < scenarios.size(); ++kind) if (o.injections[kind]) {
                    m.load_state(baseline);
                    if (kind == 3) verify_sprite_boundaries(h);
                    else {
                        inject(h, kind);
                        h.sample(scenarios[kind], true, true);
                    }
                    // Restore pre-scanout and execute the original frame, rebuilding
                    // host snapshots with the correct sprite lag and producer state.
                    m.load_state(pre); advance(m);
                    if (m.game_video->captured_frame().fallback || m.state_crc() != baseline_crc)
                        throw std::runtime_error("Oracle-to-supported recovery changed baseline native state");
                    h.sample(std::string(scenarios[kind]) + "_restored", true);
                    std::cout << "INJECTED scenario=" << scenarios[kind] << " frame=" << m.frame
                              << (kind == 3 ? " supported_branch_and_recovery=exact" : " fallback_and_recovery=exact")
                              << " (branch only)\n";
                }
                m.native_blocks = baseline_blocks; // Diagnostic replays are not gameplay execution.
            }
            size_t count;
            while ((count = m.audio->render(audio.data(), audio.size() / 2)) != 0) {
                audio_frames += count;
                for (size_t i = 0; i < count * 2; ++i) {
                    peak = std::max(peak, std::abs(int(audio[i]))); nonzero += audio[i] != 0;
                    runner::crc_byte(audio_crc, uint8_t(audio[i])); runner::crc_byte(audio_crc, uint8_t(uint16_t(audio[i]) >> 8));
                }
            }
        }
        if (o.bench) {
            if (last_supported_pre.empty()) throw std::runtime_error("No supported snapshot available for benchmark");
            const auto final_state = snapshot(m);
            const auto final_crc = m.state_crc();
            const auto final_blocks = m.native_blocks;
            m.load_state(last_supported_pre); advance(m);
            if (m.game_video->captured_frame().fallback) throw std::runtime_error("Supported benchmark replay became fallback");
            h.native_ms = 0; // Frozen-snapshot repetitions are not native frame budgets.
            benchmark(h);
            timing("CPU_native_emulation_plus_native_compositor_and_scene_export", native_frame_ms);
            timing("CPU_threaded_varied_supported_compositor", h.cpu_frame_ms);
            timing("GPU_varied_supported_submit_fence_readback", h.gpu_frame_ms);
            if (h.interpolated_gpu) timing("GPU_interpolation_varied_submit_fence_readback", h.interpolation_ms);
            timing("CPU_native_plus_threaded_render_budget", h.cpu_budget_ms);
            timing("GPU_native_plus_fenced_render_budget", h.gpu_budget_ms);
            verify_cpu_backend(h, last_supported_pre);
            m.load_state(final_state);
            m.native_blocks = final_blocks;
            if (m.state_crc() != final_crc) throw std::runtime_error("Benchmark replay failed to restore final native state");
        }
    } catch (...) { h.report(); throw; }
    h.report();
    const double seconds = std::chrono::duration<double>(Clock::now() - start).count();
    timing("GPU_host_reference_and_resource_scale_change", h.scale_change_ms);
    const uint32_t frame_crc = f3rt::crc32(reinterpret_cast<const uint8_t *>(m.native_pixels().data()), m.native_pixels().size() * sizeof(uint32_t));
    std::cout << "SUCCESS seed=" << o.seed << " frames=" << m.frame << " supported_frames=" << supported_frames
        << " actual_fallback_frames=" << fallback_frames << " actual_oracle_transitions=" << transitions
        << " native_blocks=" << m.native_blocks << " fallback_instructions=" << m.fallback_instructions
        << " cycles=" << m.cpu.cycles << " sound_driver=native audio_frames=" << audio_frames
        << " audio_peak=" << peak << " nonzero_audio_samples=" << nonzero
        << " audio_crc=0x" << std::hex << (audio_crc ^ 0xffffffffu) << " frame_crc=0x" << frame_crc
        << " state_crc=0x" << m.state_crc() << std::dec << " elapsed_seconds=" << seconds
        << " harness_fps_including_diagnostics=" << m.frame / seconds << '\n';
    return 0;
} catch (const std::exception &e) {
    std::cerr << "GPU REGRESSION ERROR: " << e.what() << '\n';
    return 1;
}

} // namespace f3rt::tool
