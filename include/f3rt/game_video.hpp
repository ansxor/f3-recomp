#pragma once
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <span>
#include "generated_config/game_video_config.hpp"

namespace f3rt {
// Compile-time scanout geometry of the game this binary is built for, from
// [video] in games/<game>/config.toml. GameVideo refuses a ROM set whose
// VideoConfig does not match (game_config::matches).
namespace geometry {
inline constexpr unsigned NATIVE_WIDTH = 320; // visible columns 46..365 on every game
inline constexpr unsigned FRAME_LINES = 256;  // scanout lines the sprite/line RAM address
inline constexpr unsigned FIRST_LINE = game_config::video.visible_y;
inline constexpr unsigned HEIGHT = game_config::video.visible_height;
inline constexpr unsigned END_LINE = game_config::visible_end;
inline constexpr unsigned NATIVE_PIXELS = NATIVE_WIDTH * HEIGHT;

} // namespace geometry
class Machine;
struct SceneSnapshot;
enum class GameVideoMode { Diagnostic, Game, Compare };

struct GameVideoOptions {
    static constexpr unsigned MAX_SCALE = 4, MAX_GPU_SCALE = 8, MAX_BORDER = 160;

    unsigned scale = 1;
    unsigned border = 0; // Additional native scene columns on EACH side.
    constexpr unsigned width() const { return (geometry::NATIVE_WIDTH + border * 2) * scale; }
    constexpr unsigned height() const { return geometry::HEIGHT * scale; }
    constexpr bool expanded() const { return scale != 1 || border != 0; }
};

enum class VideoStage : uint8_t { Idle, Decode, Scene, Oracle, Capture, Latch };
inline constexpr unsigned VIDEO_STAGE_COUNT = 6;

class VideoProbe {
public:
    virtual ~VideoProbe() = default;
    virtual void enter(VideoStage stage) = 0;
};

class GameVideo {
public:
    explicit GameVideo(Machine &machine, GameVideoMode mode = GameVideoMode::Diagnostic);
    ~GameVideo();
    GameVideo(const GameVideo &) = delete;
    GameVideo &operator=(const GameVideo &) = delete;
    void reset();
    void render_frame();
    void set_probe(VideoProbe *probe);
    void compare_layers(uint64_t frame, unsigned layer_mask);
    void report(std::ostream &output) const;
    // The newest scanout snapshot. Overwritten in place at each VBSTART capture unless a capture target is set.
    const SceneSnapshot &scene_snapshot() const;
    // Redirect subsequent captures (and state loads) into caller-owned storage; null restores the internal frame.
    // scene_snapshot() keeps naming the newest frame, which is never rewritten while a target is installed
    // (the caller picks a target that is neither the newest frame nor in use elsewhere).
    void set_snapshot_target(SceneSnapshot *target);
    size_t state_size() const;
    void save_state(std::span<uint8_t> dst) const;
    void load_state(std::span<const uint8_t> src);

private:
    friend class Machine;
    void materialize_native() const;
    void render();
    void compare_composite(uint64_t frame);
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
} // namespace f3rt
