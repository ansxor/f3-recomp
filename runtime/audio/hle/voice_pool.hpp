#pragma once
#include <array>
#include <cstdint>

namespace f3rt::hle {

inline constexpr uint8_t MAX_PHYSICAL_CHANNEL = 56;

class PhysicalVoicePool {
public:
    // c128a8 allocates channels 56..1. Their previous-key byte survives
    // sequence destruction; portamento must not use logical-track history.
    void reset() noexcept {
        m_used.fill(false);
        m_previous_key.fill(0);
    }

    uint8_t allocate() noexcept {
        for (uint8_t ch = MAX_PHYSICAL_CHANNEL; ch > 0; --ch) {
            if (!m_used[ch]) {
                m_used[ch] = true;
                return ch;
            }
        }
        return 0;
    }

    void release(uint8_t ch) noexcept {
        if (ch > 0 && ch < m_used.size()) {
            m_used[ch] = false;
        }
    }

    uint8_t previous_key(uint8_t ch) const noexcept { return m_previous_key[ch]; }

    void set_previous_key(uint8_t ch, uint8_t key) noexcept { m_previous_key[ch] = key; }

private:
    std::array<bool, MAX_PHYSICAL_CHANNEL + 1> m_used{};
    std::array<uint8_t, MAX_PHYSICAL_CHANNEL + 1> m_previous_key{};
};

} // namespace f3rt::hle
