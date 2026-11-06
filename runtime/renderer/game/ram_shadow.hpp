#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>

namespace f3rt {

inline uint64_t fresh_content_version() {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

template <size_t N> class RamShadow {
public:
    RamShadow() = default;
    RamShadow(const RamShadow &) : RamShadow() {}
    RamShadow &operator=(const RamShadow &) {
        invalidate();
        return *this;
    }

    bool changed(const uint8_t *source) {
        if (m_bytes) {
            if (std::memcmp(m_bytes->data(), source, N) == 0) return false;
        } else m_bytes = std::make_unique<std::array<uint8_t, N>>();
        std::memcpy(m_bytes->data(), source, N);
        return true;
    }
    void invalidate() { m_bytes.reset(); }

private:
    std::unique_ptr<std::array<uint8_t, N>> m_bytes;
};

} // namespace f3rt
