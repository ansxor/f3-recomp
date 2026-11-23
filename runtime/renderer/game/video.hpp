#pragma once
#include "f3rt/game_video.hpp"
#include "f3rt/renderer/game/enhancements.hpp"
#include "f3rt_video_cxx/video.h"
#include "renderer/game/scene.hpp"
#include "renderer/game/scene_snapshot.hpp"
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <span>
#include <vector>

namespace f3rt {

// The game-side frame driver: thin host façade over the Rust `rust_video::GameVideo`, keeping the
// `Machine`, `VideoProbe` and `RenderEnhancements` (C++ `SpriteStage` plugins) bindings.
class GameVideo {
public:
    explicit GameVideo(Machine &machine, RenderEnhancements enhancements = {});
    GameVideo(const GameVideo &) = delete;
    GameVideo &operator=(const GameVideo &) = delete;
    void reset();
    void render_frame();
    void set_probe(VideoProbe *probe) { m_probe = probe; }
    void report(std::ostream &output) const;

    // The newest scanout snapshot. Overwritten in place at each VBSTART capture unless a capture
    // target is set.
    const SceneSnapshot &scene_snapshot() const;
    void set_snapshot_target(SceneSnapshot *target) { m_snapshot_target = target; }

    // Oracle diagnostics: identical queries to the retired C++ decoders, answered from Rust.
    ScenePixel playfield_pixel(unsigned layer, int x, int y, bool flipped, bool alt = false) const;
    ScenePixel text_pixel(int x, int y, bool flipped) const;
    SceneRow row(unsigned screen_y) const;
    std::vector<SceneSprite> sprite_snapshot() const;
    void raster_sprites(std::span<uint16_t> plane) const;

    void materialize_native() const;
    size_t state_size() const;
    void save_state(std::span<uint8_t> dst) const;
    void load_state(std::span<const uint8_t> src);

private:
    friend class Machine;
    SceneSnapshot &next_snapshot_slot();
    rust_video::EnhancementsHost enhancements_host() const;

    Machine &m_machine;
    VideoProbe *m_probe = nullptr;
    RenderEnhancements m_enhancements;
    rust::Box<rust_video::GameVideo> m_video;
    std::unique_ptr<SceneSnapshot> m_snapshot_storage;
    SceneSnapshot *m_snapshot_target = nullptr;
};

} // namespace f3rt
