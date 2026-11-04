#pragma once
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <memory>

namespace f3rt {

// A process-wide unique tag for one content of a decoded component (never 0). Components copied into
// a capture compare tags to skip re-copying content the destination already holds.
inline uint64_t fresh_content_version() {
    static std::atomic<uint64_t> next{1};
    return next.fetch_add(1, std::memory_order_relaxed);
}

// Raw copy of one video RAM region, so a component can skip decoding a region the game left alone
// since the last decode. The game rewrites most regions every frame with the values they already
// hold, which a write log would count as a change and a content comparison does not. A copy of the
// owning component starts without a shadow (it would never be refreshed there).
template <size_t N> class RamShadow {
public:
    RamShadow() = default;
    RamShadow(const RamShadow &) {}
    RamShadow &operator=(const RamShadow &) {
        m_bytes.reset();
        return *this;
    }

    // True when `source` (N bytes) differs from the bytes seen by the previous call, or when there is
    // no previous call; the shadow then holds `source`.
    bool changed(const uint8_t *source) {
        if (m_bytes) {
            if (std::memcmp(m_bytes->data(), source, N) == 0) return false;
        } else m_bytes = std::make_unique<std::array<uint8_t, N>>();
        std::memcpy(m_bytes->data(), source, N);
        return true;
    }
    // The decoded state no longer matches the shadow: the next changed() reports true.
    void invalidate() { m_bytes.reset(); }

private:
    std::unique_ptr<std::array<uint8_t, N>> m_bytes;
};

} // namespace f3rt
