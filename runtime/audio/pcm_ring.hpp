#pragma once
#include "audio/hle/core.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <mutex>

namespace f3rt {

class PcmRing final : public hle::PcmSink {
public:
    static constexpr size_t CAPACITY = 32768;

    void write(const float *stereo, size_t frames) override {
        std::lock_guard lock(m_mutex);
        for (size_t i = 0; i < frames; ++i) {
            const size_t pos = size_t(m_write % CAPACITY) * 2;
            m_frames[pos] = stereo[i * 2];
            m_frames[pos + 1] = stereo[i * 2 + 1];
            ++m_write;
        }
        if (m_write - m_read > CAPACITY) m_read = m_write - CAPACITY;
    }

    [[nodiscard]] size_t available() const {
        std::lock_guard lock(m_mutex);
        return size_t(m_write - m_read);
    }

    template <class Sample>
        requires std::same_as<Sample, float> || std::same_as<Sample, int16_t>
    [[nodiscard]] size_t drain(Sample *out, size_t max_frames) {
        return take<false>(out, max_frames);
    }

    [[nodiscard]] size_t drain_released(int16_t *out, size_t max_frames) {
        return take<true>(out, max_frames);
    }

    void close_frame(uint64_t frame_end) noexcept {
        m_released.store(m_pending, std::memory_order_release);
        m_pending = frame_end;
    }

    void discard() {
        std::lock_guard lock(m_mutex);
        m_read = m_pending = m_write;
        m_released.store(m_write, std::memory_order_release);
    }

private:
    template <bool Released, class Sample> size_t take(Sample *out, size_t max_frames) {
        std::lock_guard lock(m_mutex);
        uint64_t ready = m_write - m_read;
        if constexpr (Released) {
            const uint64_t released = m_released.load(std::memory_order_acquire);
            ready = std::min(ready, released > m_read ? released - m_read : 0);
        }
        const size_t count = size_t(std::min<uint64_t>(max_frames, ready));
        for (size_t n = 0; n < count; ++n) {
            for (unsigned c = 0; c < 2; ++c) {
                const float value = m_frames[(m_read % CAPACITY) * 2 + c];
                if constexpr (std::same_as<Sample, float>) out[n * 2 + (c ^ 1)] = value;
                else out[n * 2 + (c ^ 1)] = int16_t(std::clamp(value * 32768.f, -32768.f, 32767.f));
            }
            ++m_read;
        }
        return count;
    }

    mutable std::mutex m_mutex;
    std::array<float, CAPACITY * 2> m_frames{};
    uint64_t m_read = 0, m_write = 0;
    std::atomic<uint64_t> m_released{0};
    uint64_t m_pending = 0;
};

} // namespace f3rt
