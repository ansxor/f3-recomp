#include "f3rt/game_video.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/video.hpp"
#include "renderer/game/tiles.hpp"
#include "renderer/game/text.hpp"
#include "renderer/game/sprites.hpp"
#include "renderer/game/lines.hpp"
#include "renderer/game/compositor.hpp"
#include "renderer/game/clip.hpp"
#include "renderer/game/video_log.hpp"
#include "renderer/game/captured_frame.hpp"
#include "state_io.hpp"
#include "sprite_units.hpp"
#include <algorithm>
#include <array>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <vector>
#include <tuple>

namespace f3rt {
namespace {
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
    std::array<uint32_t, geometry::native_pixels> pixels{};
    bool rendered = false;
    // Present exactly when GPU presentation is enabled. Overwritten once per
    // supported or fallback frame by capture(); the CPU compositor and the GPU
    // encoder both read it.
    std::unique_ptr<CapturedFrame> captured;
    // Reference-scale sprite planes, rastered lazily from `captured`; stale after every recapture.
    struct ReferencePlanes {
        std::vector<uint16_t> canonical_sprite_plane;
        std::vector<uint16_t> selected_sprite_plane;
        bool canonical_ready = false, selected_ready = false;
        explicit ReferencePlanes(GameVideoOptions options)
            : canonical_sprite_plane(options.expanded() ? plane_size(options) : 0) {}
        static size_t plane_size(GameVideoOptions options) {
            return options.expanded() ? size_t(options.width()) * options.height() : 432 * 256;
        }
        void invalidate() { canonical_ready = selected_ready = false; }
    };
    std::unique_ptr<ReferencePlanes> reference;
    bool reference_selected = false;
    bool presentation_pending = false;

    void capture() {
        auto &frame = *captured;
        frame.fallback = !rendered;
        if (reference) reference->invalidate();
        if (frame.fallback) {
            std::copy(machine.pixels_.begin(), machine.pixels_.end(), frame.native_pixels.begin());
            return;
        }
        frame.tiles = tiles;
        frame.text = text;
        std::copy(lines.rows().begin(), lines.rows().end(), frame.rows.begin());
        const auto current = sprites.presented_sprites();
        frame.sprite_count = unsigned(current.size());
        std::copy(current.begin(), current.end(), frame.sprites.begin());
        frame.pen_mask = sprites.pen_mask();
        const auto &palette = machine.palette;
        for (unsigned i = 0; i < frame.colors.size(); ++i)
            frame.colors[i] = (uint32_t(palette[i * 4 + 1]) << 16) | (uint32_t(palette[i * 4 + 2]) << 8) | palette[i * 4 + 3];
    }

    ReferencePlanes &reference_planes() {
        if (!reference) {
            reference = std::make_unique<ReferencePlanes>(options);
            if (reference_selected && gpu_scale != options.scale) {
                auto selected = options;
                selected.scale = gpu_scale;
                reference->selected_sprite_plane.resize(ReferencePlanes::plane_size(selected));
            }
        }
        return *reference;
    }

    FrameScene live_scene(std::span<const uint32_t> colors) const {
        return {tiles, text, lines.rows(), machine.video->playfield_tiles(), colors, all_layers,
                sprites.flipped(), true};
    }
    FrameScene captured_scene(uint32_t mask = all_layers) const {
        return captured->scene(machine.video->playfield_tiles(), mask);
    }

    void render_reference(std::span<uint32_t> output, GameVideoOptions opts, unsigned mask, bool serial,
                          bool canonical = false) {
        if (!captured || opts.scale != (canonical ? options.scale : gpu_scale) || opts.border != options.border ||
            output.size() < size_t(opts.width()) * opts.height() || (mask & ~all_layers))
            throw std::runtime_error("Invalid GPU CPU-reference request");
        if (!canonical && !reference_selected) {
            reference_selected = true;
            if (reference && opts.scale != options.scale)
                reference->selected_sprite_plane.resize(ReferencePlanes::plane_size(opts));
        }
        if (captured->fallback) {
            const unsigned left = opts.border * opts.scale;
            for (unsigned y = 0; y < opts.height(); ++y) for (unsigned x = 0; x < opts.width(); ++x)
                output[size_t(y) * opts.width() + x] = x < left || x >= left + geometry::native_width * opts.scale ?
                    0xff000000u : captured->native_pixels[(y / opts.scale) * geometry::native_width + (x - left) / opts.scale];
            return;
        }
        auto &ref = reference_planes();
        const bool fixed_plane = opts.scale == options.scale;
        auto &plane = fixed_plane ? ref.canonical_sprite_plane : ref.selected_sprite_plane;
        auto &ready = fixed_plane ? ref.canonical_ready : ref.selected_ready;
        if (!ready) {
            if (plane.empty()) plane.resize(ReferencePlanes::plane_size(opts));
            raster_sprites(captured->sprite_list(), captured->pen_mask, machine.video->sprite_tiles(), plane,
                           {opts});
            ready = true;
        }
        compose_game_scene(captured_scene(mask), {plane, output, opts},
                           serial ? ComposeMode::Serial : ComposeMode::Parallel);
    }

    void materialize_native() {
        if (!native_pending) return;
        // Neither live maps/palette nor the now-latched sprite list describe
        // this frame. Use the immutable capture and retained native plane.
        compose_game_scene(captured_scene(), {sprite_planes[native_plane], pixels}, ComposeMode::Serial);
        std::copy(pixels.begin(), pixels.end(), machine.pixels_.begin());
        native_pending = false;
    }
    void materialize_presentation() {
        if (!presentation_pending) return;
        // Snapshot/save paths must not lazily allocate row workers.
        render_reference(presentation_pixels, options, all_layers, true, true);
        // Canonical snapshots retain the NEXT-frame expanded sprite plane too.
        sprites.raster(machine.video->sprite_tiles(), presentation_sprites, options);
        presentation_pending = false;
    }
    uint64_t composite_frames = 0, composite_mismatches = 0;
    std::array<uint64_t, 9> frames{}, mismatches{};
    // PF2/PF3 alternate physical maps (4/5), compared only for extended_alt_maps games.
    std::array<uint64_t, 2> alt_frames{}, alt_mismatches{};
    // Frames whose render-only presented sprite list (unit splices) existed / filled its capacity.
    uint64_t presented_frames = 0, presented_overflow_frames = 0;
    // Render-only full-resolution alternate-map presentation (game_config::video.full_resolution_alt_maps):
    // the row solver cache and, over rendered frames, visible PF2/PF3 alt-map rows that draw from the
    // main map (remapped) versus the canonical alternate map (fallback).
    FullResolutionAltMaps alt_maps;
    uint64_t alt_rows_remapped = 0, alt_rows_fallback = 0;
    // Layer components in decode order; the constrained helpers below enforce
    // that each one satisfies the scene lifecycle concepts.
    auto sources() { return std::tie(tiles, text, sprites, lines); }
    // Components whose state is not derivable from video RAM, in snapshot order.
    auto stateful() { return std::tie(sprites, lines); }
    auto stateful() const { return std::tie(sprites, lines); }
    template<SceneSource... S> static void reset_all(S &...s) { (s.reset(), ...); }
    template<SceneSource... S> static void decode_all(const VideoRam &vram, S &...s) { (s.decode(vram), ...); }
    template<Snapshotable... S> static size_t state_size_all(const S &...s) { return (size_t(0) + ... + s.state_size()); }
    template<Snapshotable... S> static void save_all(StateWriter &w, const S &...s) { (s.save_state(w), ...); }
    template<Snapshotable... S> static void load_all(StateReader &r, S &...s) { (s.load_state(r), ...); }
    size_t state_size() const {
        // Playfield tiles and text are derived from serialized video RAM, so
        // they are not part of snapshot state; decode() rebuilds them at VBSTART.
        return 1 +
               std::apply([](const auto &...c) { return Impl::state_size_all(c...); }, stateful()) +
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
        std::apply([&](const auto &...c) { Impl::save_all(writer, c...); }, stateful());
        writer.write_span(std::span<const uint16_t, 432 * 256>(sprite_planes[next_plane]));
        writer.write_span(std::span<const uint32_t, geometry::native_pixels>(pixels));
        if (!sync) {
            writer.write_span(std::span<const uint32_t>(presentation_pixels));
            writer.write_span(std::span<const uint16_t>(presentation_sprites));
        }
    }
    void load_state(StateReader &reader, bool sync = false) {
        uint8_t r;
        reader.read(r);
        rendered = r != 0;
        std::apply([&](auto &...c) { Impl::load_all(reader, c...); }, stateful());
        native_pending = false;
        next_plane = native_plane = 0;
        reader.read_span(std::span<uint16_t, 432 * 256>(sprite_planes[next_plane]));
        reader.read_span(std::span<uint32_t, geometry::native_pixels>(pixels));
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
                for (unsigned x = 0; x < geometry::native_width * scale; ++x) {
                    const size_t at = size_t(y) * width + options.border * scale + x;
                    presentation_pixels[at] = machine.pixels_[(y / scale) * geometry::native_width + x / scale];
                    presentation_sprites[at] = sprite_planes[next_plane][(y / scale + geometry::first_line) * 432 + x / scale + 46];
                }
        }
        presentation_pending = false;
        if (captured) {
            captured->fallback = true;
            std::copy(machine.pixels_.begin(), machine.pixels_.end(), captured->native_pixels.begin());
        }
        if (reference) {
            reference->invalidate();
            std::fill(reference->canonical_sprite_plane.begin(), reference->canonical_sprite_plane.end(), 0);
            std::fill(reference->selected_sprite_plane.begin(), reference->selected_sprite_plane.end(), 0);
        }
    }
};

namespace {
// The renderer is compiled against games/<game>/config.toml [video]; a ROM set
// carrying different geometry must never be drawn with it.
Machine &require_matching_video(Machine &machine) {
    if (!game_config::matches(machine.roms.video))
        throw std::runtime_error("Game video geometry (rotation/sprite lag/visible window/extend/alternate maps) "
                                 "differs from the geometry this build was configured for");
    return machine;
}
static_assert(game_config::video.extend, "the game scene renderer decodes 64-column extended playfields only");
static_assert(game_config::video.rotation == 0, "the game scene renderer does not rotate the scanout");
static_assert(game_config::video.sprite_lag == 1, "GameVideo latches sprites with exactly one frame of lag");
} // namespace

GameVideo::GameVideo(Machine &machine, GameVideoMode mode, GameVideoOptions options)
    : impl_(std::make_unique<Impl>(require_matching_video(machine), mode, options)) {
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
    std::apply([](auto &...c) { Impl::reset_all(c...); }, impl_->sources());
    for (auto &plane : impl_->sprite_planes) plane.fill(0);
    impl_->next_plane = impl_->native_plane = 0;
    impl_->native_pending = false;
    std::fill(impl_->presentation_sprites.begin(), impl_->presentation_sprites.end(), 0);
    std::fill(impl_->presentation_pixels.begin(), impl_->presentation_pixels.end(), 0xff000000);
    impl_->presentation_pending = false;
    if (impl_->captured) {
        impl_->captured->fallback = true;
        impl_->captured->native_pixels.fill(0xff000000);
    }
    impl_->frames.fill(0);
    impl_->mismatches.fill(0);
    impl_->presented_frames = impl_->presented_overflow_frames = 0;
    impl_->alt_maps.reset();
    impl_->alt_rows_remapped = impl_->alt_rows_fallback = 0;
    impl_->alt_frames.fill(0);
    impl_->alt_mismatches.fill(0);
    impl_->rendered = false;
    impl_->composite_frames = impl_->composite_mismatches = 0;
    impl_->fallback_count = 0;
    impl_->rendered_frames = impl_->fallback_frames = 0;
}
#ifdef F3RT_VIDEO_WRITE_LOG
void GameVideo::observe_write(uint32_t pc, uint32_t address) {
    observe_game_video_write(pc, address, impl_->machine.frame + 1);
}
#endif

void GameVideo::latch_sprites() {
    auto &state = *impl_;
    const auto assets = state.machine.video->sprite_tiles();
    if (state.captured && state.options.expanded() && state.sprites.reg_trails_ && !state.sprites.trails()) {
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
    if (state.options.expanded() && (!state.captured || state.sprites.trails()))
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
    // Render-only unit presentation (identities/splices) valid for this decode only.
    SpritePresentation presentation;
    const bool have_presentation = m.sprite_units && m.sprite_units->active();
    if (have_presentation) {
        presentation = m.sprite_units->presentation();
        state.sprites.set_presentation(&presentation);
    }
    std::apply([&](auto &...c) { Impl::decode_all(vram, c...); }, state.sources());
    state.sprites.set_presentation(nullptr);
    if (state.sprites.presented_valid()) {
        ++state.presented_frames;
        state.presented_overflow_frames += state.sprites.presented_overflow();
    }
    render();
    if (state.rendered && state.mode == GameVideoMode::Game) {
        if (!state.captured)
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
    if (!state.rendered && state.options.expanded() && !state.captured) {
        // Unsupported geometry is never extrapolated into a made-up border.
        // Preserve the exact oracle image in the center, with black side bars.
        std::fill(state.presentation_pixels.begin(), state.presentation_pixels.end(), 0xff000000);
        const unsigned scale = state.options.scale, width = state.options.width();
        for (unsigned y = 0; y < state.options.height(); ++y)
            for (unsigned x = 0; x < geometry::native_width * scale; ++x)
                state.presentation_pixels[y * width + state.options.border * scale + x] = m.pixels_[(y / scale) * geometry::native_width + x / scale];
    }
    if (state.captured) {
        // Only a supported successor may discard an unobserved composite.
        state.native_pending = false;
        state.capture();
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
    state.lines.cull_empty_rows(state.tiles);
    for (unsigned y = geometry::first_line; y < geometry::end_line; ++y)
        if (state.lines.row(y).bitmap) { state.fallback("text", "bitmap-pivot"); return; }
    if constexpr (game_config::video.full_resolution_alt_maps) {
        state.alt_maps.update(state.tiles, state.machine.video->playfield_tiles());
        const auto counts = state.lines.resolve_full_resolution_alt(state.alt_maps);
        state.alt_rows_remapped += counts.remapped;
        state.alt_rows_fallback += counts.fallback;
    }
    if (state.mode != GameVideoMode::Game || !state.captured) {
        std::array<uint32_t, 8192> colors;
        const auto &palette = state.machine.palette;
        for (unsigned i = 0; i < colors.size(); ++i)
            colors[i] = (uint32_t(palette[i * 4 + 1]) << 16) | (uint32_t(palette[i * 4 + 2]) << 8) | palette[i * 4 + 3];
        const FrameScene scene = state.live_scene(colors);
        compose_game_scene(scene, {state.sprite_planes[state.next_plane], state.pixels});
        if (state.options.expanded() && !state.captured)
            compose_game_scene(scene, {state.presentation_sprites, state.presentation_pixels, state.options});
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
        if (!impl_->captured) impl_->captured = std::make_unique<CapturedFrame>();
        // Native and expanded snapshot observations must not allocate.
        if ((impl_->options.expanded() || impl_->mode == GameVideoMode::Game) && !impl_->reference)
            impl_->reference = std::make_unique<Impl::ReferencePlanes>(impl_->options);
    } else {
        impl_->captured.reset();
        impl_->reference.reset();
        impl_->reference_selected = false;
    }
}
void GameVideo::set_gpu_scale(unsigned scale) {
    if (!impl_->captured) throw std::runtime_error("GPU presentation snapshot is not enabled");
    if (!scale || scale > GameVideoOptions::max_gpu_scale)
        throw std::runtime_error("Game GPU reference scale must be 1..8");
    if (scale == impl_->gpu_scale) return;
    impl_->materialize_native();
    impl_->materialize_presentation();
    auto options = impl_->options;
    options.scale = scale;
    if (impl_->reference && impl_->reference_selected) {
        auto &ref = *impl_->reference;
        ref.selected_sprite_plane.resize(scale == impl_->options.scale ? 0 : Impl::ReferencePlanes::plane_size(options));
        ref.selected_ready = false;
    }
    impl_->gpu_scale = scale;
}
const CapturedFrame &GameVideo::captured_frame() const {
    if (!impl_->captured) throw std::runtime_error("GPU presentation snapshot is not enabled");
    return *impl_->captured;
}
void GameVideo::render_reference(std::span<uint32_t> output, GameVideoOptions options,
                                 unsigned layer_mask, bool serial) const {
    impl_->render_reference(output, options, layer_mask, serial);
}

void GameVideo::compare_layers(uint64_t frame, unsigned layer_mask) {
    if (!layer_mask || (layer_mask & ~all_layers)) throw std::runtime_error("Video layer mask must select PF0..3, SP0..3, text (bits 0..8)");
    auto &m = impl_->machine;
    const auto assets = m.video->playfield_tiles();
    if (layer_mask & layer_bit(LayerId::Text)) m.video->prepare_text_inspection(m.graphics);
    if (layer_mask == all_layers) impl_->lines.compare_rows(*m.video, frame);
    for (unsigned layer = 0; layer < layer_count; ++layer) {
        const LayerId id{uint8_t(layer)};
        if (!(layer_mask & layer_bit(id))) continue;
        // Every layer is decoded from video RAM at VBSTART; nothing is
        // unsupported by producer identity.
        uint64_t count = 0;
        int first_x = -1, first_y = -1;
        ScenePixel first_game{}, first_oracle{};
        const int width = layer_info[layer].width;
        const int height = layer_info[layer].height;
        const LayerKind layer_kind = kind(id);
        const bool indexed = layer_kind != LayerKind::Sprite;
        for (int y = 0; y < height; ++y) {
            const auto oracle = layer_kind == LayerKind::Playfield ? m.video->inspect_playfield_line(sub_index(id), y, m.graphics) :
                layer_kind == LayerKind::Text ? m.video->inspect_text_line(y, m.graphics) : VideoLine{};
            for (int x = 0; x < width; ++x) {
                ScenePixel game{}, expected{};
                if (layer_kind == LayerKind::Playfield) game = impl_->tiles.playfield_pixel(sub_index(id), x, y, m.video->flipscreen(), assets);
                else if (layer_kind == LayerKind::Text) game = impl_->text.pixel(x, y, m.video->flipscreen());
                if (indexed) expected = {oracle.palette[x], oracle.flags[x]};
                else {
                    const unsigned offset = (y + geometry::first_line) * 432 + x + 46;
                    const uint16_t actual = impl_->sprite_planes[impl_->next_plane][offset];
                    const uint16_t reference = m.video->sprite_plane()[offset];
                    game = {actual, uint8_t(actual && sprite_layer(actual) == id ? 0x10 : 0)};
                    expected = {reference, uint8_t(reference && sprite_layer(reference) == id ? 0x10 : 0)};
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
            error << "Game " << layer_info[layer].name << " frame " << frame << ": " << count
                  << " indexed pixel mismatches; first (" << first_x << ',' << first_y << ") game=0x"
                  << std::hex << first_game.palette << '/' << unsigned(first_game.flags)
                  << " oracle=0x" << first_oracle.palette << '/' << unsigned(first_oracle.flags);
            if (layer_kind == LayerKind::Sprite) {
                for (const auto &sprite : impl_->sprites.sprites()) {
                    const int dx = (sprite.x >> 8) - (first_x + 46);
                    const int dy = (sprite.y >> 8) - (first_y + int(geometry::first_line));
                    if (dx < -32 || dx > 32 || dy < -32 || dy > 32) continue;
                    error << "\n nearby tile=0x" << std::hex << sprite.tile << " palette=0x" << unsigned(sprite.palette)
                          << std::dec << " xy8=(" << sprite.x << ',' << sprite.y << ") scale=("
                          << sprite.scale_x << ',' << sprite.scale_y << ") flip=(" << sprite.flip_x << ',' << sprite.flip_y << ')';
                }
            }
            throw std::runtime_error(error.str());
        }
    }
    if constexpr (game_config::video.extended_alt_maps) {
        for (unsigned alt = 0; alt < 2; ++alt) {
            const LayerId id = playfield(2 + alt);
            if (!(layer_mask & layer_bit(id))) continue;
            uint64_t count = 0;
            int first_x = -1, first_y = -1;
            ScenePixel first_game{}, first_oracle{};
            for (int y = 0; y < layer_info[unsigned(id)].height; ++y) {
                const auto oracle = m.video->inspect_playfield_line(4 + alt, y, m.graphics);
                for (int x = 0; x < layer_info[unsigned(id)].width; ++x) {
                    const ScenePixel game = impl_->tiles.playfield_pixel(2 + alt, x, y, m.video->flipscreen(), assets, true);
                    const ScenePixel expected{oracle.palette[x], oracle.flags[x]};
                    if (!differs(game, expected)) continue;
                    if (!count) { first_x = x; first_y = y; first_game = game; first_oracle = expected; }
                    ++count;
                }
            }
            ++impl_->alt_frames[alt];
            impl_->alt_mismatches[alt] += count;
            if (count) {
                std::ostringstream error;
                error << "Game " << layer_info[unsigned(id)].name << " alternate map frame " << frame << ": " << count
                      << " indexed pixel mismatches; first (" << first_x << ',' << first_y << ") game=0x"
                      << std::hex << first_game.palette << '/' << unsigned(first_game.flags)
                      << " oracle=0x" << first_oracle.palette << '/' << unsigned(first_oracle.flags);
                throw std::runtime_error(error.str());
            }
        }
    }
    if (layer_mask == all_layers) compare_composite(frame);
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
              << first % geometry::native_width << ',' << first / geometry::native_width << ") game=0x" << std::hex << impl_->pixels[first]
              << " oracle=0x" << oracle[first];
        throw std::runtime_error(error.str());
    }
}

void GameVideo::report(std::ostream &output) const {
    for (unsigned layer = 0; layer < layer_count; ++layer) {
        if (!impl_->frames[layer]) continue;
        const unsigned pixels = layer_info[layer].width * layer_info[layer].height;
        output << "VIDEO layer=" << layer_info[layer].name << " domain="
               << layer_info[layer].width << 'x' << layer_info[layer].height << '-' << layer_info[layer].domain
               << " sampled_frames=" << impl_->frames[layer]
               << " compared_pixels=" << impl_->frames[layer] * pixels
               << " pixel_mismatches=" << impl_->mismatches[layer] << '\n';
    }
    if constexpr (game_config::video.extended_alt_maps) {
        for (unsigned alt = 0; alt < 2; ++alt) {
            if (!impl_->alt_frames[alt]) continue;
            const auto &info = layer_info[unsigned(LayerId::Pf2) + alt];
            output << "VIDEO layer=" << info.name << "-alt domain=" << info.width << 'x' << info.height << '-'
                   << info.domain << " sampled_frames=" << impl_->alt_frames[alt]
                   << " compared_pixels=" << impl_->alt_frames[alt] * info.width * info.height
                   << " pixel_mismatches=" << impl_->alt_mismatches[alt] << '\n';
        }
    }
    if (impl_->composite_frames)
        output << "VIDEO layer=composite domain=" << geometry::native_width << 'x' << geometry::height
               << "-RGB sampled_frames=" << impl_->composite_frames
               << " compared_pixels=" << impl_->composite_frames * geometry::native_pixels
               << " pixel_mismatches=" << impl_->composite_mismatches << '\n';
    if (impl_->presented_frames || game_config::video.full_resolution_alt_maps) {
        output << "VIDEO presented_sprite_frames=" << impl_->presented_frames
               << " presented_sprite_overflow_frames=" << impl_->presented_overflow_frames;
        if constexpr (game_config::video.full_resolution_alt_maps)
            output << " presented_alt_rows_remapped=" << impl_->alt_rows_remapped
                   << " presented_alt_rows_fallback=" << impl_->alt_rows_fallback
                   << " presented_alt_rows_solved=" << impl_->alt_maps.solved_rows();
        output << '\n';
    }
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
    if (impl_->captured) {
        impl_->captured->fallback = true;
        std::copy(impl_->machine.pixels_.begin(), impl_->machine.pixels_.end(), impl_->captured->native_pixels.begin());
        if (impl_->reference) impl_->reference->invalidate();
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
