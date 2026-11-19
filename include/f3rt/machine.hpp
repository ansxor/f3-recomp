#pragma once
#include "f3rt/cpu_abi.h"
#include "f3rt/memory_map.hpp"
#include "f3rt/rom.hpp"
#include "f3rt/renderer/graphics_assets.hpp"
#include <array>
#ifdef F3RT_TRACE_STACKED_SR
#include <bitset>
#endif
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <vector>

namespace f3rt {
class GameVideo;
class SpriteSystem;
class Machine {
public:
    // Device time is counted in 16 MHz main-clock ticks. It advances only at
    // device events (vblank, IRQ3), never per CPU instruction: the main CPU is
    // modelled as infinitely fast and runs until it waits for an interrupt.
    static constexpr uint32_t MAIN_CLOCK = 16000000;
    static constexpr uint32_t PIXEL_CLOCK = 6671500;
    static constexpr uint32_t FRAME_PIXELS = 432 * 262;
    static constexpr uint64_t IRQ3_DELAY = 10000; // MAME's level 3 delay after vblank.
    // Instructions the CPU may run between device events before the scheduler
    // forces the next one anyway: the backstop for wait loops recomp/idle.py
    // does not recognise, and for code that never waits. A burst of real work
    // must finish inside one slice, because an event landing mid-burst can split
    // work that hardware timing keeps whole: landmakrj's vblank task drops the
    // EEPROM chip select, and its write-then-verify burst runs ~150k steps once
    // programming is instant. An unrecognised wait loop (commandw's attract loop)
    // spins the whole slice every frame, ~1 ms of host time at this size.
    static constexpr uint64_t SLICE_STEPS = 200000;
    explicit Machine(RomSet roms);
    ~Machine();
    Machine(const Machine &) = delete;
    Machine &operator=(const Machine &) = delete;
    f3_cpu cpu{};
    alignas(8) std::array<uint8_t, 0x20000> ram{};
    std::array<uint8_t, 0x8000> palette{};
    std::array<uint8_t, 0x40000> graphics{};
    std::array<uint8_t, 0x20> control{};
    std::array<uint8_t, 0x800> shared{};

private:
    friend class GameVideo;
    mutable std::vector<uint32_t> m_pixels;

public:
    // Observing native scanout materializes a retained GPU-presented frame.
    const std::vector<uint32_t> &native_pixels() const;
    const std::array<uint32_t, 7> &rom_region_crcs() const { return m_rom_crcs; }
    RomSet roms;
    GraphicsAssets graphics_assets;
    // Tool-only scanout at VBSTART, when no GameVideo is installed.
    std::function<void(std::span<uint32_t>)> inspect_scanout;
    // Render-only sprite group and behaviour tracking; null = feature off. Never serialized.
    std::unique_ptr<SpriteSystem> sprites;
    // Host-only per-slot writer attribution (one PC per 16-byte sprite RAM entry); null = off. Never serialized.
    std::unique_ptr<std::array<uint32_t, 0x1000>> sprite_writers;
    std::unique_ptr<GameVideo> game_video;
    bool sound_bankswitch = false;
    std::array<uint32_t, 6> inputs{0xffffffff, 0xffffffff, 0xffffffff,
                                   0xffffffff, 0xffffffff, 0xffffffff};
    uint8_t system_inputs = 0xff; // EEPROMIN: service/test, four coins, active low.
    std::array<uint64_t, 4> coin_count{};
    std::array<bool, 4> coin_locked{};
    std::array<uint16_t, 2> coin_word{};
    uint16_t timer_control = 0;
    uint64_t frame = 0;
    uint8_t pending_irqs = 0;
    const f3_block *blocks = nullptr;
    size_t block_count = 0;
    // Derived from the validated immutable registration, never serialized.
    // A 4 KiB ROM page has at most 2048 distinct aligned entries. Dense runs
    // use arithmetic lookup; sparse runs search only their own page.
    struct NativePage {
        const f3_block *first = nullptr;
        uint16_t count = 0;
        bool dense = false;
        uint32_t address = 0;
    };
    std::array<NativePage, 0x200000 / 0x1000> native_pages{};
    std::span<const f3_excluded_range> excluded_code;
    std::span<const f3_ram_code_range> ram_code;
    uint64_t native_blocks = 0;

    void reset();
    void bind_cpu_windows();
    void reset_devices();
    void reset_main_cpu();
    bool run_frame();
    int boundary();
    // An idle loop is waiting for an interrupt: end the slice at the next boundary.
    void idle();
    int fallback();
    uint8_t read8(uint32_t address);
    uint16_t read16(uint32_t address);
    uint32_t read32(uint32_t address);
    void write8(uint32_t address, uint8_t value);
    void write16(uint32_t address, uint16_t value);
    void write32(uint32_t address, uint32_t value);
    void write_stacked_sr(uint32_t address, uint16_t value);
#ifdef F3RT_TRACE_STACKED_SR
    // Indexed by the work RAM offset of the big-endian SR's low byte.
    std::bitset<RAM_MIRROR_MASK + 1> stacked_sr_low_bytes;

private:
    void invalidate_stacked_sr(uint32_t address, unsigned width);

public:
#endif
    void load_eeprom(const std::filesystem::path &path);
    void save_eeprom(const std::filesystem::path &path) const;
    void set_input(unsigned port, uint32_t mask, bool pressed);
    size_t state_size() const;
    void save_state(std::span<uint8_t> dst) const;
    void load_state(std::span<const uint8_t> src);
    uint32_t state_crc() const;

private:
    // The audio board and the EEPROM are process-wide instances behind include/f3rt/abi/{audio,eeprom}.h.
    // A lease releases its instance when the Machine dies, also when the constructor throws part way.
    // Declared after `shared`, so they are destroyed first: the audio worker borrows it until shutdown.
    struct AudioLease {
        bool held = false;
        AudioLease() = default;
        AudioLease(const AudioLease &) = delete;
        AudioLease &operator=(const AudioLease &) = delete;
        ~AudioLease();
    };
    struct EepromLease {
        bool held = false;
        EepromLease() = default;
        EepromLease(const EepromLease &) = delete;
        EepromLease &operator=(const EepromLease &) = delete;
        ~EepromLease();
    };
    AudioLease m_audio_lease;
    EepromLease m_eeprom_lease;
    uint64_t m_time = 0;
    std::array<uint32_t, 7> m_rom_crcs{};
    uint64_t m_next_vblank = 0, m_irq3_at = UINT64_MAX;
    uint64_t m_watchdog_at = 0;
    uint64_t m_slice_end = 0;
    bool m_idle = false;
    uint64_t raster_time(uint64_t pixels) const;
    void next_event();
    uint32_t input_word(unsigned index) const;
    void coin_write(unsigned bank, uint8_t value);
    mutable std::vector<uint8_t> m_state_scratch;
};
} // namespace f3rt
