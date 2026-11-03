#pragma once
// Bit tricks shared by the indexed compositor and the sprite raster: column bitmasks over a row (bit
// c = column c, in 32-bit words with one guard word after the last) and a 16-texel tile row read
// straight from its pen bytes.
#include <array>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>

namespace f3rt {
static_assert(std::endian::native == std::endian::little, "pen byte order assumes little endian");

// Bits [x, x + len) of a column bitmask as bit i = column x + i. len <= 16, so at most two words.
[[gnu::always_inline]] inline uint32_t extract_columns(const uint32_t *words, int x, unsigned len) {
    const unsigned word = unsigned(x) >> 5, shift = unsigned(x) & 31;
    uint32_t bits = words[word] >> shift;
    if (shift + len > 32) bits |= words[word + 1] << (32 - shift);
    return bits & ((1u << len) - 1);
}

// Clear the columns of `bits` (bit i = column x + i, at most 16 wide) in a column bitmask.
[[gnu::always_inline]] inline void clear_columns(uint32_t *words, int x, uint32_t bits) {
    const unsigned word = unsigned(x) >> 5, shift = unsigned(x) & 31;
    words[word] &= ~(bits << shift);
    if (shift > 16) words[word + 1] &= ~(bits >> (32 - shift));
}

// Set the columns of `bits` (bit i = column x + i, at most 16 wide) in a column bitmask.
[[gnu::always_inline]] inline void set_columns(uint32_t *words, int x, uint32_t bits) {
    const unsigned word = unsigned(x) >> 5, shift = unsigned(x) & 31;
    words[word] |= bits << shift;
    if (shift > 16) words[word + 1] |= bits >> (32 - shift);
}

// Calls body(first, length) for each run of consecutive set bits of `bits`, lowest first.
template <class Body> [[gnu::always_inline]] inline void for_each_run(uint32_t bits, Body &&body) {
    while (bits) {
        const unsigned first = unsigned(__builtin_ctz(bits));
        const uint32_t shifted = ~(bits >> first);
        body(first, shifted ? unsigned(__builtin_ctz(shifted)) : 32 - first);
        // Adding the run's lowest bit carries through the whole run and clears it.
        bits &= bits + (1u << first);
    }
}

// The 16 pens of a tile row with the pen mask (at most 0x3f) applied, held as four little-endian
// words: pen i is byte i % 4 of word i / 4. A masked pen is at most 0x3f, so adding 0x7f to its
// byte sets bit 7 exactly when the pen is nonzero and never carries into the next byte.
class PenRow {
public:
    // Tile ROMs are word aligned (a row is 16 bytes).
    PenRow(const uint8_t *row, uint8_t pen_mask) {
        const uint32_t mask = pen_mask * 0x01010101u;
        for (unsigned i = 0; i < 4; ++i) {
            uint32_t word;
            std::memcpy(&word, std::assume_aligned<4>(row + 4 * i), sizeof word);
            m_words[i] = word & mask;
        }
    }

    // Pens of screen columns 4 * group .. 4 * group + 3 as bytes 0..3 (the row read backwards when
    // `mirrored`).
    uint32_t group(unsigned group, bool mirrored) const {
        return mirrored ? __builtin_bswap32(m_words[3 - group]) : m_words[group];
    }

    // Whether every pen is zero.
    bool blank() const { return !(m_words[0] | m_words[1] | m_words[2] | m_words[3]); }

    // Whether every pen is nonzero.
    bool all_opaque() const {
        return (nonzero(m_words[0]) & nonzero(m_words[1]) & nonzero(m_words[2]) &
                nonzero(m_words[3])) == 0x80808080u;
    }

    // The nonzero pens as a 16-bit mask: bit i is screen column i. Multiplying the byte flags by
    // the constant lines flag k up at bit 24 + k of the product; no two partial products meet, so
    // nothing carries into those four bits.
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
