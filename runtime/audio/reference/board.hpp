#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace f3rt::reference {

class Board {
public:
    Board(std::span<const uint8_t> sound_rom, std::span<const uint8_t> sample_rom,
          uint8_t *shared_ram, size_t shared_ram_size = 0x800);
    ~Board();

    Board(const Board &) = delete;
    Board &operator=(const Board &) = delete;
    Board(Board &&) noexcept;
    Board &operator=(Board &&) noexcept;

    void set_cpu_runner(std::function<int(int cycles)> runner);

    void set_reset(bool asserted);
    bool is_reset() const;
    void set_reset_callback(std::function<void(bool asserted)> cb);
    void reset_board();

    void set_sound_bank(uint32_t page);
    uint32_t sound_bank() const;

    uint8_t read8(uint32_t address);
    uint16_t read16(uint32_t address);
    uint32_t read32(uint32_t address);
    void write8(uint32_t address, uint8_t value);
    void write16(uint32_t address, uint16_t value);
    void write32(uint32_t address, uint32_t value);

    int irq_level() const;
    uint8_t irq_ack(int level);
    void set_irq_callback(std::function<void(bool asserted)> cb);

    void advance(uint32_t main_cycles);
    uint64_t clock_ticks() const;
    uint64_t generated_frames() const;

    uint32_t sample_rate() const;
    size_t available_frames() const;
    size_t render(int16_t *interleaved_stereo, size_t max_frames);
    size_t render(float *interleaved_stereo, size_t max_frames);
    size_t render_ready(int16_t *interleaved_stereo, size_t max_frames);

    size_t state_size() const;
    void save_state(std::span<uint8_t> dst) const;
    void load_state(std::span<const uint8_t> src);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace f3rt::reference
