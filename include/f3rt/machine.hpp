#pragma once
#include "f3rt/cpu_abi.h"
#include "f3rt/rom.hpp"
#include <array>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <vector>

namespace f3rt {
class Video;
class GameVideo;
class Audio;
class Interpreter;
class Eeprom;
class SoundTrace;
class SoundNative;
class Machine {
public:
    static constexpr uint32_t main_clock = 16000000;
    static constexpr uint32_t pixel_clock = 6671500;
    static constexpr uint32_t frame_pixels = 432 * 262;
    explicit Machine(RomSet roms);
    ~Machine();
    Machine(const Machine &) = delete;
    Machine &operator=(const Machine &) = delete;
    f3_cpu cpu{};
    std::array<uint8_t, 0x20000> ram{};
    std::array<uint8_t, 0x8000> palette{};
    std::array<uint8_t, 0x40000> graphics{};
    std::array<uint8_t, 0x20> control{};
    std::array<uint8_t, 0x800> shared{};
    std::array<uint32_t, 320 * 232> pixels{};
    RomSet roms;
    std::unique_ptr<Video> video;
    std::unique_ptr<GameVideo> game_video;
    std::unique_ptr<Audio> audio;
    std::unique_ptr<Eeprom> eeprom;
    std::unique_ptr<Interpreter> interpreter;
    std::unique_ptr<SoundTrace> sound_trace;
    std::unique_ptr<SoundNative> sound_native;
    std::array<uint32_t, 6> inputs{0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff, 0xffffffff};
    uint8_t system_inputs = 0xff; // EEPROMIN: service/test, four coins, active low.
    std::array<uint64_t, 4> coin_count{};
    std::array<bool, 4> coin_locked{};
    std::array<uint16_t, 2> coin_word{};
    uint16_t timer_control = 0;
    uint64_t frame = 0;
    uint8_t pending_irqs = 0;
    const f3_block *blocks = nullptr;
    size_t block_count = 0;
    std::span<const f3_excluded_range> excluded_code;
    uint64_t native_blocks = 0, fallback_instructions = 0;
    bool allow_main_fallback = true; // Native game target disables this; diagnostics opt in.
    // Optional PC coverage counter, allocated only when requested by tooling.
    std::vector<uint32_t> fallback_hits;

    void reset();
    void reset_devices();
    // Select before advancing the machine. The interpreted driver is the default.
    void use_native_sound(const f3_block *program, size_t count,
                          std::span<const f3_excluded_range> excluded);
    uint32_t sound_pc() const;
    bool run_frame(bool translated = false);
    void advance_to(uint64_t cycles);
    int boundary();
    int fallback();
    uint8_t read8(uint32_t address);
    uint16_t read16(uint32_t address);
    uint32_t read32(uint32_t address);
    void write8(uint32_t address, uint8_t value);
    void write16(uint32_t address, uint16_t value);
    void write32(uint32_t address, uint32_t value);
    void load_eeprom(const std::filesystem::path &path);
    void save_eeprom(const std::filesystem::path &path) const;
    void set_input(unsigned port, uint32_t mask, bool pressed);
    size_t state_size() const;
    void save_state(std::span<uint8_t> dst) const;
    void load_state(std::span<const uint8_t> src);
    uint32_t state_crc() const;
private:
    uint64_t hardware_cycles = 0;
    uint64_t next_vblank = 0, irq3_at = UINT64_MAX;
    uint64_t watchdog_at = 0;
    uint64_t raster_cycle(uint64_t pixels) const;
    uint32_t input_word(unsigned index) const;
    void coin_write(unsigned bank, uint8_t value);
    mutable std::vector<uint8_t> state_scratch_;
};
}
