#pragma once
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <span>

namespace f3rt {
class Machine;
enum class GameVideoMode { Diagnostic, Game, Compare };

struct GameVideoOptions {
    static constexpr unsigned max_scale = 4, max_border = 160;
    unsigned scale = 1;
    unsigned border = 0; // Additional native scene columns on EACH side.
    unsigned width() const { return (320 + border * 2) * scale; }
    unsigned height() const { return 232 * scale; }
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
    void observe();
    void observe_write(uint32_t pc, uint32_t address);
    void render_frame();
    void compare_layers(uint64_t frame, unsigned layer_mask);
    void report(std::ostream &output) const;
    std::span<const uint32_t> presentation() const;
    size_t state_size() const;
    void save_state(std::span<uint8_t> dst) const;
    void load_state(std::span<const uint8_t> src);
private:
    void latch_sprites();
    void render();
    void compare_composite(uint64_t frame);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace f3rt
