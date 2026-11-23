#include "renderer/game/video.hpp"
#include "f3rt/machine.hpp"
#include "sprites/sprite_system.hpp"
#include "sprites/groups.hpp"
#include <algorithm>
#include <ostream>
#include <stdexcept>

namespace f3rt {
namespace {

rust_video::VideoConfig rust_config(const VideoConfig &config) {
    return {config.rotation,       config.sprite_lag,
            config.visible_y,      config.visible_height,
            config.extend,         config.extended_alt_maps,
            config.full_resolution_alt_maps};
}

rust::Slice<const uint8_t> view_of(std::span<const uint8_t> span) {
    return rust::Slice<const uint8_t>(span.data(), span.size());
}

// The renderer is compiled against games/<game>/config.toml [video]; a ROM set
// carrying different geometry must never be drawn with it.
Machine &require_matching_video(Machine &machine) {
    if (!game_config::matches(machine.roms.video))
        throw std::runtime_error(
            "Game video geometry (rotation/sprite lag/visible window/extend/alternate maps) "
            "differs from the geometry this build was configured for");
    return machine;
}

rust::Box<rust_video::SpritePresentation> rust_presentation(const SpritePresentation *source) {
    auto presented = rust_video::new_sprite_presentation();
    if (!source) return presented;
    for (const auto &group : source->groups)
        presented->push_group(group.key, group.x, group.y, group.pc);
    for (const auto &splice : source->splices) {
        presented->push_splice(splice.bank, splice.first, splice.last, view_of(splice.real),
                               view_of(splice.replacement));
        for (const auto &group : splice.groups)
            presented->push_splice_group(group.key, group.x, group.y, group.pc);
    }
    return presented;
}

} // namespace

GameVideo::GameVideo(Machine &machine, RenderEnhancements enhancements)
    : m_machine(require_matching_video(machine)), m_enhancements(std::move(enhancements)),
      m_video(rust_video::new_game_video(rust_config(machine.roms.video))),
      m_snapshot_storage(std::make_unique<SceneSnapshot>(machine.roms.video)) {
    reset();
}

SceneSnapshot &GameVideo::next_snapshot_slot() {
    return m_snapshot_target ? *m_snapshot_target : *m_snapshot_storage;
}

rust_video::EnhancementsHost GameVideo::enhancements_host() const {
    const auto *tracker =
        m_machine.sprites ? &m_machine.sprites->tracker() : nullptr;
    return {const_cast<RenderEnhancements *>(&m_enhancements), tracker};
}

void GameVideo::reset() {
    auto host = enhancements_host();
    m_video->reset_with(m_machine.graphics_assets.inner(), host);
    next_snapshot_slot().capture_fallback();
    next_snapshot_slot().refresh();
}

void GameVideo::render_frame() {
    auto &m = m_machine;
    const SpritePresentation presentation =
        m.sprites && m.sprites->active() ? m.sprites->presentation() : SpritePresentation{};
    auto rust_presented = rust_presentation(&presentation);
    rust_video::ProbeHost probe{m_probe};
    auto host = enhancements_host();
    SceneSnapshot &target = next_snapshot_slot();
    m_video->render_frame(m.graphics_assets.inner(), view_of(m.graphics), view_of(m.control),
                          view_of(m.palette), m.frame + 1, *rust_presented, probe, host,
                          target.destination());
    target.refresh();
}

const SceneSnapshot &GameVideo::scene_snapshot() const {
    auto &snapshot = const_cast<GameVideo *>(this)->next_snapshot_slot();
    snapshot.refresh();
    return snapshot;
}

ScenePixel GameVideo::playfield_pixel(unsigned layer, int x, int y, bool flipped, bool alt) const {
    const auto pixel = m_video->playfield_pixel(layer, x, y, flipped,
                                               m_machine.graphics_assets.inner(), alt);
    return {pixel.palette, pixel.flags};
}

ScenePixel GameVideo::text_pixel(int x, int y, bool flipped) const {
    const auto pixel = m_video->text_pixel(x, y, flipped);
    return {pixel.palette, pixel.flags};
}

SceneRow GameVideo::row(unsigned screen_y) const {
    const auto row = m_video->row(screen_y);
    SceneRow out{};
    for (unsigned i = 0; i < 4; ++i) {
        const auto &pf = row.playfields[i];
        out.playfields[i].layer = {pf.layer.priority,    pf.layer.blend_mode,
                                   pf.layer.clip_enabled, pf.layer.clip_inverted,
                                   pf.layer.clip_inverse, pf.layer.enabled,
                                   pf.layer.blend_select, pf.layer.mosaic};
        out.playfields[i].source_x = pf.source_x;
        out.playfields[i].source_y = pf.source_y;
        out.playfields[i].x_step = pf.x_step;
        out.playfields[i].y_step = pf.y_step;
        out.playfields[i].y_fraction = pf.y_fraction;
        out.playfields[i].palette_add = pf.palette_add;
        out.playfields[i].alt_map = pf.alt_map;
        out.playfields[i].empty_row = pf.empty_row;
        out.playfields[i].full_res_offset = pf.full_res_offset;
        const auto &sp = row.sprites[i];
        out.sprites[i] = {sp.priority,    sp.blend_mode,  sp.clip_enabled, sp.clip_inverted,
                          sp.clip_inverse, sp.enabled,     sp.blend_select, sp.mosaic};
        out.clips[i] = {row.clips[i].left, row.clips[i].right};
        out.blend[i] = row.blend[i];
    }
    out.text = {row.text.priority,   row.text.blend_mode,  row.text.clip_enabled,
                row.text.clip_inverted, row.text.clip_inverse, row.text.enabled,
                row.text.blend_select,  row.text.mosaic};
    out.background = row.background;
    out.text_x = row.text_x;
    out.text_y = row.text_y;
    out.mosaic_period = row.mosaic_period;
    out.bitmap = row.bitmap;
    out.palette_15bit = row.palette_15bit;
    out.blur = row.blur;
    return out;
}

std::vector<SceneSprite> GameVideo::sprite_snapshot() const {
    std::vector<rust_video::SceneSprite> buffer(MAX_HARDWARE_SPRITES);
    const size_t count = m_video->sprite_list(
        rust::Slice<rust_video::SceneSprite>(buffer.data(), buffer.size()));
    std::vector<SceneSprite> out;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const auto &s = buffer[i];
        out.push_back({s.x,        s.y,        s.scale_x, s.scale_y, s.tile,     s.palette,
                       s.flip_x,   s.flip_y,   s.group,   s.group_x, s.group_y, s.group_pc,
                       s.blend_weight});
    }
    return out;
}

void GameVideo::raster_sprites(std::span<uint16_t> plane) const {
    m_video->raster_sprites(m_machine.graphics_assets.inner(),
                            rust::Slice<uint16_t>(plane.data(), plane.size()));
}

void GameVideo::report(std::ostream &output) const {
    m_enhancements.report(output);
    output << m_video->report();
}

size_t GameVideo::state_size() const { return m_video->state_size(); }

void GameVideo::save_state(std::span<uint8_t> dst) const {
    if (dst.size() != state_size()) throw std::invalid_argument("GameVideo::save_state size mismatch");
    auto &video = const_cast<rust_video::GameVideo &>(*m_video);
    video.save_state(rust::Slice<uint8_t>(dst.data(), dst.size()),
                     m_machine.graphics_assets.inner());
}

void GameVideo::materialize_native() const {
    auto &video = const_cast<rust_video::GameVideo &>(*m_video);
    const auto &target =
        m_snapshot_target ? *m_snapshot_target : *m_snapshot_storage;
    video.materialize_native(m_machine.graphics_assets.inner(), target.inner());
    const auto pixels = video.native_pixels();
    std::ranges::copy(pixels, m_machine.m_pixels.begin());
}

void GameVideo::load_state(std::span<const uint8_t> src) {
    if (src.size() != state_size()) throw std::invalid_argument("GameVideo::load_state size mismatch");
    m_video->set_native_pixels(rust::Slice<const uint32_t>(
        m_machine.m_pixels.data(), m_machine.m_pixels.size()));
    auto host = enhancements_host();
    SceneSnapshot &target = next_snapshot_slot();
    m_video->load_state(rust::Slice<const uint8_t>(src.data(), src.size()), host,
                        target.destination());
    target.refresh();
}

} // namespace f3rt
