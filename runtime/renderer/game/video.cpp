#include "renderer/game/video.hpp"
#include "f3rt/machine.hpp"
#include "sprites/sprite_system.hpp"
#include "state_io.hpp"
#include <optional>
#include <ostream>
#include <stdexcept>
#include <utility>

namespace f3rt {
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
} // namespace

VideoRam VideoRam::from(const Machine &m) { return {m.graphics, m.control, m.frame + 1, m.palette}; }

GameVideo::GameVideo(Machine &machine, RenderEnhancements enhancements)
    : m_machine(require_matching_video(machine)), m_enhancements(std::move(enhancements)) {
    reset();
}

SceneSnapshot &GameVideo::next_snapshot_slot() {
    m_snapshot = m_snapshot_target ? m_snapshot_target : m_snapshot_storage.get();
    return *m_snapshot;
}

void GameVideo::reset() {
    materialize_native();
    m_tiles.reset();
    m_text.reset();
    m_palette.reset();
    m_sprites.reset();
    m_lines.reset();
    m_native_pending = false;
    m_enhancements.reset();
    next_snapshot_slot().capture_fallback();
    m_presented_frames = m_presented_overflow_frames = 0;
    m_alt_maps.reset();
    m_alt_rows_remapped = m_alt_rows_fallback = 0;
    m_rendered_frames = 0;
}

void GameVideo::render_frame() {
    auto &m = m_machine;
    enter(VideoStage::Decode);
    const VideoRam vram = VideoRam::from(m);
    const SpritePresentation presentation =
        m.sprites && m.sprites->active() ? m.sprites->presentation() : SpritePresentation{};
    const auto sprite_assets = m.graphics_assets.sprite_tiles();
    const auto present_enhancements = [&] {
        if (m_enhancements.empty()) return;
        const auto frame = m.sprites ? SpriteFrame::read(m.sprites->tracker(), m_sprites.flipped())
                                     : std::nullopt;
        m_enhancements.present(m_sprites.presented_sprites(), frame);
    };

    m_tiles.decode(vram);
    m_text.decode(vram);
    m_palette.decode(vram);
    if (m_sprites.before_scanout(vram, presentation, sprite_assets) == SpriteLatch::Latched)
        present_enhancements();
    m_lines.decode(vram, m_sprites.scanout_flipped());

    enter(VideoStage::Scene);
    const auto alt_counts = m_lines.resolve_scene(m_sprites.scanout_flipped(), m_tiles, m_alt_maps,
                                                  m.graphics_assets.playfield_tiles());
    m_alt_rows_remapped += alt_counts.remapped;
    m_alt_rows_fallback += alt_counts.fallback;
    ++m_rendered_frames;

    enter(VideoStage::Capture);
    next_snapshot_slot().capture(m_tiles, m_text, m_palette, m_sprites, m_lines, m_enhancements);
    m_native_pending = true;

    enter(VideoStage::Latch);
    if (m_sprites.after_scanout(vram, presentation, sprite_assets) == SpriteLatch::Latched)
        present_enhancements();
    if (m_sprites.presented_valid()) {
        ++m_presented_frames;
        m_presented_overflow_frames += m_sprites.presented_overflow();
    }
    enter(VideoStage::Idle);
}

void GameVideo::materialize_native() const {
    if (!m_native_pending) return;
    if (!m_native)
        m_native = std::make_unique<NativeCompositor>(m_machine.graphics_assets.playfield_tiles());
    m_snapshot->compose(m_machine.m_pixels, geometry::NATIVE_WIDTH, PixelFormat::Argb8888, 0,
                        m_native->tile_rows, m_machine.graphics_assets.sprite_tiles(),
                        m_native->scratch, SpriteList::Canonical);
    m_native_pending = false;
}

void GameVideo::report(std::ostream &output) const {
    m_enhancements.report(output);
    if (m_presented_frames || game_config::video.full_resolution_alt_maps) {
        output << "VIDEO presented_sprite_frames=" << m_presented_frames
               << " presented_sprite_overflow_frames=" << m_presented_overflow_frames;
        if constexpr (game_config::video.full_resolution_alt_maps)
            output << " presented_alt_rows_remapped=" << m_alt_rows_remapped
                   << " presented_alt_rows_fallback=" << m_alt_rows_fallback
                   << " presented_alt_rows_solved=" << m_alt_maps.solved_rows();
        output << '\n';
    }
    output << "VIDEO game_frames=" << m_rendered_frames << '\n';
}

size_t GameVideo::state_size() const {
    return GameSprites::TRAIL_STATE_SIZE + m_sprites.state_size() + m_lines.state_size();
}

void GameVideo::save_state(std::span<uint8_t> dst) const {
    if (dst.size() != state_size()) {
        throw std::invalid_argument("GameVideo::save_state size mismatch");
    }
    materialize_native();
    StateWriter writer(dst);
    m_sprites.save_state(writer);
    m_lines.save_state(writer);
    m_sprites.save_trails(writer);
    if (writer.remaining() != 0) {
        throw std::logic_error("GameVideo::save_state remaining unwritten bytes");
    }
}

void GameVideo::load_state(std::span<const uint8_t> src) {
    if (src.size() != state_size()) {
        throw std::invalid_argument("GameVideo::load_state size mismatch");
    }
    StateReader reader(src);
    m_sprites.load_state(reader);
    m_lines.load_state(reader);
    m_sprites.load_trails(reader);
    m_native_pending = false;
    if (!m_enhancements.empty()) m_enhancements.present(m_sprites.presented_sprites(), std::nullopt);
    next_snapshot_slot().capture_fallback(m_machine.m_pixels);
    if (reader.remaining() != 0) {
        throw std::logic_error("GameVideo::load_state remaining unread bytes");
    }
}

static_assert(SnapshotRoot<GameVideo>);
} // namespace f3rt
