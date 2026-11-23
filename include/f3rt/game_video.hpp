#pragma once
#include <cstdint>
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
class GameVideo;
class Machine;
struct SceneSnapshot;

struct GameVideoOptions {
    static constexpr unsigned MAX_SCALE = 4, MAX_GPU_SCALE = 8, MAX_BORDER = 160;

    unsigned scale = 1;
    unsigned border = 0; // Additional native scene columns on EACH side.
    constexpr unsigned width() const { return (geometry::NATIVE_WIDTH + border * 2) * scale; }
    constexpr unsigned height() const { return geometry::HEIGHT * scale; }
    constexpr bool expanded() const { return scale != 1 || border != 0; }
};

enum class VideoStage : uint8_t { Idle, Decode, Scene, Capture, Latch };
inline constexpr unsigned VIDEO_STAGE_COUNT = 5;

class VideoProbe {
public:
    virtual ~VideoProbe() = default;
    virtual void enter(VideoStage stage) = 0;
};
} // namespace f3rt
