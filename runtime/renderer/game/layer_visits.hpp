#pragma once
#include "renderer/game/scene.hpp"
#include <cstdint>

namespace f3rt {

struct LayerPass {
    uint32_t first_bit, repeat_bit;
};

class LayerVisits {
public:
    static constexpr LayerPass pass(unsigned layer, bool select) {
        return {1u << (layer * 2 + unsigned(select)), 1u << (REPEAT_SHIFT + layer)};
    }

    constexpr LayerVisits() = default;
    constexpr explicit LayerVisits(LayerPass first) : m_bits(first.first_bit) {}

    constexpr void add(LayerPass next) {
        m_bits |= (m_bits & next.first_bit) ? next.repeat_bit : next.first_bit;
    }

    constexpr bool saw(unsigned layer, bool select) const {
        return (m_bits >> (layer * 2 + unsigned(select))) & 1u;
    }

    constexpr unsigned passes(unsigned layer) const {
        return 1 + ((m_bits >> (REPEAT_SHIFT + layer)) & 1u);
    }

    constexpr bool empty() const { return m_bits == 0; }
    constexpr uint32_t hash() const { return m_bits * 0x9e3779b1u; }
    constexpr bool operator==(const LayerVisits &) const = default;

private:
    static constexpr unsigned REPEAT_SHIFT = 2 * LAYER_COUNT;
    static_assert(REPEAT_SHIFT + LAYER_COUNT <= 32);
    uint32_t m_bits = 0;
};

} // namespace f3rt
