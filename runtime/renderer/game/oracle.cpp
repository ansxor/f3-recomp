#include "renderer/game/oracle.hpp"
#include "f3rt/machine.hpp"
#include "renderer/game/scene_snapshot.hpp"
#include "renderer/game/video.hpp"
#include <algorithm>
#include <ostream>
#include <sstream>
#include <stdexcept>
#include <string>

namespace f3rt {
namespace {

bool differs(ScenePixel game, ScenePixel oracle) {
    const bool visible = (game.flags & 0x10) != 0;
    return visible != ((oracle.flags & 0x10) != 0) ||
           (visible && (game.palette != oracle.palette || game.flags != oracle.flags));
}

std::string format_layer(const SceneLayer &l) {
    std::ostringstream ss;
    ss << "{en=" << l.enabled << " pri=" << int(l.priority) << " bm=" << int(l.blend_mode)
       << " clip_en=" << int(l.clip_enabled) << " clip_inv=" << int(l.clip_inverted)
       << " clip_mode=" << l.clip_inverse << " b_sel=" << l.blend_select << " mos=" << l.mosaic
       << "}";
    return ss.str();
}

bool layers_differ(const SceneLayer &g, const SceneLayer &o) {
    return g.enabled != o.enabled || g.priority != o.priority || g.blend_mode != o.blend_mode ||
           g.clip_enabled != o.clip_enabled || g.clip_inverted != o.clip_inverted ||
           g.clip_inverse != o.clip_inverse || g.blend_select != o.blend_select ||
           g.mosaic != o.mosaic;
}

} // namespace

OracleVerifier::OracleVerifier(const GraphicsAssets &assets, const VideoConfig &config)
    : m_oracle(assets, config), m_oracle_pixels(geometry::NATIVE_PIXELS) {
    m_oracle.enable_scene_inspection();
}

void OracleVerifier::reset() {
    m_oracle.reset();
    m_frames.fill(0);
    m_mismatches.fill(0);
    m_alt_frames.fill(0);
    m_alt_mismatches.fill(0);
    m_composite_frames = m_composite_mismatches = 0;
}

void OracleVerifier::step(const Machine &machine) {
    m_oracle.render_frame(machine.palette, machine.graphics, machine.control, m_oracle_pixels);
}

void OracleVerifier::compare_rows(const GameVideo &video, uint64_t frame) const {
    for (unsigned screen_y = geometry::FIRST_LINE; screen_y < geometry::END_LINE; ++screen_y) {
        const SceneRow g = video.row(screen_y);
        const auto &o = m_oracle.inspect_scene_row(screen_y);

        if (g.background != o.background) {
            std::ostringstream ss;
            ss << "Frame " << frame << " row " << screen_y << " background mismatch: game 0x"
               << std::hex << g.background << " vs oracle 0x" << o.background;
            throw std::runtime_error(ss.str());
        }

        if (g.mosaic_period != o.mosaic_period) {
            std::ostringstream ss;
            ss << "Frame " << frame << " row " << screen_y << " mosaic_period mismatch: game "
               << int(g.mosaic_period) << " vs oracle " << int(o.mosaic_period);
            throw std::runtime_error(ss.str());
        }

        if (g.bitmap != o.bitmap) {
            std::ostringstream ss;
            ss << "Frame " << frame << " row " << screen_y << " bitmap mismatch: game " << g.bitmap
               << " vs oracle " << o.bitmap;
            throw std::runtime_error(ss.str());
        }

        for (int i = 0; i < 4; ++i) {
            if (g.blend[i] != o.blend[i]) {
                std::ostringstream ss;
                ss << "Frame " << frame << " row " << screen_y << " blend[" << i
                   << "] mismatch: game " << int(g.blend[i]) << " vs oracle " << int(o.blend[i]);
                throw std::runtime_error(ss.str());
            }
            if (g.clips[i].left != o.clips[i].left || g.clips[i].right != o.clips[i].right) {
                std::ostringstream ss;
                ss << "Frame " << frame << " row " << screen_y << " clip[" << i
                   << "] mismatch: game [" << g.clips[i].left << ", " << g.clips[i].right
                   << "] vs oracle [" << o.clips[i].left << ", " << o.clips[i].right << "]";
                throw std::runtime_error(ss.str());
            }
        }

        if (layers_differ(g.text, o.text)) {
            std::ostringstream ss;
            ss << "Frame " << frame << " row " << screen_y << " text layer mismatch: game "
               << format_layer(g.text) << " vs oracle " << format_layer(o.text);
            throw std::runtime_error(ss.str());
        }
        if (g.text.enabled) {
            if (g.text_x != o.text_x || g.text_y != o.text_y) {
                std::ostringstream ss;
                ss << "Frame " << frame << " row " << screen_y << " text coords mismatch: game ("
                   << g.text_x << ", " << g.text_y << ") vs oracle (" << o.text_x << ", "
                   << o.text_y << ")";
                throw std::runtime_error(ss.str());
            }
        }

        for (int i = 0; i < 4; ++i) {
            if (layers_differ(g.sprites[i], o.sprites[i])) {
                std::ostringstream ss;
                ss << "Frame " << frame << " row " << screen_y << " sprite[" << i
                   << "] mismatch: game " << format_layer(g.sprites[i]) << " vs oracle "
                   << format_layer(o.sprites[i]);
                throw std::runtime_error(ss.str());
            }
        }

        for (int i = 0; i < 4; ++i) {
            const auto &g_pf = g.playfields[i];
            const auto &o_pf = o.playfields[i];
            if (layers_differ(g_pf.layer, o_pf.layer)) {
                std::ostringstream ss;
                ss << "Frame " << frame << " row " << screen_y << " PF[" << i
                   << "] layer mismatch: game " << format_layer(g_pf.layer) << " vs oracle "
                   << format_layer(o_pf.layer);
                throw std::runtime_error(ss.str());
            }
            if (g_pf.layer.enabled) {
                if (g_pf.source_x != o_pf.source_x || g_pf.source_y != o_pf.source_y ||
                    g_pf.x_step != o_pf.x_step || g_pf.y_step != o_pf.y_step ||
                    g_pf.y_fraction != o_pf.y_fraction || g_pf.palette_add != o_pf.palette_add) {
                    std::ostringstream ss;
                    ss << "Frame " << frame << " row " << screen_y << " PF[" << i
                       << "] coords mismatch: game {sx=" << g_pf.source_x << " sy=" << g_pf.source_y
                       << " step=" << g_pf.x_step << " y_step=" << g_pf.y_step
                       << " y_phase=" << int(g_pf.y_fraction) << " pal_add=" << g_pf.palette_add
                       << "} vs oracle {sx=" << o_pf.source_x << " sy=" << o_pf.source_y
                       << " step=" << o_pf.x_step << " y_step=" << o_pf.y_step
                       << " y_phase=" << int(o_pf.y_fraction) << " pal_add=" << o_pf.palette_add
                       << "}";
                    throw std::runtime_error(ss.str());
                }
            }
        }
    }
}

void OracleVerifier::compare_layer(LayerId id, const Machine &machine, const GameVideo &video,
                                   uint64_t frame) {
    const unsigned layer = unsigned(id);
    uint64_t count = 0;
    int first_x = -1, first_y = -1;
    ScenePixel first_game{}, first_oracle{};
    const int width = LAYER_INFO[layer].width;
    const int height = LAYER_INFO[layer].height;
    const LayerKind layer_kind = kind(id);
    const bool indexed = layer_kind != LayerKind::Sprite;
    for (int y = 0; y < height; ++y) {
        const auto reference_line =
            layer_kind == LayerKind::Playfield
                ? m_oracle.inspect_playfield_line(sub_index(id), y, machine.graphics)
            : layer_kind == LayerKind::Text ? m_oracle.inspect_text_line(y, machine.graphics)
                                            : VideoLine{};
        for (int x = 0; x < width; ++x) {
            ScenePixel game{}, expected{};
            if (layer_kind == LayerKind::Playfield)
                game = video.playfield_pixel(sub_index(id), x, y, m_oracle.flipscreen());
            else if (layer_kind == LayerKind::Text)
                game = video.text_pixel(x, y, m_oracle.flipscreen());
            if (indexed) expected = {reference_line.palette[x], reference_line.flags[x]};
            else {
                const unsigned offset = (y + geometry::FIRST_LINE) * 432 + x + 46;
                const uint16_t actual = m_sprite_plane[offset];
                const uint16_t reference = m_oracle.sprite_plane()[offset];
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
    ++m_frames[layer];
    m_mismatches[layer] += count;
    if (!count) return;
    std::ostringstream error;
    error << "Game " << LAYER_INFO[layer].name << " frame " << frame << ": " << count
          << " indexed pixel mismatches; first (" << first_x << ',' << first_y << ") game=0x"
          << std::hex << first_game.palette << '/' << unsigned(first_game.flags)
          << " oracle=0x" << first_oracle.palette << '/' << unsigned(first_oracle.flags);
    if (layer_kind == LayerKind::Sprite) {
        for (const auto &sprite : video.sprite_snapshot()) {
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

void OracleVerifier::compare_alt_map(unsigned alt, const Machine &machine, const GameVideo &video,
                                     uint64_t frame) {
    const LayerId id = playfield(2 + alt);
    uint64_t count = 0;
    int first_x = -1, first_y = -1;
    ScenePixel first_game{}, first_oracle{};
    for (int y = 0; y < LAYER_INFO[unsigned(id)].height; ++y) {
        const auto reference = m_oracle.inspect_playfield_line(4 + alt, y, machine.graphics);
        for (int x = 0; x < LAYER_INFO[unsigned(id)].width; ++x) {
            const ScenePixel game = video.playfield_pixel(2 + alt, x, y, m_oracle.flipscreen(),
                                                          true);
            const ScenePixel expected{reference.palette[x], reference.flags[x]};
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
    ++m_alt_frames[alt];
    m_alt_mismatches[alt] += count;
    if (!count) return;
    std::ostringstream error;
    error << "Game " << LAYER_INFO[unsigned(id)].name << " alternate map frame " << frame << ": "
          << count << " indexed pixel mismatches; first (" << first_x << ',' << first_y
          << ") game=0x" << std::hex << first_game.palette << '/' << unsigned(first_game.flags)
          << " oracle=0x" << first_oracle.palette << '/' << unsigned(first_oracle.flags);
    throw std::runtime_error(error.str());
}

void OracleVerifier::compare_layers(const Machine &machine, const GameVideo &video, uint64_t frame,
                                    unsigned layer_mask) {
    if (!layer_mask || (layer_mask & ~ALL_LAYERS))
        throw std::runtime_error("Video layer mask must select PF0..3, SP0..3, text (bits 0..8)");
    m_sprite_plane.resize(size_t(432) * 256);
    std::ranges::fill(m_sprite_plane, 0);
    video.raster_sprites(m_sprite_plane);
    if (layer_mask & layer_bit(LayerId::Text)) m_oracle.prepare_text_inspection(machine.graphics);
    if (layer_mask == ALL_LAYERS) compare_rows(video, frame);
    for (unsigned layer = 0; layer < LAYER_COUNT; ++layer) {
        const LayerId id{uint8_t(layer)};
        if (layer_mask & layer_bit(id)) compare_layer(id, machine, video, frame);
    }
    if constexpr (!game_config::video.extend || game_config::video.extended_alt_maps) {
        for (unsigned alt = 0; alt < 2; ++alt)
            if (layer_mask & layer_bit(playfield(2 + alt)))
                compare_alt_map(alt, machine, video, frame);
    }
    if (layer_mask == ALL_LAYERS)
        compare_composite(video.scene_snapshot(), machine.graphics_assets, frame);
}

void OracleVerifier::compare_composite(const SceneSnapshot &snapshot, const GraphicsAssets &assets,
                                       uint64_t frame) {
    m_composite.resize(geometry::NATIVE_PIXELS);
    snapshot.compose(m_composite, geometry::NATIVE_WIDTH, PixelFormat::Argb8888, 0, assets,
                     SpriteList::Canonical);
    uint64_t count = 0;
    unsigned first = 0;
    for (unsigned i = 0; i < m_composite.size(); ++i) {
        if (m_composite[i] == m_oracle_pixels[i]) continue;
        if (!count) first = i;
        ++count;
    }
    ++m_composite_frames;
    m_composite_mismatches += count;
    if (!count) return;
    std::ostringstream error;
    error << "Game composite frame " << frame << ": " << count << " RGB pixel mismatches; first ("
          << first % geometry::NATIVE_WIDTH << ',' << first / geometry::NATIVE_WIDTH << ") game=0x"
          << std::hex << m_composite[first] << " oracle=0x" << m_oracle_pixels[first];
    throw std::runtime_error(error.str());
}

void OracleVerifier::report(std::ostream &output) const {
    for (unsigned layer = 0; layer < LAYER_COUNT; ++layer) {
        if (!m_frames[layer]) continue;
        const unsigned pixels = LAYER_INFO[layer].width * LAYER_INFO[layer].height;
        output << "VIDEO layer=" << LAYER_INFO[layer].name << " domain=" << LAYER_INFO[layer].width
               << 'x' << LAYER_INFO[layer].height << '-' << LAYER_INFO[layer].domain
               << " sampled_frames=" << m_frames[layer]
               << " compared_pixels=" << m_frames[layer] * pixels
               << " pixel_mismatches=" << m_mismatches[layer] << '\n';
    }
    if constexpr (!game_config::video.extend || game_config::video.extended_alt_maps) {
        for (unsigned alt = 0; alt < 2; ++alt) {
            if (!m_alt_frames[alt]) continue;
            const auto &info = LAYER_INFO[unsigned(LayerId::Pf2) + alt];
            output << "VIDEO layer=" << info.name << "-alt domain=" << info.width << 'x'
                   << info.height << '-' << info.domain << " sampled_frames=" << m_alt_frames[alt]
                   << " compared_pixels=" << m_alt_frames[alt] * info.width * info.height
                   << " pixel_mismatches=" << m_alt_mismatches[alt] << '\n';
        }
    }
    if (m_composite_frames)
        output << "VIDEO layer=composite domain=" << geometry::NATIVE_WIDTH << 'x'
               << geometry::HEIGHT << "-RGB sampled_frames=" << m_composite_frames
               << " compared_pixels=" << m_composite_frames * geometry::NATIVE_PIXELS
               << " pixel_mismatches=" << m_composite_mismatches << '\n';
}

} // namespace f3rt
