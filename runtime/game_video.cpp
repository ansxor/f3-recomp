#include "f3rt/game_video.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/video.hpp"
#include "game_tiles.hpp"
#include "game_text.hpp"
#include "game_sprites.hpp"
#include "game_lines.hpp"
#include "game_compositor.hpp"
#include "game_clip.hpp"
#include "game_video_log.hpp"
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
        const char *reason = nullptr;
        uint64_t frames = 0, first = 0, last = 0;
    };
    std::array<Fallback, 32> fallbacks{};
    unsigned fallback_count = 0;
    uint64_t rendered_frames = 0, fallback_frames = 0;
    // `component`/`reason` are string literals; repeats are counted, logged once.
    void fallback(const char *component, const char *reason) {
        const uint64_t frame = machine.frame + 1;
        log_unsupported_video(component, reason, frame);
        ++fallback_frames;
        for (unsigned i = 0; i < fallback_count; ++i) {
            auto &entry = fallbacks[i];
            if (entry.reason != reason) continue;
            ++entry.frames;
            entry.last = frame;
            return;
        }
        if (fallback_count == fallbacks.size()) throw std::runtime_error("Game video fallback reason capacity exceeded");
        fallbacks[fallback_count++] = {reason, 1, frame, frame};
    }
    GameTiles tiles;
    GameText text;
    GameSprites sprites;
    GameLines lines;
    // Only sprite_planes[next_plane] is canonical state. The other plane
    // retains the pending frame while latch_sprites prepares its successor.
    std::array<std::array<uint16_t, 432 * 256>, 2> sprite_planes{};
    unsigned next_plane = 0, native_plane = 0;
    bool native_pending = false;
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
            : canonical_sprite_plane(options.expanded() ? plane_size(options) : 0) {}
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
            std::copy(machine.pixels_.begin(), machine.pixels_.end(), scene.native_pixels.begin());
        if (reference) {
            reference->ready = false;
            reference->canonical_ready = reference->selected_ready = false;
        }
        if (scene.fallback) return;
        auto &w = scene.words;
        for (unsigned l = 0; l < 4; ++l) for (unsigned i = 0; i < 2048; ++i) {
            // Raw 4-byte video-RAM cell (attributes<<16 | code); the shader
            // decodes it exactly like GameTiles::RowSampler. Word 1 is unused.
            const unsigned at = GpuScene::pf_cells + (l * 2048 + i) * 2;
            w[at] = tiles.maps_[l][i];
        }
        for (unsigned i = 0; i < 4096; ++i) {
            // Raw big-endian text-map word; the shader decodes it exactly like
            // GameText::pixel (see runtime/game_text.hpp for the bit layout).
            w[GpuScene::text_cells + i] = text.map_[i];
            // Raw glyph RAM, byte-packed four bytes per word; the shader indexes
            // it as bytes, so only the first 2048 words of the region are used.
            w[GpuScene::glyphs + i] = uint32_t(text.glyph_ram_[i * 4]) |
                (uint32_t(text.glyph_ram_[i * 4 + 1]) << 8) |
                (uint32_t(text.glyph_ram_[i * 4 + 2]) << 16) |
                (uint32_t(text.glyph_ram_[i * 4 + 3]) << 24);
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
        for (unsigned l = 0; l < 4; ++l) for (unsigned i = 0; i < 2048; ++i)
            ref.tiles.maps_[l][i] = w[GpuScene::pf_cells + (l * 2048 + i) * 2];
        for (unsigned i = 0; i < 4096; ++i)
            ref.text.map_[i] = uint16_t(w[GpuScene::text_cells + i]);
        for (unsigned i = 0; i < 2048; ++i)
            for (unsigned b = 0; b < 4; ++b)
                ref.text.glyph_ram_[i * 4 + b] = uint8_t(w[GpuScene::glyphs + i] >> (b * 8));
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
            if (plane.empty()) plane.resize(Reference::plane_size(opts));
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

    void materialize_native() {
        if (!native_pending) return;
        prepare_reference();
        auto &ref = *reference;
        ref.lines.rows_ = gpu->reference_rows;
        // Neither live maps/palette nor the now-latched sprite list describe
        // this frame. Use the immutable capture and retained native plane.
        compose_game_scene_serial(ref.tiles, ref.text, ref.lines, sprite_planes[native_plane], false,
                                  machine.video->playfield_tiles(),
                                  std::span(gpu->words).subspan(GpuScene::palette, 8192), pixels);
        std::copy(pixels.begin(), pixels.end(), machine.pixels_.begin());
        native_pending = false;
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
        // Playfield tiles and text are derived from serialized video RAM, so
        // they are not part of snapshot state; decode() rebuilds them at VBSTART.
        return 1 +
               sprites.state_size() +
               lines.state_size() +
               sizeof(uint16_t) * 432 * 256 +
               sizeof(uint32_t) * pixels.size() +
               sizeof(uint32_t) * presentation_pixels.size() +
               sizeof(uint16_t) * presentation_sprites.size();
    }
    size_t sync_state_size() const {
        return state_size() - sizeof(uint32_t) * presentation_pixels.size() -
               sizeof(uint16_t) * presentation_sprites.size();
    }
    void save_state(StateWriter &writer, bool sync = false) const {
        uint8_t r = rendered ? 1 : 0;
        writer.write(r);
        sprites.save_state(writer);
        lines.save_state(writer);
        writer.write_span(std::span<const uint16_t, 432 * 256>(sprite_planes[next_plane]));
        writer.write_span(std::span<const uint32_t, 320 * 232>(pixels));
        if (!sync) {
            writer.write_span(std::span<const uint32_t>(presentation_pixels));
            writer.write_span(std::span<const uint16_t>(presentation_sprites));
        }
    }
    void load_state(StateReader &reader, bool sync = false) {
        uint8_t r;
        reader.read(r);
        rendered = r != 0;
        sprites.load_state(reader);
        lines.load_state(reader);
        native_pending = false;
        next_plane = native_plane = 0;
        reader.read_span(std::span<uint16_t, 432 * 256>(sprite_planes[next_plane]));
        reader.read_span(std::span<uint32_t, 320 * 232>(pixels));
        if (!sync) {
            reader.read_span(std::span<uint32_t>(presentation_pixels));
            reader.read_span(std::span<uint16_t>(presentation_sprites));
        }
    }
    void reseed_presentation() {
        // Imported native retention is authoritative. Do not reconstruct a
        // previous frame from the now-latched NEXT-frame sprite list.
        std::fill(presentation_sprites.begin(), presentation_sprites.end(), 0);
        std::fill(presentation_pixels.begin(), presentation_pixels.end(), 0xff000000);
        if (options.expanded()) {
            const unsigned scale = options.scale, width = options.width();
            for (unsigned y = 0; y < options.height(); ++y)
                for (unsigned x = 0; x < 320 * scale; ++x) {
                    const size_t at = size_t(y) * width + options.border * scale + x;
                    presentation_pixels[at] = machine.pixels_[(y / scale) * 320 + x / scale];
                    presentation_sprites[at] = sprite_planes[next_plane][(y / scale + 24) * 432 + x / scale + 46];
                }
        }
        presentation_pending = false;
        if (gpu) {
            gpu->fallback = true;
            std::copy(machine.pixels_.begin(), machine.pixels_.end(), gpu->native_pixels.begin());
        }
        if (reference) {
            reference->ready = false;
            reference->canonical_ready = reference->selected_ready = false;
            std::fill(reference->canonical_sprite_plane.begin(), reference->canonical_sprite_plane.end(), 0);
            std::fill(reference->selected_sprite_plane.begin(), reference->selected_sprite_plane.end(), 0);
        }
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
    impl_->materialize_native();
    impl_->materialize_presentation();
    impl_->tiles.reset();
    impl_->text.reset();
    impl_->sprites.reset();
    impl_->lines.reset();
    for (auto &plane : impl_->sprite_planes) plane.fill(0);
    impl_->next_plane = impl_->native_plane = 0;
    impl_->native_pending = false;
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
#ifdef F3RT_VIDEO_WRITE_LOG
void GameVideo::observe_write(uint32_t pc, uint32_t address) {
    const uint64_t frame = impl_->machine.frame + 1;
    impl_->tiles.observe_write(pc, address, frame);
    impl_->text.observe_write(pc, address, frame);
    impl_->sprites.observe_write(pc, address, frame);
    impl_->lines.observe_write(pc, address, frame);
}
#endif

void GameVideo::latch_sprites() {
    auto &state = *impl_;
    const auto assets = state.machine.video->sprite_tiles();
    if (state.gpu && state.options.expanded() && state.sprites.reg_trails_ && !state.sprites.trails()) {
        // Seed retention with the preceding expanded plane before latching the
        // first trail list. Reconstructing only the final list loses history.
        state.sprites.raster(assets, state.presentation_sprites, state.options);
    }
    // A trail latch updates the prior plane in place. Finish its pending
    // composite first rather than copying a full plane just to preserve it.
    if (state.native_pending && state.sprites.reg_trails_) state.materialize_native();
    state.sprites.latch();
    if (state.native_pending) state.next_plane ^= 1;
    state.sprites.raster(assets, state.sprite_planes[state.next_plane]);
    if (state.options.expanded() && (!state.gpu || state.sprites.trails()))
        state.sprites.raster(assets, state.presentation_sprites, state.options);
}

void GameVideo::render_frame() {
    auto &state = *impl_;
    auto &m = state.machine;
    // VBSTART: video RAM is the source of truth. Decode this frame's playfield,
    // text, sprite (next submission) and scanline layers from it before rendering.
    // Render below still uses the previously latched sprite plane, preserving the
    // one-frame FDP sprite lag.
    const VideoRam vram{m.graphics, m.control, m.frame + 1};
    state.tiles.decode(vram);
    state.text.decode(vram);
    state.sprites.decode(vram);
    state.lines.decode(vram);
    render();
    if (state.rendered && state.mode == GameVideoMode::Game) {
        if (!state.gpu)
            std::copy(state.pixels.begin(), state.pixels.end(), m.pixels_.begin());
        // Keep only the oracle's sprite lag current, so an unsupported future
        // frame can use its exact visible result without rerunning the CPU.
        m.video->vblank(m.graphics);
    } else {
        // The game composite is retained across oracle fallback frames, even
        // though Machine scanout now comes from the oracle.
        state.materialize_native();
        m.video->render_frame(m.palette, m.graphics, m.control, m.pixels_);
        if (state.rendered && state.mode == GameVideoMode::Compare) compare_composite(m.frame + 1);
    }
    if (!state.rendered && state.options.expanded() && !state.gpu) {
        // Unsupported geometry is never extrapolated into a made-up border.
        // Preserve the exact oracle image in the center, with black side bars.
        std::fill(state.presentation_pixels.begin(), state.presentation_pixels.end(), 0xff000000);
        const unsigned scale = state.options.scale, width = state.options.width();
        for (unsigned y = 0; y < state.options.height(); ++y)
            for (unsigned x = 0; x < 320 * scale; ++x)
                state.presentation_pixels[y * width + state.options.border * scale + x] = m.pixels_[(y / scale) * 320 + x / scale];
    }
    if (state.gpu) {
        // Only a supported successor may discard an unobserved composite.
        state.native_pending = false;
        state.capture_gpu();
        state.native_pending = state.rendered && state.mode == GameVideoMode::Game;
        state.native_plane = state.next_plane;
        state.presentation_pending = state.options.expanded();
    }
    latch_sprites();
}

void GameVideo::render() {
    auto &state = *impl_;
    state.rendered = false;
    // Every layer decodes from video RAM, so only these features the scene
    // renderer does not draw force the FDP oracle fallback.
    if (state.sprites.flipped()) { state.fallback("sprites", "flipped-screen"); return; }
    if (state.sprites.trails()) { state.fallback("sprites", "sprite-trails"); return; }
    state.lines.prepare(state.sprites.flipped());
    for (unsigned y = 24; y < 256; ++y)
        if (state.lines.row(y).bitmap) { state.fallback("text", "bitmap-pivot"); return; }
    if (state.mode != GameVideoMode::Game || !state.gpu) {
        std::array<uint32_t, 8192> colors;
        const auto &palette = state.machine.palette;
        for (unsigned i = 0; i < colors.size(); ++i)
            colors[i] = (uint32_t(palette[i * 4 + 1]) << 16) | (uint32_t(palette[i * 4 + 2]) << 8) | palette[i * 4 + 3];
        compose_game_scene(state.tiles, state.text, state.lines, state.sprite_planes[state.next_plane],
                           state.sprites.flipped(), state.machine.video->playfield_tiles(),
                           colors, state.pixels);
        if (state.options.expanded() && !state.gpu)
            compose_game_scene(state.tiles, state.text, state.lines, state.presentation_sprites,
                               state.sprites.flipped(), state.machine.video->playfield_tiles(),
                               colors, state.presentation_pixels, state.options);
    }
    state.rendered = true;
    ++state.rendered_frames;
}

void GameVideo::materialize_native() const {
    impl_->materialize_native();
}
std::span<const uint32_t> GameVideo::presentation() const {
    impl_->materialize_presentation();
    return impl_->options.expanded() ? std::span<const uint32_t>(impl_->presentation_pixels) :
                                     std::span<const uint32_t>(impl_->machine.native_pixels());
}

void GameVideo::enable_gpu_presentation(bool enabled) {
    impl_->materialize_native();
    impl_->materialize_presentation();
    if (enabled) {
        if (!impl_->gpu) impl_->gpu = std::make_unique<GpuScene>();
        // Native and expanded snapshot observations must not allocate.
        if ((impl_->options.expanded() || impl_->mode == GameVideoMode::Game) && !impl_->reference)
            impl_->reference = std::make_unique<Impl::Reference>(impl_->options);
    } else {
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
    impl_->materialize_native();
    impl_->materialize_presentation();
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
        // Every layer is decoded from video RAM at VBSTART; nothing is
        // unsupported by producer identity.
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
                    const uint16_t actual = impl_->sprite_planes[impl_->next_plane][offset];
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
    impl_->materialize_native();
    if (!impl_->rendered) {
        std::ostringstream error;
        error << "Game composite fallback at frame " << frame;
        throw std::runtime_error(error.str());
    }
    uint64_t count = 0;
    unsigned first = 0;
    const auto &oracle = impl_->machine.pixels_;
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
        output << "VIDEO fallback=" << entry.reason << " frames=" << entry.frames
               << " first=" << entry.first << " last=" << entry.last << '\n';
    }
}
size_t GameVideo::state_size() const {
    return impl_->state_size();
}
void GameVideo::save_state(std::span<uint8_t> dst) const {
    if (dst.size() != state_size()) {
        throw std::invalid_argument("GameVideo::save_state size mismatch");
    }
    impl_->materialize_native();
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
    impl_->materialize_native();
    impl_->materialize_presentation();
    StateReader reader(src);
    impl_->load_state(reader);
    impl_->presentation_pending = false;
    if (impl_->gpu) {
        impl_->gpu->fallback = true;
        std::copy(impl_->machine.pixels_.begin(), impl_->machine.pixels_.end(), impl_->gpu->native_pixels.begin());
        if (impl_->reference) impl_->reference->ready = false;
    }
    if (reader.remaining() != 0) {
        throw std::logic_error("GameVideo::load_state remaining unread bytes");
    }
}
size_t GameVideo::sync_state_size() const {
    return impl_->sync_state_size();
}
void GameVideo::save_sync_state(std::span<uint8_t> dst) const {
    if (dst.size() != sync_state_size())
        throw std::invalid_argument("GameVideo::save_sync_state size mismatch");
    impl_->materialize_native();
    StateWriter writer(dst);
    impl_->save_state(writer, true);
    if (writer.remaining()) throw std::logic_error("GameVideo sync snapshot unwritten bytes");
}
void GameVideo::load_sync_state(std::span<const uint8_t> src) {
    if (src.size() != sync_state_size())
        throw std::invalid_argument("GameVideo::load_sync_state size mismatch");
    impl_->materialize_native();
    impl_->materialize_presentation();
    StateReader reader(src);
    impl_->load_state(reader, true);
    if (reader.remaining()) throw std::logic_error("GameVideo sync snapshot unread bytes");
    impl_->reseed_presentation();
}
} // namespace f3rt
