#pragma once
#include <cstdint>
#include <iosfwd>
#include <memory>
#include <span>

namespace f3rt {
class Machine;
struct GpuScene;
enum class GameVideoMode { Diagnostic, Game, Compare };

struct GameVideoOptions {
    static constexpr unsigned max_scale = 4, max_gpu_scale = 8, max_border = 160;
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
#ifdef F3RT_VIDEO_WRITE_LOG
    // Debug builds only: Machine::write8 reports graphics/control stores so
    // stores from undocumented game routines are logged once per PC.
    void observe_write(uint32_t pc, uint32_t address);
#endif
    void render_frame();
    void compare_layers(uint64_t frame, unsigned layer_mask);
    void report(std::ostream &output) const;
    std::span<const uint32_t> presentation() const;
    // Host-only presentation snapshot. Native observations remain CPU-produced.
    void enable_gpu_presentation(bool enabled = true);
    // Host-only GPU/reference geometry; constructor presentation/state stay fixed.
    void set_gpu_scale(unsigned scale);
    const GpuScene &gpu_scene() const;
    void render_reference(std::span<uint32_t> output, GameVideoOptions options,
                          unsigned layer_mask = 511, bool serial = false) const;
    size_t state_size() const;
    void save_state(std::span<uint8_t> dst) const;
    void load_state(std::span<const uint8_t> src);
    size_t sync_state_size() const;
    void save_sync_state(std::span<uint8_t> dst) const;
    void load_sync_state(std::span<const uint8_t> src);
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
