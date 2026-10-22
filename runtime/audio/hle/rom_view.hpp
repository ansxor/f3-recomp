#pragma once
#include "f3rt/memory_map.hpp"
#include <cstdint>
#include <span>
#include <stdexcept>

namespace f3rt::hle {

// ROM bus addresses & banking offsets
inline constexpr uint32_t BANKED_WINDOW_BASE = 0xc20000;
inline constexpr uint32_t BANKED_WINDOW_END = 0xc40000;
inline constexpr uint32_t BANKED_OVERFLOW_BASE = 0xc40000;
inline constexpr uint32_t BANK_PAGE_SIZE = 0x20000;
inline constexpr uint32_t BANK_OVERFLOW_OFFSET = 0x40000;
inline constexpr uint32_t SOUND_ROM_IMAGE_SIZE = 0x80000;

class RomView {
public:
    constexpr RomView() noexcept = default;
    constexpr RomView(std::span<const uint8_t> image, uint8_t bank = 1) noexcept
        : m_rom(image), m_bank(bank) {}

    void set_bank(uint8_t bank) noexcept { m_bank = bank; }
    const uint8_t *data() const noexcept { return m_rom.data(); }
    size_t size() const noexcept { return m_rom.size(); }

    uint32_t resolve_offset(uint32_t address) const {
        if (address < SOUND_ROM_BASE || address >= SOUND_ROM_BASE + SOUND_ROM_IMAGE_SIZE) {
            throw std::runtime_error("HLE ROM read outside image");
        }
        uint32_t off = address - SOUND_ROM_BASE;
        if (m_rom.size() > SOUND_ROM_SIZE) {
            if (address >= BANKED_WINDOW_BASE && address < BANKED_WINDOW_END) {
                off = uint32_t(m_bank) * BANK_PAGE_SIZE + (address - BANKED_WINDOW_BASE);
            } else if (address >= BANKED_OVERFLOW_BASE) {
                off = BANK_OVERFLOW_OFFSET + (address - BANKED_OVERFLOW_BASE);
            }
        }
        if (off >= m_rom.size()) {
            throw std::runtime_error("HLE ROM read outside image");
        }
        return off;
    }

    uint8_t read_byte(uint32_t address) const { return m_rom[resolve_offset(address)]; }

    uint16_t read_u16(uint32_t address) const {
        const uint32_t off = resolve_offset(address);
        if (off + 1 >= m_rom.size()) throw std::runtime_error("HLE ROM read outside image");
        return uint16_t((uint16_t(m_rom[off]) << 8) | m_rom[off + 1]);
    }

    uint32_t read_u32(uint32_t address) const {
        const uint32_t off = resolve_offset(address);
        if (off + 3 >= m_rom.size()) throw std::runtime_error("HLE ROM read outside image");
        return (uint32_t(m_rom[off]) << 24) | (uint32_t(m_rom[off + 1]) << 16) |
               (uint32_t(m_rom[off + 2]) << 8) | uint32_t(m_rom[off + 3]);
    }

    const uint8_t *pointer(uint32_t address, size_t bytes = 1) const {
        const uint32_t off = resolve_offset(address);
        if (off + bytes > m_rom.size())
            throw std::runtime_error("HLE descriptor exceeds sound ROM");
        return m_rom.data() + off;
    }

    uint8_t operator[](size_t offset) const noexcept {
        return offset < m_rom.size() ? m_rom[offset] : 0xff;
    }

private:
    std::span<const uint8_t> m_rom{};
    uint8_t m_bank = 1;
};

} // namespace f3rt::hle
