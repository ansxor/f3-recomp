#pragma once

#include <bit>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>

namespace f3rt {

/// Read an unaligned big-endian integer of type T from a raw byte pointer.
template <std::integral T>
[[nodiscard]] constexpr T read_be(const void *p) noexcept {
    T val;
    std::memcpy(&val, p, sizeof(T));
    if constexpr (std::endian::native == std::endian::little) {
        return std::byteswap(val);
    }
    return val;
}

/// Read an unaligned big-endian integer of type T from a byte span at a byte offset.
template <std::integral T>
[[nodiscard]] constexpr T read_be(std::span<const uint8_t> bytes, size_t offset = 0) noexcept {
    return read_be<T>(bytes.data() + offset);
}

/// Convenience aliases for standard big-endian reads.
[[nodiscard]] inline uint16_t read_be16(const void *p) noexcept { return read_be<uint16_t>(p); }
[[nodiscard]] inline uint32_t read_be32(const void *p) noexcept { return read_be<uint32_t>(p); }
[[nodiscard]] inline uint64_t read_be64(const void *p) noexcept { return read_be<uint64_t>(p); }

[[nodiscard]] inline uint16_t read_be16(std::span<const uint8_t> bytes, size_t offset = 0) noexcept {
    return read_be<uint16_t>(bytes, offset);
}
[[nodiscard]] inline uint32_t read_be32(std::span<const uint8_t> bytes, size_t offset = 0) noexcept {
    return read_be<uint32_t>(bytes, offset);
}
[[nodiscard]] inline uint64_t read_be64(std::span<const uint8_t> bytes, size_t offset = 0) noexcept {
    return read_be<uint64_t>(bytes, offset);
}

/// Write an unaligned big-endian integer of type T to a raw byte pointer.
template <std::integral T>
constexpr void write_be(void *p, T val) noexcept {
    if constexpr (std::endian::native == std::endian::little) {
        val = std::byteswap(val);
    }
    std::memcpy(p, &val, sizeof(T));
}

/// Write an unaligned big-endian integer of type T to a mutable byte span at a byte offset.
template <std::integral T>
constexpr void write_be(std::span<uint8_t> bytes, size_t offset, T val) noexcept {
    write_be<T>(bytes.data() + offset, val);
}

[[nodiscard]] inline int16_t s16(uint32_t x) noexcept { return int16_t(uint16_t(x)); }
[[nodiscard]] inline int8_t s8(uint32_t x) noexcept { return int8_t(uint8_t(x)); }

} // namespace f3rt
