#pragma once
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>

namespace f3rt {
static_assert(std::endian::native == std::endian::little, "pen byte order assumes little endian");

[[gnu::always_inline]] inline uint32_t extract_columns(const uint32_t *words, int x, unsigned len) {
    const unsigned word = unsigned(x) >> 5, shift = unsigned(x) & 31;
    uint32_t bits = words[word] >> shift;
    if (shift + len > 32) bits |= words[word + 1] << (32 - shift);
    return bits & ((1u << len) - 1);
}

[[gnu::always_inline]] inline void clear_columns(uint32_t *words, int x, uint32_t bits) {
    const unsigned word = unsigned(x) >> 5, shift = unsigned(x) & 31;
    words[word] &= ~(bits << shift);
    if (shift > 16) words[word + 1] &= ~(bits >> (32 - shift));
}

[[gnu::always_inline]] inline void set_columns(uint32_t *words, int x, uint32_t bits) {
    const unsigned word = unsigned(x) >> 5, shift = unsigned(x) & 31;
    words[word] |= bits << shift;
    if (shift > 16) words[word + 1] |= bits >> (32 - shift);
}

template <class Body> [[gnu::always_inline]] inline void for_each_run(uint32_t bits, Body &&body) {
    while (bits) {
        const unsigned first = unsigned(__builtin_ctz(bits));
        const uint32_t shifted = ~(bits >> first);
        body(first, shifted ? unsigned(__builtin_ctz(shifted)) : 32 - first);
        bits &= bits + (1u << first);
    }
}

[[gnu::always_inline]] inline uint32_t glyph_row_pens(const uint8_t *row, bool mirrored) {
    uint32_t word;
    std::memcpy(&word, row, sizeof word);
    if (!mirrored) return __builtin_bswap32(word);
    return (word >> 4 & 0x0f0f0f0fu) | (word & 0x0f0f0f0fu) << 4;
}

[[gnu::always_inline]] inline uint32_t nonzero_nibbles(uint32_t pens) {
    uint32_t flags = pens | pens >> 1;
    flags = (flags | flags >> 2) & 0x11111111u; // bit 4k: nibble k is nonzero
    flags = (flags | flags >> 3) & 0x03030303u;
    flags = (flags | flags >> 6) & 0x000f000fu;
    return (flags | flags >> 12) & 0xffu;
}

class PenRow {
public:
    PenRow(const uint8_t *row, uint8_t pen_mask) {
        const uint32_t mask = pen_mask * 0x01010101u;
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t word;
            std::memcpy(&word, std::assume_aligned<4>(row + 4 * i), sizeof word);
            m_words[i] = word & mask;
        }
    }

    uint32_t group(unsigned group, bool mirrored) const {
        return mirrored ? __builtin_bswap32(m_words[3 - group]) : m_words[group];
    }

    bool blank() const { return !(m_words[0] | m_words[1] | m_words[2] | m_words[3]); }

    bool all_opaque() const {
        return (nonzero(m_words[0]) & nonzero(m_words[1]) & nonzero(m_words[2]) &
                nonzero(m_words[3])) == 0x80808080u;
    }

    uint32_t opaque(bool mirrored) const {
        const auto gather = [this](unsigned group, bool mirrored) {
            const uint32_t flags = (m_words[group] + 0x7f7f7f7fu) >> 7 & 0x01010101u;
            return (flags * (mirrored ? 0x08040201u : 0x01020408u)) >> 24;
        };
        if (!mirrored)
            return gather(0, false) | gather(1, false) << 4 | gather(2, false) << 8 |
                   gather(3, false) << 12;
        return gather(3, true) | gather(2, true) << 4 | gather(1, true) << 8 |
               gather(0, true) << 12;
    }

private:
    static uint32_t nonzero(uint32_t word) { return (word + 0x7f7f7f7fu) & 0x80808080u; }

    std::array<uint32_t, 4> m_words;
};
} // namespace f3rt
