#include "f3rt/game_video.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/video.hpp"
#include "game_tiles.hpp"
#include "game_text.hpp"
#include "game_sprites.hpp"
#include "game_lines.hpp"
#include "game_compositor.hpp"
#include <algorithm>
#include <array>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <vector>

namespace f3rt {
namespace {
constexpr std::array<const char *, 9> layer_names{"pf0", "pf1", "pf2", "pf3", "sp0", "sp1", "sp2", "sp3", "text"};
bool differs(ScenePixel game, ScenePixel oracle) {
    const bool visible = (game.flags & 0x10) != 0;
    return visible != ((oracle.flags & 0x10) != 0) ||
        (visible && (game.palette != oracle.palette || game.flags != oracle.flags));
}
}
struct GameVideo::Impl {
    Impl(Machine &value, GameVideoMode selected, GameVideoOptions presentation_options)
        : machine(value), mode(selected), options(presentation_options) {}
    Machine &machine;
    GameVideoMode mode;
    GameVideoOptions options;
    std::vector<uint32_t> presentation_pixels;
    std::vector<uint16_t> presentation_sprites;
    struct Fallback {
        const char *component = nullptr;
        uint32_t pc = 0;
        uint64_t frames = 0, first = 0, last = 0;
    };
    std::array<Fallback, 32> fallbacks{};
    unsigned fallback_count = 0;
    uint64_t rendered_frames = 0, fallback_frames = 0;
    void fallback(const char *component, uint32_t pc) {
        const uint64_t frame = machine.frame + 1;
        ++fallback_frames;
        for (unsigned i = 0; i < fallback_count; ++i) {
            auto &entry = fallbacks[i];
            if (entry.component != component || entry.pc != pc) continue;
            ++entry.frames;
            entry.last = frame;
            return;
        }
        if (fallback_count == fallbacks.size()) throw std::runtime_error("Game video fallback reason capacity exceeded");
        fallbacks[fallback_count++] = {component, pc, 1, frame, frame};
    }
    GameTiles tiles;
    GameText text;
    GameSprites sprites;
    GameLines lines;
    std::array<uint16_t, 432 * 256> sprite_plane{};
    std::array<uint32_t, 320 * 232> pixels{};
    bool rendered = false;
    uint64_t composite_frames = 0, composite_mismatches = 0;
    std::array<uint64_t, 9> frames{}, mismatches{};
};

GameVideo::GameVideo(Machine &machine, GameVideoMode mode, GameVideoOptions options)
    : impl_(std::make_unique<Impl>(machine, mode, options)) {
    if (!options.scale || options.scale > GameVideoOptions::max_scale || options.border > GameVideoOptions::max_border)
        throw std::runtime_error("Game presentation scale must be 1..4 and border 0..160");
    if (options.expanded()) {
        const size_t size = size_t(options.width()) * options.height();
        impl_->presentation_pixels.resize(size);
        impl_->presentation_sprites.resize(size);
    }
    machine.video->enable_scene_inspection(mode != GameVideoMode::Game);
    reset();
}
GameVideo::~GameVideo() = default;
void GameVideo::reset() {
    impl_->tiles.reset();
    impl_->text.reset();
    impl_->sprites.reset();
    impl_->lines.reset();
    impl_->sprite_plane.fill(0);
    std::fill(impl_->presentation_sprites.begin(), impl_->presentation_sprites.end(), 0);
    std::fill(impl_->presentation_pixels.begin(), impl_->presentation_pixels.end(), 0xff000000);
    impl_->frames.fill(0);
    impl_->mismatches.fill(0);
    impl_->rendered = false;
    impl_->composite_frames = impl_->composite_mismatches = 0;
    impl_->fallback_count = 0;
    impl_->rendered_frames = impl_->fallback_frames = 0;
}
void GameVideo::observe() {
    auto &m = impl_->machine;
    GameMemory memory{m.roms.main, m.ram};
    impl_->tiles.observe(memory, m.cpu);
    memory.supported = true;
    impl_->text.observe(memory, m.cpu);
    memory.supported = true;
    impl_->sprites.observe(memory, m.cpu);
    memory.supported = true;
    impl_->lines.observe(memory, m.cpu);
}
void GameVideo::observe_write(uint32_t pc, uint32_t address) {
    impl_->tiles.observe_write(pc, address);
    impl_->text.observe_write(pc, address);
    impl_->sprites.observe_write(pc, address);
    impl_->lines.observe_write(pc, address);
}

void GameVideo::latch_sprites() {
    auto &state = *impl_;
    state.sprites.latch();
    const auto assets = state.machine.video->sprite_tiles();
    state.sprites.raster(assets, state.sprite_plane);
    if (state.options.expanded()) state.sprites.raster(assets, state.presentation_sprites, state.options);
}

void GameVideo::render_frame() {
    auto &state = *impl_;
    auto &m = state.machine;
    render();
    if (state.rendered && state.mode == GameVideoMode::Game) {
        std::copy(state.pixels.begin(), state.pixels.end(), m.pixels.begin());
        // Keep only the oracle's sprite lag current, so an unsupported future
        // frame can use its exact visible result without rerunning the CPU.
        m.video->vblank(m.graphics);
    } else {
        m.video->render_frame(m.palette, m.graphics, m.control, m.pixels);
        if (state.rendered && state.mode == GameVideoMode::Compare) compare_composite(m.frame + 1);
    }
    if (!state.rendered && state.options.expanded()) {
        // Unsupported geometry is never extrapolated into a made-up border.
        // Preserve the exact oracle image in the center, with black side bars.
        std::fill(state.presentation_pixels.begin(), state.presentation_pixels.end(), 0xff000000);
        const unsigned scale = state.options.scale, width = state.options.width();
        for (unsigned y = 0; y < state.options.height(); ++y)
            for (unsigned x = 0; x < 320 * scale; ++x)
                state.presentation_pixels[y * width + state.options.border * scale + x] = m.pixels[(y / scale) * 320 + x / scale];
    }
    latch_sprites();
}

void GameVideo::render() {
    auto &state = *impl_;
    state.rendered = false;
    if (!state.lines.supported()) { state.fallback("lines", state.lines.unsupported_pc()); return; }
    if (!state.text.supported()) { state.fallback("text", state.text.unsupported_pc()); return; }
    if (!state.sprites.supported()) { state.fallback("sprites", state.sprites.unsupported_pc()); return; }
    // Descriptor decoding retains these command bits, but their complete
    // scanout behavior is outside the measured normal-orientation contract.
    if (state.sprites.flipped()) { state.fallback("flipped-screen", 0x43e0); return; }
    if (state.sprites.trails()) { state.fallback("sprite-trails", 0x43e0); return; }
    for (unsigned layer = 0; layer < 4; ++layer) {
        if (state.tiles.supported(layer)) continue;
        state.fallback(layer_names[layer], state.tiles.unsupported_pc(layer));
        return;
    }
    state.lines.prepare(state.sprites.flipped());
    for (unsigned y = 24; y < 256; ++y)
        if (state.lines.row(y).bitmap) { state.fallback("bitmap-pivot", 0); return; }
    std::array<uint32_t, 8192> colors;
    const auto &palette = state.machine.palette;
    for (unsigned i = 0; i < colors.size(); ++i)
        colors[i] = (uint32_t(palette[i * 4 + 1]) << 16) | (uint32_t(palette[i * 4 + 2]) << 8) | palette[i * 4 + 3];
    compose_game_scene(state.tiles, state.text, state.lines, state.sprite_plane,
                       state.sprites.flipped(), state.machine.video->playfield_tiles(),
                       colors, state.pixels);
    if (state.options.expanded())
        compose_game_scene(state.tiles, state.text, state.lines, state.presentation_sprites,
                           state.sprites.flipped(), state.machine.video->playfield_tiles(),
                           colors, state.presentation_pixels, state.options);
    state.rendered = true;
    ++state.rendered_frames;
}

std::span<const uint32_t> GameVideo::presentation() const {
    return impl_->options.expanded() ? std::span<const uint32_t>(impl_->presentation_pixels) : impl_->machine.pixels;
}

void GameVideo::compare_layers(uint64_t frame, unsigned layer_mask) {
    if (!layer_mask || (layer_mask & ~511u)) throw std::runtime_error("Video layer mask must select PF0..3, SP0..3, text (bits 0..8)");
    auto &m = impl_->machine;
    const auto assets = m.video->playfield_tiles();
    if (layer_mask & 256) m.video->prepare_text_inspection(m.graphics);
    if (layer_mask == 511) impl_->lines.compare_rows(*m.video, frame);
    for (unsigned layer = 0; layer < 9; ++layer) {
        if (!(layer_mask & (1u << layer))) continue;
        const bool supported = layer < 4 ? impl_->tiles.supported(layer) : layer < 8 ? impl_->sprites.supported() : impl_->text.supported();
        if (!supported) {
            const uint32_t pc = layer < 4 ? impl_->tiles.unsupported_pc(layer) : layer < 8 ? impl_->sprites.unsupported_pc() : impl_->text.unsupported_pc();
            std::ostringstream error;
            error << "Game " << layer_names[layer] << " unsupported producer at frame " << frame
                  << " PC 0x" << std::hex << pc;
            throw std::runtime_error(error.str());
        }
        uint64_t count = 0;
        int first_x = -1, first_y = -1;
        ScenePixel first_game{}, first_oracle{};
        const int width = layer < 4 ? 1024 : layer < 8 ? 320 : 512;
        const int height = layer < 4 || layer == 8 ? 512 : 232;
        for (int y = 0; y < height; ++y) {
            const auto oracle = layer < 4 ? m.video->inspect_playfield_line(layer, y, m.graphics) :
                layer == 8 ? m.video->inspect_text_line(y, m.graphics) : VideoLine{};
            for (int x = 0; x < width; ++x) {
                ScenePixel game{}, expected{};
                if (layer < 4) game = impl_->tiles.playfield_pixel(layer, x, y, m.video->flipscreen(), assets);
                else if (layer == 8) game = impl_->text.pixel(x, y, m.video->flipscreen());
                if (layer < 4 || layer == 8) expected = {oracle.palette[x], oracle.flags[x]};
                else {
                    const unsigned offset = (y + 24) * 432 + x + 46;
                    const uint16_t actual = impl_->sprite_plane[offset];
                    const uint16_t reference = m.video->sprite_plane()[offset];
                    game = {actual, uint8_t(actual && ((actual >> 10) & 3) == layer - 4 ? 0x10 : 0)};
                    expected = {reference, uint8_t(reference && ((reference >> 10) & 3) == layer - 4 ? 0x10 : 0)};
                }
                if (differs(game, expected)) {
                    if (!count) { first_x = x; first_y = y; first_game = game; first_oracle = expected; }
                    ++count;
                }
            }
        }
        ++impl_->frames[layer];
        impl_->mismatches[layer] += count;
        if (count) {
            std::ostringstream error;
            error << "Game " << layer_names[layer] << " frame " << frame << ": " << count
                  << " indexed pixel mismatches; first (" << first_x << ',' << first_y << ") game=0x"
                  << std::hex << first_game.palette << '/' << unsigned(first_game.flags)
                  << " oracle=0x" << first_oracle.palette << '/' << unsigned(first_oracle.flags);
            if (layer >= 4 && layer < 8) {
                for (const auto &sprite : impl_->sprites.sprites()) {
                    const int dx = (sprite.x >> 8) - (first_x + 46);
                    const int dy = (sprite.y >> 8) - (first_y + 24);
                    if (dx < -32 || dx > 32 || dy < -32 || dy > 32) continue;
                    error << "\n nearby tile=0x" << std::hex << sprite.tile << " palette=0x" << unsigned(sprite.palette)
                          << std::dec << " xy8=(" << sprite.x << ',' << sprite.y << ") scale=("
                          << sprite.scale_x << ',' << sprite.scale_y << ") flip=(" << sprite.flip_x << ',' << sprite.flip_y << ')';
                }
            }
            throw std::runtime_error(error.str());
        }
    }
    if (layer_mask == 511) compare_composite(frame);
}

void GameVideo::compare_composite(uint64_t frame) {
    if (!impl_->rendered) {
        std::ostringstream error;
        error << "Game composite unsupported at frame " << frame << " line producer PC 0x"
              << std::hex << impl_->lines.unsupported_pc();
        throw std::runtime_error(error.str());
    }
    uint64_t count = 0;
    unsigned first = 0;
    const auto &oracle = impl_->machine.pixels;
    for (unsigned i = 0; i < impl_->pixels.size(); ++i) {
        if (impl_->pixels[i] == oracle[i]) continue;
        if (!count) first = i;
        ++count;
    }
    ++impl_->composite_frames;
    impl_->composite_mismatches += count;
    if (count) {
        std::ostringstream error;
        error << "Game composite frame " << frame << ": " << count << " RGB pixel mismatches; first ("
              << first % 320 << ',' << first / 320 << ") game=0x" << std::hex << impl_->pixels[first]
              << " oracle=0x" << oracle[first];
        throw std::runtime_error(error.str());
    }
}

void GameVideo::report(std::ostream &output) const {
    for (unsigned layer = 0; layer < 9; ++layer) {
        if (!impl_->frames[layer]) continue;
        const unsigned pixels = layer < 4 ? 1024 * 512 : layer < 8 ? 320 * 232 : 512 * 512;
        output << "VIDEO layer=" << layer_names[layer] << " domain="
               << (layer < 4 ? "1024x512-indexed-texture" : layer < 8 ? "320x232-next-sprite-plane" : "512x512-indexed-texture")
               << " sampled_frames=" << impl_->frames[layer]
               << " compared_pixels=" << impl_->frames[layer] * pixels
               << " pixel_mismatches=" << impl_->mismatches[layer] << '\n';
    }
    if (impl_->composite_frames)
        output << "VIDEO layer=composite domain=320x232-RGB sampled_frames=" << impl_->composite_frames
               << " compared_pixels=" << impl_->composite_frames * 320 * 232
               << " pixel_mismatches=" << impl_->composite_mismatches << '\n';
    output << "VIDEO game_frames=" << impl_->rendered_frames << " oracle_fallback_frames=" << impl_->fallback_frames << '\n';
    for (unsigned i = 0; i < impl_->fallback_count; ++i) {
        const auto &entry = impl_->fallbacks[i];
        output << "VIDEO fallback=" << entry.component << " producer_pc=0x" << std::hex << entry.pc << std::dec
               << " frames=" << entry.frames << " first=" << entry.first << " last=" << entry.last << '\n';
    }
}
} // namespace f3rt

extern "C" void f3_landmakr_video_hook(f3_cpu *cpu) {
    auto &machine = *static_cast<f3rt::Machine *>(cpu->runtime);
    if (machine.game_video) machine.game_video->observe();
}
