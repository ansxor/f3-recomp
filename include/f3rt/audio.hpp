#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace f3rt {
namespace hle { struct VoiceEvent; }

class Audio {
public:
    enum class GainModel { MameRouting, SingleStage };
    enum class Backend { Accurate, Hle };
    struct HleStats {
        uint64_t commands = 0, reused = 0, cancelled = 0, rendered_frames = 0;
    };
    // Select once, after ROM loading and before machine execution.
    void set_backend(Backend backend);
    Backend backend() const;
    void shared_write(uint32_t offset, uint64_t frame);
    void begin_frame(uint64_t frame);
    void finish_frame(uint64_t frame);
    void begin_rollback(uint64_t begin, uint64_t end);
    void end_rollback();
    HleStats hle_stats() const;
    // Diagnostic callback executes on the worker, never on the main CPU.
    void set_hle_observer(std::function<void(const hle::VoiceEvent &)> observer);
    Audio();
    ~Audio();

    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;
    Audio(Audio &&) noexcept;
    Audio &operator=(Audio &&) noexcept;

    // ROM and memory configuration
    // sound_rom: 0x80000 bytes 68000 program code (interleaved high/low byte pairs)
    void load_sound_rom(std::span<const uint8_t> sound_rom);
    // sample_rom: up to 16MB raw big-endian 16-bit sound samples
    void load_sample_rom(std::span<const uint8_t> sample_rom);
    // shared_ram: 0x800-byte shared dual-port RAM pointer owned by parent/main CPU bus
    void set_shared_ram(uint8_t *shared_ram, size_t size = 0x800);

    // CPU execution callback: parent provides runner wrapping Musashi context
    // returns number of cycles executed
    void set_cpu_runner(std::function<int(int cycles)> runner);

    // Reset control (active-high asserted = CPU held in reset)
    void set_reset(bool asserted);
    bool is_reset() const;
    void set_reset_callback(std::function<void(bool asserted)> cb);
    // Whole-machine/watchdog reset: hold CPU, reset DUART/DSP/volume, reload
    // boot vectors; preserve RAM, OTIS state, and the continuous audio clock.
    void reset_board();

    // Sound 68000 bus handlers (called by Musashi memory map bridge)
    uint8_t read8(uint32_t address);
    uint16_t read16(uint32_t address);
    uint32_t read32(uint32_t address);
    void write8(uint32_t address, uint8_t value);
    void write16(uint32_t address, uint16_t value);
    void write32(uint32_t address, uint32_t value);

    // Sound 68000 interrupt handling
    int irq_level() const;
    uint8_t irq_ack(int level);
    void set_irq_callback(std::function<void(bool asserted)> cb);

    // Time advancement driven by main CPU 16MHz cycles
    void advance(uint32_t main_cycles);
    // Continuous effective device time, not the caller's main-block endpoint.
    uint64_t clock_ticks() const;
    uint64_t generated_frames() const;

    // Audio output stream: interleaved stereo (Left, Right)
    // Accurate: ES5505 native rate; HLE: 48000 Hz.
    uint32_t sample_rate() const;
    size_t available_frames() const;
    size_t render(int16_t *interleaved_stereo, size_t max_frames);
    size_t render(float *interleaved_stereo, size_t max_frames);
    // Default matches the observed MAME board mix; SingleStage is an explicit
    // unverified analog-gain experiment, not an asserted board schematic.
    void set_gain_model(GainModel model);

    // State snapshot serialization
    size_t state_size() const;
    void save_state(std::span<uint8_t> dst) const;
    void load_state(std::span<const uint8_t> src);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace f3rt
