#include "renderer/game/scene_snapshot.hpp"
#include "f3rt/rom.hpp"
#include <algorithm>
#include <stdexcept>

namespace f3rt {
namespace {

rust_video::VideoConfig rust_config(const VideoConfig &config) {
    return {config.rotation,       config.sprite_lag,
            config.visible_y,      config.visible_height,
            config.extend,         config.extended_alt_maps,
            config.full_resolution_alt_maps};
}

rust_video::SpriteList rust_list(SpriteList list) {
    return list == SpriteList::Canonical ? rust_video::SpriteList::Canonical
                                         : rust_video::SpriteList::Presented;
}

rust_video::PixelFormat rust_format(PixelFormat format) {
    return format == PixelFormat::Rgba8888 ? rust_video::PixelFormat::Rgba8888
                                           : rust_video::PixelFormat::Argb8888;
}

rust::Slice<rust_video::SceneRow> row_slice(std::array<SceneRow, 256> &rows) {
    return {reinterpret_cast<rust_video::SceneRow *>(rows.data()), rows.size()};
}

rust::Slice<const rust_video::SceneRow> const_row_slice(const std::array<SceneRow, 256> &rows) {
    return {reinterpret_cast<const rust_video::SceneRow *>(rows.data()), rows.size()};
}

rust::Slice<rust_video::SceneSprite> sprite_slice(std::array<SceneSprite, MAX_PRESENTED_SPRITES> &s) {
    return {reinterpret_cast<rust_video::SceneSprite *>(s.data()), s.size()};
}

rust::Slice<rust_video::SceneSprite>
canonical_slice(std::array<SceneSprite, MAX_HARDWARE_SPRITES> &s) {
    return {reinterpret_cast<rust_video::SceneSprite *>(s.data()), s.size()};
}

rust::Slice<const rust_video::SceneSprite> const_sprite_slice(const SceneSprite *sprites,
                                                              unsigned count) {
    return {reinterpret_cast<const rust_video::SceneSprite *>(sprites), count};
}

static_assert(sizeof(SceneSprite) == sizeof(rust_video::SceneSprite),
              "host sprite mirror layout must match the Rust bridge");
static_assert(sizeof(SceneRow) == sizeof(rust_video::SceneRow),
              "host row mirror layout must match the Rust bridge");
static_assert(sizeof(ScenePlayfield) == sizeof(rust_video::ScenePlayfield));
static_assert(sizeof(SceneLayer) == sizeof(rust_video::SceneLayer));
static_assert(sizeof(SceneClip) == sizeof(rust_video::SceneClip));

} // namespace

SceneSnapshot::SceneSnapshot() : SceneSnapshot(game_config::video) {}

SceneSnapshot::SceneSnapshot(const VideoConfig &config)
    : m_inner(rust_video::new_scene_snapshot(rust_config(config))) {}

SceneSnapshot::SceneSnapshot(const SceneSnapshot &other)
    : rows(other.rows), sprites(other.sprites), sprite_count(other.sprite_count),
      canonical_sprites(other.canonical_sprites),
      canonical_sprite_count(other.canonical_sprite_count), colors(other.colors),
      pen_mask(other.pen_mask), flipped(other.flipped), sprite_flipped(other.sprite_flipped),
      sprite_trail_seed(other.sprite_trail_seed), fallback(other.fallback),
      native_pixels(other.native_pixels), m_inner(other.m_inner->clone_scene_snapshot()),
      m_mirror_dirty(other.m_mirror_dirty) {}

SceneSnapshot &SceneSnapshot::operator=(const SceneSnapshot &other) {
    if (this == &other) return *this;
    rows = other.rows;
    sprites = other.sprites;
    sprite_count = other.sprite_count;
    canonical_sprites = other.canonical_sprites;
    canonical_sprite_count = other.canonical_sprite_count;
    colors = other.colors;
    pen_mask = other.pen_mask;
    flipped = other.flipped;
    sprite_flipped = other.sprite_flipped;
    sprite_trail_seed = other.sprite_trail_seed;
    fallback = other.fallback;
    native_pixels = other.native_pixels;
    m_inner = other.m_inner->clone_scene_snapshot();
    m_mirror_dirty = other.m_mirror_dirty;
    return *this;
}

SceneSnapshot::~SceneSnapshot() = default;

void SceneSnapshot::refresh() const {
    auto &self = const_cast<SceneSnapshot &>(*this);
    self.sprite_trail_seed.resize(m_inner->trail_len());
    const auto meta = m_inner->export_scene(
        row_slice(self.rows), sprite_slice(self.sprites), canonical_slice(self.canonical_sprites),
        rust::Slice<uint32_t>(self.colors.data(), self.colors.size()),
        rust::Slice<uint16_t>(self.sprite_trail_seed.data(), self.sprite_trail_seed.size()));
    self.sprite_count = meta.sprite_count;
    self.canonical_sprite_count = meta.canonical_sprite_count;
    self.pen_mask = meta.pen_mask;
    self.flipped = meta.flipped;
    self.sprite_flipped = meta.sprite_flipped;
    self.fallback = meta.fallback;
    if (meta.fallback)
        m_inner->export_native(
            rust::Slice<uint32_t>(self.native_pixels.data(), self.native_pixels.size()));
    self.m_mirror_dirty = false;
}

void SceneSnapshot::flush() const {
    auto &self = const_cast<SceneSnapshot &>(*this);
    const rust_video::SceneMeta meta{self.sprite_count,
                                     self.canonical_sprite_count,
                                     self.pen_mask,
                                     self.flipped,
                                     self.sprite_flipped,
                                     self.fallback,
                                     unsigned(self.sprite_trail_seed.size())};
    m_inner->import_scene(
        const_row_slice(self.rows), const_sprite_slice(self.sprites.data(), self.sprite_count),
        const_sprite_slice(self.canonical_sprites.data(), self.canonical_sprite_count),
        rust::Slice<const uint32_t>(self.colors.data(), self.colors.size()),
        rust::Slice<const uint16_t>(self.sprite_trail_seed.data(), self.sprite_trail_seed.size()),
        meta);
    if (self.fallback)
        m_inner->import_native(
            rust::Slice<const uint32_t>(self.native_pixels.data(), self.native_pixels.size()));
    self.m_mirror_dirty = false;
}

const rust_video::SceneSnapshot &SceneSnapshot::inner() const {
    flush();
    return *m_inner;
}

std::span<const SceneSprite> SceneSnapshot::sprite_list() const {
    return {sprites.data(), sprite_count};
}

std::span<const SceneSprite> SceneSnapshot::sprite_list(SpriteList list) const {
    return list == SpriteList::Canonical ? canonical_sprite_list() : sprite_list();
}

std::span<const SceneSprite> SceneSnapshot::canonical_sprite_list() const {
    return {canonical_sprites.data(), canonical_sprite_count};
}

bool SceneSnapshot::presents_enhanced_sprites() const {
    return !fallback && (!std::ranges::equal(sprite_list(), canonical_sprite_list()) ||
                         std::ranges::any_of(sprite_list(), &SceneSprite::weighted));
}

unsigned SceneSnapshot::sprite_count_for(SpriteList list) const {
    flush();
    return unsigned(m_inner->sprite_count(rust_list(list)));
}

std::vector<uint16_t> SceneSnapshot::text_map() const {
    std::vector<uint16_t> map(4096);
    m_inner->text_map(rust::Slice<uint16_t>(map.data(), map.size()));
    return map;
}

void SceneSnapshot::set_text_map(std::span<const uint16_t> map) {
    m_inner->set_text_map(rust::Slice<const uint16_t>(map.data(), map.size()));
}

ScenePixel SceneSnapshot::text_pixel(int x, int y, bool flipped) const {
    const auto pixel = m_inner->text_pixel(x, y, flipped);
    return {pixel.palette, pixel.flags};
}

unsigned SceneSnapshot::trail_len() const {
    flush();
    return unsigned(m_inner->trail_len());
}

bool SceneSnapshot::fallback_frame() const { return fallback; }

SceneSnapshot &SceneSnapshot::capture_fallback(std::span<const uint32_t> native) {
    fallback = true;
    if (native.empty()) {
        native_pixels.fill(0xff000000u);
    } else {
        std::ranges::copy_n(native.begin(), std::min(native.size(), native_pixels.size()),
                            native_pixels.begin());
    }
    flush();
    return *this;
}

void SceneSnapshot::compose(std::span<uint32_t> output, size_t stride, PixelFormat format,
                            unsigned border, const GraphicsAssets &assets,
                            SpriteList list) const {
    if (border > GameVideoOptions::MAX_BORDER)
        throw std::runtime_error("Indexed composition border out of range");
    const GameVideoOptions options{1, border};
    if (stride < options.width() || output.size() < stride * options.height())
        throw std::runtime_error("Incomplete indexed frame render buffers");
    flush();
    m_inner->compose(rust::Slice<uint32_t>(output.data(), output.size()), stride,
                     rust_format(format), border, assets.inner(), rust_list(list));
}

} // namespace f3rt
