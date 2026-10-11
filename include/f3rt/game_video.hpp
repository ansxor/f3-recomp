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
inline constexpr unsigned native_width = 320;   // visible columns 46..365 on every game
inline constexpr unsigned frame_lines = 256;    // scanout lines the sprite/line RAM address
inline constexpr unsigned first_line = game_config::video.visible_y;
inline constexpr unsigned height = game_config::video.visible_height;
inline constexpr unsigned end_line = game_config::visible_end;
inline constexpr unsigned native_pixels = native_width * height;
} // namespace geometry
class Machine;
struct CapturedFrame;
enum class GameVideoMode { Diagnostic, Game, Compare };

struct GameVideoOptions {
    static constexpr unsigned max_scale = 4, max_gpu_scale = 8, max_border = 160;
    unsigned scale = 1;
    unsigned border = 0; // Additional native scene columns on EACH side.
    unsigned width() const { return (geometry::native_width + border * 2) * scale; }
    unsigned height() const { return geometry::height * scale; }
    bool expanded() const { return scale != 1 || border != 0; }
};

class GameVideo {
public:
    explicit GameVideo(Machine &machine, GameVideoMode mode = GameVideoMode::Diagnostic,
                       GameVideoOptions options = {});
    ~GameVideo();
    GameVideo(const GameVideo &) = delete;
    GameVideo &operator=(const GameVideo &) = delete;
    void reset();
    void render_frame();
    void compare_layers(uint64_t frame, unsigned layer_mask);
    void report(std::ostream &output) const;
    std::span<const uint32_t> presentation() const;
    // Host-only presentation snapshot. Native observations remain CPU-produced.
    void enable_gpu_presentation(bool enabled = true);
    // Host-only GPU/reference geometry; constructor presentation/state stay fixed.
    void set_gpu_scale(unsigned scale);
    // The one scanout snapshot, overwritten at each VBSTART capture.
    const CapturedFrame &captured_frame() const;
    void render_reference(std::span<uint32_t> output, GameVideoOptions options,
                          unsigned layer_mask = 511, bool serial = false) const;
    size_t state_size() const;
    void save_state(std::span<uint8_t> dst) const;
    void load_state(std::span<const uint8_t> src);
private:
    friend class Machine;
    void materialize_native() const;
    void latch_sprites();
    void render();
    void compare_composite(uint64_t frame);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace f3rt
