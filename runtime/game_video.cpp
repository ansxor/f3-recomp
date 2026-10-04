#include "f3rt/game_video.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/video.hpp"
#include "game_tiles.hpp"
#include "game_text.hpp"
#include "game_sprites.hpp"
#include "game_lines.hpp"
#include "game_compositor.hpp"
#include "game_clip.hpp"
#include "gpu_scene.hpp"
#include "state_io.hpp"
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
        : machine(value), mode(selected), options(presentation_options), gpu_scale(presentation_options.scale) {}
    Machine &machine;
    GameVideoMode mode;
    GameVideoOptions options;
    unsigned gpu_scale;
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
    std::unique_ptr<GpuScene> gpu;
    struct Reference {
        GameTiles tiles;
        GameText text;
        GameLines lines;
        GameSprites sprites;
        std::vector<uint16_t> canonical_sprite_plane;
        std::vector<uint16_t> selected_sprite_plane;
        bool ready = false, canonical_ready = false, selected_ready = false;
        explicit Reference(GameVideoOptions options)
            : canonical_sprite_plane(plane_size(options)) {}
        static size_t plane_size(GameVideoOptions options) {
            return options.expanded() ? size_t(options.width()) * options.height() : 432 * 256;
        }
    };
    std::unique_ptr<Reference> reference;
    bool reference_selected = false;
    bool presentation_pending = false;

    void capture_gpu() {
        auto &scene = *gpu;
        scene.fallback = !rendered;
        if (scene.fallback)
            std::copy(machine.pixels.begin(), machine.pixels.end(), scene.native_pixels.begin());
        if (reference) {
            reference->ready = false;
            reference->canonical_ready = reference->selected_ready = false;
        }
        if (scene.fallback) return;
        auto &w = scene.words;
        for (unsigned l = 0; l < 4; ++l) for (unsigned i = 0; i < 2048; ++i) {
            const auto &c = tiles.maps_[l][i];
            const unsigned at = GpuScene::pf_cells + (l * 2048 + i) * 2;
            w[at] = c.tile;
            w[at + 1] = c.palette | (uint32_t(c.pen_mask) << 16) |
                (uint32_t(c.flip_x) << 24) | (uint32_t(c.flip_y) << 25) | (uint32_t(c.blend) << 26);
        }
        for (unsigned i = 0; i < 4096; ++i) {
            const auto &c = text.cells_[i];
            w[GpuScene::text_cells + i] = c.tile | (uint32_t(c.palette) << 8) |
                (uint32_t(c.flip_x) << 16) | (uint32_t(c.flip_y) << 17);
            const auto *pens = text.glyphs_.data() + i * 4;
            w[GpuScene::glyphs + i] = pens[0] | (uint32_t(pens[1]) << 8) |
                (uint32_t(pens[2]) << 16) | (uint32_t(pens[3]) << 24);
        }
        for (unsigned i = 0; i < 8192; ++i)
            w[GpuScene::palette + i] = (uint32_t(machine.palette[i * 4 + 1]) << 16) |
                (uint32_t(machine.palette[i * 4 + 2]) << 8) | machine.palette[i * 4 + 3];
        for (unsigned y = 0; y < 256; ++y) {
            const auto &r = lines.row(y);
            scene.reference_rows[y] = r;
            const unsigned at = GpuScene::rows + y * GpuScene::row_stride;
            w[at] = r.background;
            w[at + 1] = r.mosaic_period;
            w[at + 2] = uint32_t(r.text_x); w[at + 3] = uint32_t(r.text_y);
            w[at + 4] = r.blend[0] | (uint32_t(r.blend[1]) << 8) |
                (uint32_t(r.blend[2]) << 16) | (uint32_t(r.blend[3]) << 24);
            const auto order = scene_order(r);
            for (unsigned i = 0; i < 9; ++i) {
                w[at + 5 + i] = order[i];
                const auto &l = scene_layer(r, i);
                const unsigned la = at + GpuScene::row_layers + i * GpuScene::layer_stride;
                w[la] = l.priority | (uint32_t(l.blend_mode) << 4) | (uint32_t(l.enabled) << 6) |
                    (uint32_t(l.blend_select) << 7) | (uint32_t(l.mosaic) << 8);
                const auto clips = clip_ranges(r, l, int16_t(46 - int(options.border)), int16_t(366 + options.border));
                w[la + 1] = clips.count;
                for (unsigned c = 0; c < clips.count; ++c) {
                    w[la + 2 + c * 2] = uint32_t(clips.ranges[c].left);
                    w[la + 3 + c * 2] = uint32_t(clips.ranges[c].right);
                }
            }
            for (unsigned i = 0; i < 4; ++i) {
                const auto &p = r.playfields[i];
                const unsigned pa = at + GpuScene::row_pf + i * 6;
                w[pa] = uint32_t(p.source_x); w[pa + 1] = uint32_t(p.source_y);
                w[pa + 2] = uint32_t(p.x_step); w[pa + 3] = uint32_t(p.y_step);
                w[pa + 4] = p.y_fraction; w[pa + 5] = p.palette_add;
            }
        }
        const auto current = sprites.sprites();
        scene.sprite_count = unsigned(current.size());
        scene.pen_mask = sprites.pen_mask();
        for (unsigned i = 0; i < current.size(); ++i) {
            const auto &s = current[i];
            const unsigned at = GpuScene::sprites + i * GpuScene::sprite_stride;
            w[at] = uint32_t(s.x); w[at + 1] = uint32_t(s.y);
            w[at + 2] = s.scale_x; w[at + 3] = s.scale_y;
            w[at + 4] = s.tile; w[at + 5] = s.palette;
            w[at + 6] = unsigned(s.flip_x) | (unsigned(s.flip_y) << 1);
        }
    }

    void prepare_reference() {
        if (!reference) {
            reference = std::make_unique<Reference>(options);
            if (reference_selected && gpu_scale != options.scale) {
                auto selected = options;
                selected.scale = gpu_scale;
                reference->selected_sprite_plane.resize(Reference::plane_size(selected));
            }
        }
        auto &ref = *reference;
        if (ref.ready) return;
        const auto &w = gpu->words;
        for (unsigned l = 0; l < 4; ++l) for (unsigned i = 0; i < 2048; ++i) {
            auto &c = ref.tiles.maps_[l][i];
            const unsigned at = GpuScene::pf_cells + (l * 2048 + i) * 2;
            const auto a = w[at + 1];
            c.tile = uint16_t(w[at]); c.palette = uint16_t(a); c.pen_mask = uint8_t(a >> 16);
            c.flip_x = (a & (1u << 24)) != 0; c.flip_y = (a & (1u << 25)) != 0;
            c.blend = (a & (1u << 26)) != 0;
        }
        for (unsigned i = 0; i < 4096; ++i) {
            const auto a = w[GpuScene::text_cells + i];
            auto &c = ref.text.cells_[i];
            c.tile = uint8_t(a); c.palette = uint8_t(a >> 8);
            c.flip_x = (a & (1u << 16)) != 0; c.flip_y = (a & (1u << 17)) != 0;
            for (unsigned b = 0; b < 4; ++b) ref.text.glyphs_[i * 4 + b] = uint8_t(w[GpuScene::glyphs + i] >> (b * 8));
        }
        ref.sprites.current_count_ = gpu->sprite_count;
        ref.sprites.current_flipped_ = false;
        ref.sprites.current_trails_ = false;
        ref.sprites.current_pen_mask_ = uint8_t(gpu->pen_mask);
        for (unsigned i = 0; i < gpu->sprite_count; ++i) {
            const unsigned at = GpuScene::sprites + i * GpuScene::sprite_stride;
            auto &s = ref.sprites.current_sprites_[i];
            s.x = int32_t(w[at]); s.y = int32_t(w[at + 1]);
            s.scale_x = uint16_t(w[at + 2]); s.scale_y = uint16_t(w[at + 3]);
            s.tile = w[at + 4]; s.palette = uint8_t(w[at + 5]);
            s.flip_x = (w[at + 6] & 1) != 0; s.flip_y = (w[at + 6] & 2) != 0;
        }
        ref.ready = true;
    }

    void render_reference(std::span<uint32_t> output, GameVideoOptions opts, unsigned mask, bool serial,
                          bool canonical = false) {
        if (!gpu || opts.scale != (canonical ? options.scale : gpu_scale) || opts.border != options.border ||
            output.size() < size_t(opts.width()) * opts.height() || (mask & ~511u))
            throw std::runtime_error("Invalid GPU CPU-reference request");
        if (!canonical && !reference_selected) {
            reference_selected = true;
            if (reference && opts.scale != options.scale)
                reference->selected_sprite_plane.resize(Reference::plane_size(opts));
        }
        if (gpu->fallback) {
            const unsigned left = opts.border * opts.scale;
            for (unsigned y = 0; y < opts.height(); ++y) for (unsigned x = 0; x < opts.width(); ++x)
                output[size_t(y) * opts.width() + x] = x < left || x >= left + 320 * opts.scale ?
                    0xff000000u : gpu->native_pixels[(y / opts.scale) * 320 + (x - left) / opts.scale];
            return;
        }
        prepare_reference();
        auto &ref = *reference;
        const bool fixed_plane = opts.scale == options.scale;
        auto &plane = fixed_plane ? ref.canonical_sprite_plane : ref.selected_sprite_plane;
        auto &ready = fixed_plane ? ref.canonical_ready : ref.selected_ready;
        if (!ready) {
            ref.sprites.raster(machine.video->sprite_tiles(), plane, opts);
            ready = true;
        }
        ref.lines.rows_ = gpu->reference_rows;
        for (auto &r : ref.lines.rows_) {
            for (unsigned i = 0; i < 4; ++i) {
                r.playfields[i].layer.enabled &= (mask & (1u << i)) != 0;
                r.sprites[i].enabled &= (mask & (1u << (i + 4))) != 0;
            }
            r.text.enabled &= (mask & 256u) != 0;
        }
        const auto compositor = serial ? compose_game_scene_serial : compose_game_scene;
        compositor(ref.tiles, ref.text, ref.lines, plane, false,
                   machine.video->playfield_tiles(), std::span(gpu->words).subspan(GpuScene::palette, 8192),
                   output, opts);
    }

    void materialize_presentation() {
        if (!presentation_pending) return;
        // Snapshot/save paths must not lazily allocate row workers.
        render_reference(presentation_pixels, options, 511, true, true);
        // Canonical snapshots retain the NEXT-frame expanded sprite plane too.
        sprites.raster(machine.video->sprite_tiles(), presentation_sprites, options);
        presentation_pending = false;
    }
    uint64_t composite_frames = 0, composite_mismatches = 0;
    std::array<uint64_t, 9> frames{}, mismatches{};
    size_t state_size() const {
        return 1 +
               tiles.state_size() +
               text.state_size() +
               sprites.state_size() +
               lines.state_size() +
               sizeof(uint16_t) * 432 * 256 +
               sizeof(uint32_t) * pixels.size() +
               sizeof(uint32_t) * presentation_pixels.size() +
               sizeof(uint16_t) * presentation_sprites.size();
    }
    void save_state(StateWriter &writer) const {
        uint8_t r = rendered ? 1 : 0;
        writer.write(r);
        tiles.save_state(writer);
        text.save_state(writer);
        sprites.save_state(writer);
        lines.save_state(writer);
        writer.write_span(std::span<const uint16_t, 432 * 256>(sprite_plane));
        writer.write_span(std::span<const uint32_t, 320 * 232>(pixels));
        writer.write_span(std::span<const uint32_t>(presentation_pixels));
        writer.write_span(std::span<const uint16_t>(presentation_sprites));
    }
    void load_state(StateReader &reader) {
        uint8_t r;
        reader.read(r);
        rendered = r != 0;
        tiles.load_state(reader);
        text.load_state(reader);
        sprites.load_state(reader);
        lines.load_state(reader);
        reader.read_span(std::span<uint16_t, 432 * 256>(sprite_plane));
        reader.read_span(std::span<uint32_t, 320 * 232>(pixels));
        reader.read_span(std::span<uint32_t>(presentation_pixels));
        reader.read_span(std::span<uint16_t>(presentation_sprites));
    }
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
    impl_->presentation_pending = false;
    if (impl_->gpu) {
        impl_->gpu->fallback = true;
        impl_->gpu->native_pixels.fill(0xff000000);
    }
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
    const auto assets = state.machine.video->sprite_tiles();
    if (state.gpu && state.options.expanded() && state.sprites.reg_trails_ && !state.sprites.trails()) {
        // Seed retention with the preceding expanded plane before latching the
        // first trail list. Reconstructing only the final list loses history.
        state.sprites.raster(assets, state.presentation_sprites, state.options);
    }
    state.sprites.latch();
    state.sprites.raster(assets, state.sprite_plane);
    if (state.options.expanded() && (!state.gpu || state.sprites.trails()))
        state.sprites.raster(assets, state.presentation_sprites, state.options);
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
    if (!state.rendered && state.options.expanded() && !state.gpu) {
        // Unsupported geometry is never extrapolated into a made-up border.
        // Preserve the exact oracle image in the center, with black side bars.
        std::fill(state.presentation_pixels.begin(), state.presentation_pixels.end(), 0xff000000);
        const unsigned scale = state.options.scale, width = state.options.width();
        for (unsigned y = 0; y < state.options.height(); ++y)
            for (unsigned x = 0; x < 320 * scale; ++x)
                state.presentation_pixels[y * width + state.options.border * scale + x] = m.pixels[(y / scale) * 320 + x / scale];
    }
    if (state.gpu) {
        state.capture_gpu();
        state.presentation_pending = state.options.expanded();
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
    if (state.options.expanded() && !state.gpu)
        compose_game_scene(state.tiles, state.text, state.lines, state.presentation_sprites,
                           state.sprites.flipped(), state.machine.video->playfield_tiles(),
                           colors, state.presentation_pixels, state.options);
    state.rendered = true;
    ++state.rendered_frames;
}

std::span<const uint32_t> GameVideo::presentation() const {
    impl_->materialize_presentation();
    return impl_->options.expanded() ? std::span<const uint32_t>(impl_->presentation_pixels) : impl_->machine.pixels;
}

void GameVideo::enable_gpu_presentation(bool enabled) {
    if (enabled) {
        if (!impl_->gpu) impl_->gpu = std::make_unique<GpuScene>();
        // Only expanded canonical snapshots require eager reference storage.
        // Interactive GPU scaling never allocates an unused diagnostic plane.
        if (impl_->options.expanded() && !impl_->reference)
            impl_->reference = std::make_unique<Impl::Reference>(impl_->options);
    } else {
        impl_->materialize_presentation();
        impl_->gpu.reset();
        impl_->reference.reset();
        impl_->reference_selected = false;
    }
}
void GameVideo::set_gpu_scale(unsigned scale) {
    if (!impl_->gpu) throw std::runtime_error("GPU presentation snapshot is not enabled");
    if (!scale || scale > GameVideoOptions::max_gpu_scale)
        throw std::runtime_error("Game GPU reference scale must be 1..8");
    if (scale == impl_->gpu_scale) return;
    auto options = impl_->options;
    options.scale = scale;
    if (impl_->reference && impl_->reference_selected) {
        auto &ref = *impl_->reference;
        ref.selected_sprite_plane.resize(scale == impl_->options.scale ? 0 : Impl::Reference::plane_size(options));
        ref.selected_ready = false;
    }
    impl_->gpu_scale = scale;
}
const GpuScene &GameVideo::gpu_scene() const {
    if (!impl_->gpu) throw std::runtime_error("GPU presentation snapshot is not enabled");
    return *impl_->gpu;
}
void GameVideo::render_reference(std::span<uint32_t> output, GameVideoOptions options,
                                 unsigned layer_mask, bool serial) const {
    impl_->render_reference(output, options, layer_mask, serial);
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
size_t GameVideo::state_size() const {
    return impl_->state_size();
}
void GameVideo::save_state(std::span<uint8_t> dst) const {
    if (dst.size() != state_size()) {
        throw std::invalid_argument("GameVideo::save_state size mismatch");
    }
    impl_->materialize_presentation();
    StateWriter writer(dst);
    impl_->save_state(writer);
    if (writer.remaining() != 0) {
        throw std::logic_error("GameVideo::save_state remaining unwritten bytes");
    }
}
void GameVideo::load_state(std::span<const uint8_t> src) {
    if (src.size() != state_size()) {
        throw std::invalid_argument("GameVideo::load_state size mismatch");
    }
    StateReader reader(src);
    impl_->load_state(reader);
    impl_->presentation_pending = false;
    if (impl_->gpu) {
        impl_->gpu->fallback = true;
        std::copy(impl_->machine.pixels.begin(), impl_->machine.pixels.end(), impl_->gpu->native_pixels.begin());
        if (impl_->reference) impl_->reference->ready = false;
    }
    if (reader.remaining() != 0) {
        throw std::logic_error("GameVideo::load_state remaining unread bytes");
    }
}
} // namespace f3rt

extern "C" void f3_landmakr_video_hook(f3_cpu *cpu) {
    auto &machine = *static_cast<f3rt::Machine *>(cpu->runtime);
    if (machine.game_video) machine.game_video->observe();
}
