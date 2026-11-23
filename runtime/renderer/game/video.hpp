#pragma once
#include "f3rt/game_video.hpp"
#include "f3rt/renderer/game/enhancements.hpp"
#include "renderer/game/indexed_compositor.hpp"
#include "renderer/game/lines.hpp"
#include "renderer/game/palette.hpp"
#include "renderer/game/scene_snapshot.hpp"
#include "renderer/game/sprites.hpp"
#include "renderer/game/text.hpp"
#include "renderer/game/tiles.hpp"
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <span>

namespace f3rt {

class GameVideo {
public:
    explicit GameVideo(Machine &machine, RenderEnhancements enhancements = {});
    GameVideo(const GameVideo &) = delete;
    GameVideo &operator=(const GameVideo &) = delete;
    void reset();
    void render_frame();
    void set_probe(VideoProbe *probe) { m_probe = probe; }
    void report(std::ostream &output) const;

    const GameTiles &tiles() const { return m_tiles; }
    const GameText &text() const { return m_text; }
    const GameSprites &sprites() const { return m_sprites; }
    const GameLines &lines() const { return m_lines; }
    // The newest scanout snapshot. Overwritten in place at each VBSTART capture unless a capture target is set.
    const SceneSnapshot &scene_snapshot() const { return *m_snapshot; }
    void set_snapshot_target(SceneSnapshot *target) { m_snapshot_target = target; }

    size_t state_size() const;
    void save_state(std::span<uint8_t> dst) const;
    void load_state(std::span<const uint8_t> src);

private:
    friend class Machine;
    struct NativeCompositor {
        explicit NativeCompositor(std::span<const uint8_t> tile_pixels) : tile_rows(tile_pixels) {}
        TileRowTable tile_rows;
        IndexedScratch scratch;
    };

    void enter(VideoStage stage) {
        if (m_probe) m_probe->enter(stage);
    }
    SceneSnapshot &next_snapshot_slot();
    void materialize_native() const;

    Machine &m_machine;
    VideoProbe *m_probe = nullptr;
    uint64_t m_rendered_frames = 0;

    GameTiles m_tiles;
    GameText m_text;
    GamePalette m_palette;
    GameSprites m_sprites;
    GameLines m_lines;
    RenderEnhancements m_enhancements;

    FullResolutionAltMaps m_alt_maps;
    uint64_t m_alt_rows_remapped = 0, m_alt_rows_fallback = 0;
    uint64_t m_presented_frames = 0, m_presented_overflow_frames = 0;

    std::unique_ptr<SceneSnapshot> m_snapshot_storage = std::make_unique<SceneSnapshot>();
    SceneSnapshot *m_snapshot = m_snapshot_storage.get();
    SceneSnapshot *m_snapshot_target = nullptr;

    mutable bool m_native_pending = false;
    mutable std::unique_ptr<NativeCompositor> m_native;
};

}
