#pragma once
#include <cstdint>
#include <iosfwd>
#include <memory>

namespace f3rt {
class Machine;
enum class GameVideoMode { Diagnostic, Game, Compare };

class GameVideo {
public:
    explicit GameVideo(Machine &machine, GameVideoMode mode = GameVideoMode::Diagnostic);
    ~GameVideo();
    GameVideo(const GameVideo &) = delete;
    GameVideo &operator=(const GameVideo &) = delete;
    void reset();
    void observe();
    void observe_write(uint32_t pc, uint32_t address);
    void render_frame();
    void compare_layers(uint64_t frame, unsigned layer_mask);
    void report(std::ostream &output) const;
private:
    void latch_sprites();
    void render();
    void compare_composite(uint64_t frame);
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace f3rt
