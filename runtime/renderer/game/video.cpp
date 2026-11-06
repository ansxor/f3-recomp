#include "f3rt/game_video.hpp"
#include "f3rt/machine.hpp"
#include "f3rt/video.hpp"
#include "renderer/game/tiles.hpp"
#include "renderer/game/text.hpp"
#include "renderer/game/sprites.hpp"
#include "renderer/game/lines.hpp"
#include "renderer/game/clip.hpp"
#include "renderer/game/video_log.hpp"
#include "debug/discovery.hpp"
#include "renderer/game/scene_snapshot.hpp"
#include "renderer/game/indexed_compositor.hpp"
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
    Impl(Machine &value, GameVideoMode selected) : machine(value), mode(selected) {}
    Machine &machine;
    GameVideoMode mode;
    VideoProbe *probe = nullptr;
    void enter(VideoStage stage) {
        if (probe) probe->enter(stage);
    }
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
    bool native_pending = false;
    bool rendered = false;
    std::unique_ptr<SceneSnapshot> snapshot_storage = std::make_unique<SceneSnapshot>();
    SceneSnapshot *snapshot = snapshot_storage.get();
    // Optional caller-owned destination for the next capture or load. `snapshot` always names the newest frame
    // and is never rewritten in place once a target is installed, so a frame handed to another thread stays immutable.
    SceneSnapshot *snapshot_target = nullptr;
    SceneSnapshot &snapshot_dest() { return snapshot_target ? *snapshot_target : *snapshot; }
    struct NativeCompositor {
        explicit NativeCompositor(std::span<const uint8_t> tile_pixels) : tile_rows(tile_pixels) {}
        TileRowTable tile_rows;
        IndexedScratch scratch;
    };
    std::unique_ptr<NativeCompositor> native;
    void compose_native(std::span<uint32_t> output) {
        if (!native) native = std::make_unique<NativeCompositor>(machine.video->playfield_tiles());
        compose_snapshot_indexed(*snapshot, native->tile_rows, machine.video->sprite_tiles(),
                                 native->scratch, output, geometry::NATIVE_WIDTH,
                                 PixelFormat::Argb8888, GameVideoOptions{}, ComposeMode::Serial,
                                 SpriteList::Canonical);
    }
    std::vector<uint32_t> composite;
    std::vector<uint16_t> sprite_plane;

    RamShadow<PALETTE_RAM_SIZE> palette_shadow;
    std::array<uint32_t, SceneSnapshot::PALETTE_SIZE> colors{};
    uint64_t colors_version = fresh_content_version();
    void refresh_colors() {
        const auto &palette = machine.palette;
        if (!palette_shadow.changed(palette.data())) return;
        for (unsigned i = 0; i < colors.size(); ++i)
            colors[i] = (uint32_t(palette[i * 4 + 1]) << 16) | (uint32_t(palette[i * 4 + 2]) << 8) |
                        palette[i * 4 + 3];
        colors_version = fresh_content_version();
    }

    void take_snapshot() {
        auto &frame = snapshot_dest();
        snapshot = &frame;
        frame.fallback = !rendered;
        if (frame.fallback || mode == GameVideoMode::Compare)
            std::copy(machine.m_pixels.begin(), machine.m_pixels.end(),
                      frame.native_pixels.begin());
        if (frame.fallback) return;
        if (frame.tiles.version() != tiles.version()) frame.tiles = tiles;
        if (frame.text.version() != text.version()) frame.text = text;
        if (frame.rows_version != lines.rows_version()) {
            std::copy(lines.rows().begin(), lines.rows().end(), frame.rows.begin());
            frame.rows_version = lines.rows_version();
        }
        const auto current = sprites.presented_sprites();
        frame.sprite_count = unsigned(current.size());
        std::copy(current.begin(), current.end(), frame.sprites.begin());
        const auto canonical = sprites.sprites();
        frame.canonical_sprite_count = unsigned(canonical.size());
        std::copy(canonical.begin(), canonical.end(), frame.canonical_sprites.begin());
        frame.shadow_count = unsigned(std::count_if(current.begin(), current.end(),
                                                    [](const SceneSprite &s) { return s.shadow; }));
        frame.pen_mask = sprites.pen_mask();
        frame.flipped = sprites.flipped();
        if (frame.colors_version != colors_version) {
            frame.colors = colors;
            frame.colors_version = colors_version;
        }
    }

    void materialize_native() {
        if (!native_pending) return;
        compose_native(machine.m_pixels);
        native_pending = false;
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
               std::apply([](const auto &...c) { return Impl::state_size_all(c...); }, stateful());
    }
    void save_state(StateWriter &writer) const {
        uint8_t r = rendered ? 1 : 0;
        writer.write(r);
        std::apply([&](const auto &...c) { Impl::save_all(writer, c...); }, stateful());
    }
    void load_state(StateReader &reader) {
        uint8_t r;
        reader.read(r);
        rendered = r != 0;
        std::apply([&](auto &...c) { Impl::load_all(reader, c...); }, stateful());
        native_pending = false;
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

GameVideo::GameVideo(Machine &machine, GameVideoMode mode)
    : m_impl(std::make_unique<Impl>(require_matching_video(machine), mode)) {
    machine.video->enable_scene_inspection(mode != GameVideoMode::Game);
    reset();
}
GameVideo::~GameVideo() = default;
void GameVideo::reset() {
    m_impl->materialize_native();
    std::apply([](auto &...c) { Impl::reset_all(c...); }, m_impl->sources());
    m_impl->native_pending = false;
    {
        auto &frame = m_impl->snapshot_dest();
        m_impl->snapshot = &frame;
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

void GameVideo::render_frame() {
    auto &state = *m_impl;
    auto &m = state.machine;
    state.enter(VideoStage::Decode);
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
    state.refresh_colors();
    state.sprites.set_presentation(nullptr);
    if (state.sprites.presented_valid()) {
        ++state.presented_frames;
        state.presented_overflow_frames += state.sprites.presented_overflow();
    }
    state.enter(VideoStage::Scene);
    render();
    state.enter(VideoStage::Oracle);
    if (state.rendered && state.mode == GameVideoMode::Game) {
        // Keep only the oracle's sprite lag current, so an unsupported future
        // frame can use its exact visible result without rerunning the CPU.
        // Do not remove: without it, a fallback frame's sprite lag is wrong.
        m.video->vblank(m.graphics);
    } else {
        m.video->render_frame(m.palette, m.graphics, m.control, m.m_pixels);
    }
    state.enter(VideoStage::Capture);
    state.native_pending = false;
    state.take_snapshot();
    state.native_pending = state.rendered && state.mode == GameVideoMode::Game;
    if (state.rendered && state.mode == GameVideoMode::Compare) compare_composite(m.frame + 1);
    state.enter(VideoStage::Latch);
    state.sprites.latch();
    state.enter(VideoStage::Idle);
}

void GameVideo::set_probe(VideoProbe *probe) { m_impl->probe = probe; }

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
    state.rendered = true;
    ++state.rendered_frames;
}

void GameVideo::materialize_native() const { m_impl->materialize_native(); }
void GameVideo::set_snapshot_target(SceneSnapshot *target) { m_impl->snapshot_target = target; }
const SceneSnapshot &GameVideo::scene_snapshot() const { return *m_impl->snapshot; }

void GameVideo::compare_layers(uint64_t frame, unsigned layer_mask) {
    if (!layer_mask || (layer_mask & ~ALL_LAYERS))
        throw std::runtime_error("Video layer mask must select PF0..3, SP0..3, text (bits 0..8)");
    auto &m = m_impl->machine;
    const auto assets = m.video->playfield_tiles();
    auto &plane = m_impl->sprite_plane;
    plane.resize(size_t(432) * 256);
    m_impl->sprites.raster(m.video->sprite_tiles(), plane);
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
                    const uint16_t actual = plane[offset];
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
    auto &game = m_impl->composite;
    game.resize(geometry::NATIVE_PIXELS);
    m_impl->compose_native(game);
    uint64_t count = 0;
    unsigned first = 0;
    const auto &oracle = m_impl->machine.m_pixels;
    for (unsigned i = 0; i < game.size(); ++i) {
        if (game[i] == oracle[i]) continue;
        if (!count) first = i;
        ++count;
    }
    ++m_impl->composite_frames;
    m_impl->composite_mismatches += count;
    if (count) {
        std::ostringstream error;
        error << "Game composite frame " << frame << ": " << count
              << " RGB pixel mismatches; first (" << first % geometry::NATIVE_WIDTH << ','
              << first / geometry::NATIVE_WIDTH << ") game=0x" << std::hex << game[first]
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
    StateReader reader(src);
    m_impl->load_state(reader);
    {
        auto &frame = m_impl->snapshot_dest();
        m_impl->snapshot = &frame;
        frame.fallback = true;
        std::copy(m_impl->machine.m_pixels.begin(), m_impl->machine.m_pixels.end(),
                  frame.native_pixels.begin());
    }
    if (reader.remaining() != 0) {
        throw std::logic_error("GameVideo::load_state remaining unread bytes");
    }
}

static_assert(SnapshotRoot<GameVideo>);
} // namespace f3rt
