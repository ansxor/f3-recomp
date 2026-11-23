#pragma once
#include "f3rt/game_video.hpp"
#include "f3rt/video.hpp"
#include "renderer/game/indexed_compositor.hpp"
#include "renderer/game/scene.hpp"
#include <array>
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <span>
#include <vector>

namespace f3rt {
class GameLines;
class GraphicsAssets;
class Machine;
struct SceneSnapshot;
struct VideoConfig;

class OracleVerifier {
public:
    OracleVerifier(const GraphicsAssets &assets, const VideoConfig &config);

    void reset();
    // Render the machine's current palette, video RAM and control registers with the oracle.
    void step(const Machine &machine);
    // The oracle's last composed frame (320 x geometry::HEIGHT ARGB).
    std::span<const uint32_t> oracle_pixels() const { return m_oracle_pixels; }
    // Compare scene rows, then each layer selected by `layer_mask` (bits 0..8, LayerId order)
    // after the oracle's last step(). The full mask also compares the composite.
    void compare_layers(const Machine &machine, const GameVideo &video, uint64_t frame,
                        unsigned layer_mask);
    // Compose `snapshot` canonically on the CPU and compare it with the oracle's last frame.
    void compare_composite(const SceneSnapshot &snapshot, const GraphicsAssets &assets,
                           uint64_t frame);
    void report(std::ostream &output) const;

private:
    void compare_rows(const GameLines &lines, uint64_t frame) const;
    void compare_layer(LayerId id, const Machine &machine, const GameVideo &video, uint64_t frame);
    void compare_alt_map(unsigned alt, const Machine &machine, const GameVideo &video,
                         uint64_t frame);

    Video m_oracle;
    std::vector<uint32_t> m_oracle_pixels;
    std::vector<uint32_t> m_composite;
    std::vector<uint16_t> m_sprite_plane;
    std::unique_ptr<TileRowTable> m_tile_rows;
    IndexedScratch m_scratch;
    std::array<uint64_t, LAYER_COUNT> m_frames{}, m_mismatches{};
    std::array<uint64_t, 2> m_alt_frames{}, m_alt_mismatches{};
    uint64_t m_composite_frames = 0, m_composite_mismatches = 0;
};

}
