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
#include "debug/discovery.hpp"
#include "renderer/game/captured_frame.hpp"
#include "state_io.hpp"
#include "sprites/units.hpp"
#include <algorithm>
#include <array>
#include <iostream>
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
} // namespace
struct GameVideo::Impl {
    Impl(Machine &value, GameVideoMode selected, GameVideoOptions presentation_options)
        : machine(value), mode(selected), options(presentation_options),
          gpu_scale(presentation_options.scale) {}
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
        if (machine.discovery) machine.discovery->video_fallback(component, reason, frame);
        ++fallback_frames;
        for (unsigned i = 0; i < fallback_count; ++i) {
            auto &entry = fallbacks[i];
            if (entry.reason != reason) continue;
            ++entry.frames;
            entry.last = frame;
            return;
        }
        if (fallback_count == fallbacks.size())
            throw std::runtime_error("Game video fallback reason capacity exceeded");
        fallbacks[fallback_count++] = {reason, 1, frame, frame};
    }
    GameTiles tiles;
    GameText text;
    GameSprites sprites;
    GameLines lines;
    // Only sprite_planes[next_plane] is canonical state. The other plane
    // retains the pending frame while latch_sprites prepares its successor.
    std::array<std::array<uint16_t, size_t(432) * 256>, 2> sprite_planes{};
    unsigned next_plane = 0, native_plane = 0;
    bool native_pending = false;
    std::array<uint32_t, geometry::NATIVE_PIXELS> pixels{};
    bool rendered = false;
    // Present exactly when GPU presentation is enabled. Overwritten once per
    // supported or fallback frame by capture(); the CPU compositor and the GPU
    // encoder both read it.
    CapturedFrame *captured = nullptr;
    std::unique_ptr<CapturedFrame> captured_storage;
    // Optional caller-owned destination for the next capture or load. `captured` always names the newest frame
    // and is never rewritten in place once a target is installed, so a frame handed to another thread stays immutable.
    CapturedFrame *capture_target = nullptr;
    CapturedFrame &capture_dest() { return capture_target ? *capture_target : *captured; }
    // Reference-scale sprite planes, rastered lazily from `captured`; stale after every recapture.
    struct ReferencePlanes {
        std::vector<uint16_t> canonical_sprite_plane;
        std::vector<uint16_t> selected_sprite_plane;
        bool canonical_ready = false, selected_ready = false;
        explicit ReferencePlanes(GameVideoOptions options)
            : canonical_sprite_plane(options.expanded() ? plane_size(options) : 0) {}
        static size_t plane_size(GameVideoOptions options) {
            return options.expanded() ? size_t(options.width()) * options.height()
                                      : size_t(432) * 256;
        }
        void invalidate() { canonical_ready = selected_ready = false; }
    };
    std::unique_ptr<ReferencePlanes> reference;
    bool reference_selected = false;
    bool presentation_pending = false;

    void capture() {
        auto &frame = capture_dest();
        captured = &frame;
        frame.fallback = !rendered;
        if (reference) reference->invalidate();
        if (frame.fallback) {
            std::copy(machine.m_pixels.begin(), machine.m_pixels.end(),
                      frame.native_pixels.begin());
            return;
        }
        frame.tiles = tiles;
        frame.text = text;
        std::copy(lines.rows().begin(), lines.rows().end(), frame.rows.begin());
        const auto current = sprites.presented_sprites();
        frame.sprite_count = unsigned(current.size());
        std::copy(current.begin(), current.end(), frame.sprites.begin());
        frame.shadow_count = unsigned(std::count_if(current.begin(), current.end(),
                                                    [](const SceneSprite &s) { return s.shadow; }));
        frame.pen_mask = sprites.pen_mask();
        frame.flipped = sprites.flipped();
        const auto &palette = machine.palette;
        for (unsigned i = 0; i < frame.colors.size(); ++i)
            frame.colors[i] = (uint32_t(palette[i * 4 + 1]) << 16) |
                              (uint32_t(palette[i * 4 + 2]) << 8) | palette[i * 4 + 3];
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
        return {tiles,  text,       lines.rows(),      machine.video->playfield_tiles(),
                colors, ALL_LAYERS, sprites.flipped(), true};
    }
    FrameScene captured_scene(uint32_t mask = ALL_LAYERS) const {
        return captured->scene(machine.video->playfield_tiles(), mask);
    }

    void render_reference(std::span<uint32_t> output, GameVideoOptions opts, unsigned mask,
                          bool serial, bool canonical = false) {
        if (!captured || opts.scale != (canonical ? options.scale : gpu_scale) ||
            opts.border != options.border || output.size() < size_t(opts.width()) * opts.height() ||
            (mask & ~ALL_LAYERS))
            throw std::runtime_error("Invalid GPU CPU-reference request");
        if (!canonical && !reference_selected) {
            reference_selected = true;
            if (reference && opts.scale != options.scale)
                reference->selected_sprite_plane.resize(ReferencePlanes::plane_size(opts));
        }
        if (captured->fallback) {
            const unsigned left = opts.border * opts.scale;
            for (unsigned y = 0; y < opts.height(); ++y)
                for (unsigned x = 0; x < opts.width(); ++x)
                    output[size_t(y) * opts.width() + x] =
                        x < left || x >= left + geometry::NATIVE_WIDTH * opts.scale
                            ? 0xff000000u
                            : captured->native_pixels[(y / opts.scale) * geometry::NATIVE_WIDTH +
                                                      (x - left) / opts.scale];
            return;
        }
        auto &ref = reference_planes();
        const bool fixed_plane = opts.scale == options.scale;
        auto &plane = fixed_plane ? ref.canonical_sprite_plane : ref.selected_sprite_plane;
        auto &ready = fixed_plane ? ref.canonical_ready : ref.selected_ready;
        if (!ready) {
            if (plane.empty()) plane.resize(ReferencePlanes::plane_size(opts));
            raster_sprites(captured->sprite_list(), captured->pen_mask,
                           machine.video->sprite_tiles(), plane, {opts});
            ready = true;
        }
        compose_game_scene(captured_scene(mask), {plane, output, opts},
                           serial ? ComposeMode::Serial : ComposeMode::Parallel);
    }

    void materialize_native() {
        if (!native_pending) return;
        // Neither live maps/palette nor the now-latched sprite list describe
        // this frame. Use the immutable capture and retained native plane.
        compose_game_scene(captured_scene(), {sprite_planes[native_plane], pixels},
                           ComposeMode::Serial);
        std::copy(pixels.begin(), pixels.end(), machine.m_pixels.begin());
        native_pending = false;
    }
    void materialize_presentation() {
        if (!presentation_pending) return;
        // Snapshot/save paths must not lazily allocate row workers.
        render_reference(presentation_pixels, options, ALL_LAYERS, true, true);
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
    template <SceneSource... S> static void reset_all(S &...s) { (s.reset(), ...); }
    template <SceneSource... S> static void decode_all(const VideoRam &vram, S &...s) {
        (s.decode(vram), ...);
    }
    template <Snapshotable... S> static size_t state_size_all(const S &...s) {
        return (size_t(0) + ... + s.state_size());
    }
    template <Snapshotable... S> static void save_all(StateWriter &w, const S &...s) {
        (s.save_state(w), ...);
    }
    template <Snapshotable... S> static void load_all(StateReader &r, S &...s) {
        (s.load_state(r), ...);
    }
    // Playfield tiles and text are derived from serialized video RAM, so
    // their caches are not serialized.
    size_t state_size() const {
        return 1 +
               std::apply([](const auto &...c) { return Impl::state_size_all(c...); }, stateful()) +
               sizeof(uint16_t) * 432 * 256 + sizeof(uint32_t) * pixels.size() +
               sizeof(uint32_t) * presentation_pixels.size() +
               sizeof(uint16_t) * presentation_sprites.size();
    }
    void save_state(StateWriter &writer) const {
        uint8_t r = rendered ? 1 : 0;
        writer.write(r);
        std::apply([&](const auto &...c) { Impl::save_all(writer, c...); }, stateful());
        writer.write_span(std::span<const uint16_t, size_t(432) * 256>(sprite_planes[next_plane]));
        writer.write_span(std::span<const uint32_t, geometry::NATIVE_PIXELS>(pixels));
        writer.write_span(std::span<const uint32_t>(presentation_pixels));
        writer.write_span(std::span<const uint16_t>(presentation_sprites));
    }
    void load_state(StateReader &reader) {
        uint8_t r;
        reader.read(r);
        rendered = r != 0;
        std::apply([&](auto &...c) { Impl::load_all(reader, c...); }, stateful());
        native_pending = false;
        next_plane = native_plane = 0;
        reader.read_span(std::span<uint16_t, size_t(432) * 256>(sprite_planes[next_plane]));
        reader.read_span(std::span<uint32_t, geometry::NATIVE_PIXELS>(pixels));
        reader.read_span(std::span<uint32_t>(presentation_pixels));
        reader.read_span(std::span<uint16_t>(presentation_sprites));
    }
    void reseed_presentation() {
        // Imported native retention is authoritative. Do not reconstruct a
        // previous frame from the now-latched NEXT-frame sprite list.
        std::fill(presentation_sprites.begin(), presentation_sprites.end(), 0);
        std::fill(presentation_pixels.begin(), presentation_pixels.end(), 0xff000000);
        if (options.expanded()) {
            const unsigned scale = options.scale, width = options.width();
            for (unsigned y = 0; y < options.height(); ++y)
                for (unsigned x = 0; x < geometry::NATIVE_WIDTH * scale; ++x) {
                    const size_t at = size_t(y) * width + size_t(options.border) * scale + x;
                    presentation_pixels[at] =
                        machine.m_pixels[(y / scale) * geometry::NATIVE_WIDTH + x / scale];
                    presentation_sprites[at] =
                        sprite_planes[next_plane]
                                     [(y / scale + geometry::FIRST_LINE) * 432 + x / scale + 46];
                }
        }
        presentation_pending = false;
        if (captured) {
            captured->fallback = true;
            std::copy(machine.m_pixels.begin(), machine.m_pixels.end(),
                      captured->native_pixels.begin());
        }
        if (reference) {
            reference->invalidate();
            std::fill(reference->canonical_sprite_plane.begin(),
                      reference->canonical_sprite_plane.end(), 0);
            std::fill(reference->selected_sprite_plane.begin(),
                      reference->selected_sprite_plane.end(), 0);
        }
    }
};

namespace {
// The renderer is compiled against games/<game>/config.toml [video]; a ROM set
// carrying different geometry must never be drawn with it.
Machine &require_matching_video(Machine &machine) {
    if (!game_config::matches(machine.roms.video))
        throw std::runtime_error(
            "Game video geometry (rotation/sprite lag/visible window/extend/alternate maps) "
            "differs from the geometry this build was configured for");
    return machine;
}
#if F3RT_GAME_VIDEO
static_assert(game_config::video.rotation == 0,
              "GameVideo does not rotate the scanout (video.rotation == 0)");
static_assert(game_config::video.sprite_lag == 1,
              "GameVideo latches sprites with exactly one frame of lag (video.sprite_lag == 1)");
#endif
} // namespace

GameVideo::GameVideo(Machine &machine, GameVideoMode mode, GameVideoOptions options)
    : m_impl(std::make_unique<Impl>(require_matching_video(machine), mode, options)) {
    if (!options.scale || options.scale > GameVideoOptions::MAX_SCALE ||
        options.border > GameVideoOptions::MAX_BORDER)
        throw std::runtime_error("Game presentation scale must be 1..4 and border 0..160");
    if (options.expanded()) {
        const size_t size = size_t(options.width()) * options.height();
        m_impl->presentation_pixels.resize(size);
        m_impl->presentation_sprites.resize(size);
    }
    machine.video->enable_scene_inspection(mode != GameVideoMode::Game);
    reset();
}
GameVideo::~GameVideo() = default;
void GameVideo::reset() {
    m_impl->materialize_native();
    m_impl->materialize_presentation();
    std::apply([](auto &...c) { Impl::reset_all(c...); }, m_impl->sources());
    for (auto &plane : m_impl->sprite_planes) plane.fill(0);
    m_impl->next_plane = m_impl->native_plane = 0;
    m_impl->native_pending = false;
    std::fill(m_impl->presentation_sprites.begin(), m_impl->presentation_sprites.end(), 0);
    std::fill(m_impl->presentation_pixels.begin(), m_impl->presentation_pixels.end(), 0xff000000);
    m_impl->presentation_pending = false;
    if (m_impl->captured) {
        auto &frame = m_impl->capture_dest();
        m_impl->captured = &frame;
        frame.fallback = true;
        frame.native_pixels.fill(0xff000000);
    }
    m_impl->frames.fill(0);
    m_impl->mismatches.fill(0);
    m_impl->presented_frames = m_impl->presented_overflow_frames = 0;
    m_impl->alt_maps.reset();
    m_impl->alt_rows_remapped = m_impl->alt_rows_fallback = 0;
    m_impl->alt_frames.fill(0);
    m_impl->alt_mismatches.fill(0);
    m_impl->rendered = false;
    m_impl->composite_frames = m_impl->composite_mismatches = 0;
    m_impl->fallback_count = 0;
    m_impl->rendered_frames = m_impl->fallback_frames = 0;
}

void GameVideo::latch_sprites() {
    auto &state = *m_impl;
    const auto assets = state.machine.video->sprite_tiles();
    if (state.captured && state.options.expanded() && state.sprites.m_reg_trails &&
        !state.sprites.trails()) {
        // Seed retention with the preceding expanded plane before latching the
        // first trail list. Reconstructing only the final list loses history.
        state.sprites.raster(assets, state.presentation_sprites, state.options);
    }
    // A trail latch updates the prior plane in place. Finish its pending
    // composite first rather than copying a full plane just to preserve it.
    if (state.native_pending && state.sprites.m_reg_trails) state.materialize_native();
    state.sprites.latch();
    if (state.native_pending) state.next_plane ^= 1;
    state.sprites.raster(assets, state.sprite_planes[state.next_plane]);
    if (state.options.expanded() && (!state.captured || state.sprites.trails()))
        state.sprites.raster(assets, state.presentation_sprites, state.options);
}

void GameVideo::render_frame() {
    auto &state = *m_impl;
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
            std::copy(state.pixels.begin(), state.pixels.end(), m.m_pixels.begin());
        // Keep only the oracle's sprite lag current, so an unsupported future
        // frame can use its exact visible result without rerunning the CPU.
        // Do not remove: without it, a fallback frame's sprite lag is wrong.
        m.video->vblank(m.graphics);
    } else {
        // The game composite is retained across oracle fallback frames, even
        // though Machine scanout now comes from the oracle.
        state.materialize_native();
        m.video->render_frame(m.palette, m.graphics, m.control, m.m_pixels);
        if (state.rendered && state.mode == GameVideoMode::Compare) compare_composite(m.frame + 1);
    }
    if (!state.rendered && state.options.expanded() && !state.captured) {
        // Unsupported geometry is never extrapolated into a made-up border.
        // Preserve the exact oracle image in the center, with black side bars.
        std::fill(state.presentation_pixels.begin(), state.presentation_pixels.end(), 0xff000000);
        const unsigned scale = state.options.scale, width = state.options.width();
        for (unsigned y = 0; y < state.options.height(); ++y)
            for (unsigned x = 0; x < geometry::NATIVE_WIDTH * scale; ++x)
                state.presentation_pixels[y * width + state.options.border * scale + x] =
                    m.m_pixels[(y / scale) * geometry::NATIVE_WIDTH + x / scale];
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
    auto &state = *m_impl;
    state.rendered = false;
    // Every layer decodes from video RAM, so only these features the scene
    // renderer does not draw force the FDP oracle fallback.
    if (state.sprites.flipped()) {
        state.fallback("sprites", "flipped-screen");
        return;
    }
    if (state.sprites.trails()) {
        state.fallback("sprites", "sprite-trails");
        return;
    }
    state.lines.prepare(state.sprites.flipped());
    state.lines.cull_empty_rows(state.tiles);
    for (unsigned y = geometry::FIRST_LINE; y < geometry::END_LINE; ++y)
        if (state.lines.row(y).bitmap) {
            state.fallback("text", "bitmap-pivot");
            return;
        }
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
            colors[i] = (uint32_t(palette[i * 4 + 1]) << 16) | (uint32_t(palette[i * 4 + 2]) << 8) |
                        palette[i * 4 + 3];
        const FrameScene scene = state.live_scene(colors);
        compose_game_scene(scene, {state.sprite_planes[state.next_plane], state.pixels});
        if (state.options.expanded() && !state.captured)
            compose_game_scene(
                scene, {state.presentation_sprites, state.presentation_pixels, state.options});
    }
    state.rendered = true;
    ++state.rendered_frames;
}

void GameVideo::materialize_native() const { m_impl->materialize_native(); }
std::span<const uint32_t> GameVideo::presentation() const {
    m_impl->materialize_presentation();
    return m_impl->options.expanded() ? std::span<const uint32_t>(m_impl->presentation_pixels)
                                      : std::span<const uint32_t>(m_impl->machine.native_pixels());
}

void GameVideo::enable_gpu_presentation(bool enabled) {
    m_impl->materialize_native();
    m_impl->materialize_presentation();
    if (enabled) {
        if (!m_impl->captured)
            m_impl->captured = (m_impl->captured_storage = std::make_unique<CapturedFrame>()).get();
        // Native and expanded snapshot observations must not allocate.
        if ((m_impl->options.expanded() || m_impl->mode == GameVideoMode::Game) &&
            !m_impl->reference)
            m_impl->reference = std::make_unique<Impl::ReferencePlanes>(m_impl->options);
    } else {
        m_impl->captured = m_impl->capture_target = nullptr;
        m_impl->captured_storage.reset();
        m_impl->reference.reset();
        m_impl->reference_selected = false;
    }
}
void GameVideo::set_capture_target(CapturedFrame *target) {
    if (target && !m_impl->captured)
        throw std::runtime_error("GPU presentation snapshot is not enabled");
    m_impl->capture_target = target;
}
void GameVideo::set_gpu_scale(unsigned scale) {
    if (!m_impl->captured) throw std::runtime_error("GPU presentation snapshot is not enabled");
    if (!scale || scale > GameVideoOptions::MAX_GPU_SCALE)
        throw std::runtime_error("Game GPU reference scale must be 1..8");
    if (scale == m_impl->gpu_scale) return;
    m_impl->materialize_native();
    m_impl->materialize_presentation();
    auto options = m_impl->options;
    options.scale = scale;
    if (m_impl->reference && m_impl->reference_selected) {
        auto &ref = *m_impl->reference;
        ref.selected_sprite_plane.resize(
            scale == m_impl->options.scale ? 0 : Impl::ReferencePlanes::plane_size(options));
        ref.selected_ready = false;
    }
    m_impl->gpu_scale = scale;
}
const CapturedFrame &GameVideo::captured_frame() const {
    if (!m_impl->captured) throw std::runtime_error("GPU presentation snapshot is not enabled");
    return *m_impl->captured;
}
void GameVideo::render_reference(std::span<uint32_t> output, GameVideoOptions options,
                                 unsigned layer_mask, bool serial) const {
    m_impl->render_reference(output, options, layer_mask, serial);
}

void GameVideo::compare_layers(uint64_t frame, unsigned layer_mask) {
    if (!layer_mask || (layer_mask & ~ALL_LAYERS))
        throw std::runtime_error("Video layer mask must select PF0..3, SP0..3, text (bits 0..8)");
    auto &m = m_impl->machine;
    const auto assets = m.video->playfield_tiles();
    if (layer_mask & layer_bit(LayerId::Text)) m.video->prepare_text_inspection(m.graphics);
    if (layer_mask == ALL_LAYERS) m_impl->lines.compare_rows(*m.video, frame);
    for (unsigned layer = 0; layer < LAYER_COUNT; ++layer) {
        const LayerId id{uint8_t(layer)};
        if (!(layer_mask & layer_bit(id))) continue;
        // Every layer is decoded from video RAM at VBSTART; nothing is
        // unsupported by producer identity.
        uint64_t count = 0;
        int first_x = -1, first_y = -1;
        ScenePixel first_game{}, first_oracle{};
        const int width = LAYER_INFO[layer].width;
        const int height = LAYER_INFO[layer].height;
        const LayerKind layer_kind = kind(id);
        const bool indexed = layer_kind != LayerKind::Sprite;
        for (int y = 0; y < height; ++y) {
            const auto oracle = layer_kind == LayerKind::Playfield
                                    ? m.video->inspect_playfield_line(sub_index(id), y, m.graphics)
                                : layer_kind == LayerKind::Text
                                    ? m.video->inspect_text_line(y, m.graphics)
                                    : VideoLine{};
            for (int x = 0; x < width; ++x) {
                ScenePixel game{}, expected{};
                if (layer_kind == LayerKind::Playfield)
                    game = m_impl->tiles.playfield_pixel(sub_index(id), x, y, m.video->flipscreen(),
                                                         assets);
                else if (layer_kind == LayerKind::Text)
                    game = m_impl->text.pixel(x, y, m.video->flipscreen());
                if (indexed) expected = {oracle.palette[x], oracle.flags[x]};
                else {
                    const unsigned offset = (y + geometry::FIRST_LINE) * 432 + x + 46;
                    const uint16_t actual = m_impl->sprite_planes[m_impl->next_plane][offset];
                    const uint16_t reference = m.video->sprite_plane()[offset];
                    game = {actual, uint8_t(actual && sprite_layer(actual) == id ? 0x10 : 0)};
                    expected = {reference,
                                uint8_t(reference && sprite_layer(reference) == id ? 0x10 : 0)};
                }
                if (differs(game, expected)) {
                    if (!count) {
                        first_x = x;
                        first_y = y;
                        first_game = game;
                        first_oracle = expected;
                    }
                    ++count;
                }
            }
        }
        ++m_impl->frames[layer];
        m_impl->mismatches[layer] += count;
        if (count) {
            std::ostringstream error;
            error << "Game " << LAYER_INFO[layer].name << " frame " << frame << ": " << count
                  << " indexed pixel mismatches; first (" << first_x << ',' << first_y
                  << ") game=0x" << std::hex << first_game.palette << '/'
                  << unsigned(first_game.flags) << " oracle=0x" << first_oracle.palette << '/'
                  << unsigned(first_oracle.flags);
            if (layer_kind == LayerKind::Sprite) {
                for (const auto &sprite : m_impl->sprites.sprites()) {
                    const int dx = (sprite.x >> 8) - (first_x + 46);
                    const int dy = (sprite.y >> 8) - (first_y + int(geometry::FIRST_LINE));
                    if (dx < -32 || dx > 32 || dy < -32 || dy > 32) continue;
                    error << "\n nearby tile=0x" << std::hex << sprite.tile << " palette=0x"
                          << unsigned(sprite.palette) << std::dec << " xy8=(" << sprite.x << ','
                          << sprite.y << ") scale=(" << sprite.scale_x << ',' << sprite.scale_y
                          << ") flip=(" << sprite.flip_x << ',' << sprite.flip_y << ')';
                }
            }
            throw std::runtime_error(error.str());
        }
    }
    if constexpr (!game_config::video.extend || game_config::video.extended_alt_maps) {
        for (unsigned alt = 0; alt < 2; ++alt) {
            const LayerId id = playfield(2 + alt);
            if (!(layer_mask & layer_bit(id))) continue;
            uint64_t count = 0;
            int first_x = -1, first_y = -1;
            ScenePixel first_game{}, first_oracle{};
            for (int y = 0; y < LAYER_INFO[unsigned(id)].height; ++y) {
                const auto oracle = m.video->inspect_playfield_line(4 + alt, y, m.graphics);
                for (int x = 0; x < LAYER_INFO[unsigned(id)].width; ++x) {
                    const ScenePixel game = m_impl->tiles.playfield_pixel(
                        2 + alt, x, y, m.video->flipscreen(), assets, true);
                    const ScenePixel expected{oracle.palette[x], oracle.flags[x]};
                    if (!differs(game, expected)) continue;
                    if (!count) {
                        first_x = x;
                        first_y = y;
                        first_game = game;
                        first_oracle = expected;
                    }
                    ++count;
                }
            }
            ++m_impl->alt_frames[alt];
            m_impl->alt_mismatches[alt] += count;
            if (count) {
                std::ostringstream error;
                error << "Game " << LAYER_INFO[unsigned(id)].name << " alternate map frame "
                      << frame << ": " << count << " indexed pixel mismatches; first (" << first_x
                      << ',' << first_y << ") game=0x" << std::hex << first_game.palette << '/'
                      << unsigned(first_game.flags) << " oracle=0x" << first_oracle.palette << '/'
                      << unsigned(first_oracle.flags);
                throw std::runtime_error(error.str());
            }
        }
    }
    if (layer_mask == ALL_LAYERS) compare_composite(frame);
}

void GameVideo::compare_composite(uint64_t frame) {
    m_impl->materialize_native();
    if (!m_impl->rendered) {
        std::ostringstream error;
        error << "Game composite fallback at frame " << frame;
        throw std::runtime_error(error.str());
    }
    uint64_t count = 0;
    unsigned first = 0;
    const auto &oracle = m_impl->machine.m_pixels;
    for (unsigned i = 0; i < m_impl->pixels.size(); ++i) {
        if (m_impl->pixels[i] == oracle[i]) continue;
        if (!count) first = i;
        ++count;
    }
    ++m_impl->composite_frames;
    m_impl->composite_mismatches += count;
    if (count) {
        std::ostringstream error;
        error << "Game composite frame " << frame << ": " << count
              << " RGB pixel mismatches; first (" << first % geometry::NATIVE_WIDTH << ','
              << first / geometry::NATIVE_WIDTH << ") game=0x" << std::hex << m_impl->pixels[first]
              << " oracle=0x" << oracle[first];
        throw std::runtime_error(error.str());
    }
}

void GameVideo::report(std::ostream &output) const {
    for (unsigned layer = 0; layer < LAYER_COUNT; ++layer) {
        if (!m_impl->frames[layer]) continue;
        const unsigned pixels = LAYER_INFO[layer].width * LAYER_INFO[layer].height;
        output << "VIDEO layer=" << LAYER_INFO[layer].name << " domain=" << LAYER_INFO[layer].width
               << 'x' << LAYER_INFO[layer].height << '-' << LAYER_INFO[layer].domain
               << " sampled_frames=" << m_impl->frames[layer]
               << " compared_pixels=" << m_impl->frames[layer] * pixels
               << " pixel_mismatches=" << m_impl->mismatches[layer] << '\n';
    }
    if constexpr (!game_config::video.extend || game_config::video.extended_alt_maps) {
        for (unsigned alt = 0; alt < 2; ++alt) {
            if (!m_impl->alt_frames[alt]) continue;
            const auto &info = LAYER_INFO[unsigned(LayerId::Pf2) + alt];
            output << "VIDEO layer=" << info.name << "-alt domain=" << info.width << 'x'
                   << info.height << '-' << info.domain
                   << " sampled_frames=" << m_impl->alt_frames[alt]
                   << " compared_pixels=" << m_impl->alt_frames[alt] * info.width * info.height
                   << " pixel_mismatches=" << m_impl->alt_mismatches[alt] << '\n';
        }
    }
    if (m_impl->composite_frames)
        output << "VIDEO layer=composite domain=" << geometry::NATIVE_WIDTH << 'x'
               << geometry::HEIGHT << "-RGB sampled_frames=" << m_impl->composite_frames
               << " compared_pixels=" << m_impl->composite_frames * geometry::NATIVE_PIXELS
               << " pixel_mismatches=" << m_impl->composite_mismatches << '\n';
    if (m_impl->presented_frames || game_config::video.full_resolution_alt_maps) {
        output << "VIDEO presented_sprite_frames=" << m_impl->presented_frames
               << " presented_sprite_overflow_frames=" << m_impl->presented_overflow_frames;
        if constexpr (game_config::video.full_resolution_alt_maps)
            output << " presented_alt_rows_remapped=" << m_impl->alt_rows_remapped
                   << " presented_alt_rows_fallback=" << m_impl->alt_rows_fallback
                   << " presented_alt_rows_solved=" << m_impl->alt_maps.solved_rows();
        output << '\n';
    }
    output << "VIDEO game_frames=" << m_impl->rendered_frames
           << " oracle_fallback_frames=" << m_impl->fallback_frames << '\n';
    for (unsigned i = 0; i < m_impl->fallback_count; ++i) {
        const auto &entry = m_impl->fallbacks[i];
        output << "VIDEO fallback=" << entry.reason << " frames=" << entry.frames
               << " first=" << entry.first << " last=" << entry.last << '\n';
    }
}
size_t GameVideo::state_size() const { return m_impl->state_size(); }
void GameVideo::save_state(std::span<uint8_t> dst) const {
    static_assert(Snapshotable<Impl>);
    if (dst.size() != state_size()) {
        throw std::invalid_argument("GameVideo::save_state size mismatch");
    }
    m_impl->materialize_native();
    m_impl->materialize_presentation();
    StateWriter writer(dst);
    m_impl->save_state(writer);
    if (writer.remaining() != 0) {
        throw std::logic_error("GameVideo::save_state remaining unwritten bytes");
    }
}
void GameVideo::load_state(std::span<const uint8_t> src) {
    if (src.size() != state_size()) {
        throw std::invalid_argument("GameVideo::load_state size mismatch");
    }
    m_impl->materialize_native();
    m_impl->materialize_presentation();
    StateReader reader(src);
    m_impl->load_state(reader);
    m_impl->presentation_pending = false;
    if (m_impl->captured) {
        auto &frame = m_impl->capture_dest();
        m_impl->captured = &frame;
        frame.fallback = true;
        std::copy(m_impl->machine.m_pixels.begin(), m_impl->machine.m_pixels.end(),
                  frame.native_pixels.begin());
        if (m_impl->reference) m_impl->reference->invalidate();
    }
    if (reader.remaining() != 0) {
        throw std::logic_error("GameVideo::load_state remaining unread bytes");
    }
}

static_assert(SnapshotRoot<GameVideo>);
} // namespace f3rt
