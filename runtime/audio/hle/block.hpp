#pragma once

#include <algorithm>
#include <array>
#include <concepts>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <type_traits>
#include <vector>

namespace f3rt::hle {

inline constexpr size_t BLOCK_FRAMES = 256;

// Physical PCB wiring from OTIS (ES5505) to ESP (ES5510) and DAC
enum class OtisBus : size_t {
    NcL = 0,
    NcR = 1,
    DryL = 2,
    DryR = 3,
    DelayL = 4,
    DelayR = 5,
    ReverbL = 6,
    ReverbR = 7,
};

enum class StereoChannel : size_t {
    Left = 0,
    Right = 1,
};

enum class BusSelect : uint8_t {
    All,
    Dry,
    Delay,
    Reverb,
};

constexpr std::optional<BusSelect> otis_target_bus(uint8_t pair) noexcept {
    switch (pair) {
    case 1:
        return BusSelect::Dry;
    case 2:
        return BusSelect::Delay;
    case 3:
        return BusSelect::Reverb;
    default:
        return std::nullopt;
    }
}

struct BusSlice;
struct MutableBusSlice;
template <size_t Channels, size_t MaxFrames, typename T> struct alignas(64) PlanarAudioBlock;

// Non-owning read-only 2D view over interleaved frame data
template <size_t Channels> class AudioBlockSpan {
public:
    static constexpr size_t K_CHANNELS = Channels;

    constexpr AudioBlockSpan() noexcept = default;
    constexpr AudioBlockSpan(const float *data, size_t frames) noexcept
        : m_data(data), m_frames(frames) {}
    template <typename Alloc>
    explicit constexpr AudioBlockSpan(const std::vector<float, Alloc> &vec) noexcept
        : m_data(vec.data()), m_frames(vec.size() / Channels) {}

    constexpr float operator[](size_t f, size_t ch) const noexcept {
        return m_data[f * Channels + ch];
    }

    template <typename Enum>
        requires std::is_enum_v<Enum>
    constexpr float operator[](size_t f, Enum ch) const noexcept {
        return m_data[f * Channels + static_cast<size_t>(ch)];
    }

    constexpr std::span<const float, Channels> operator[](size_t f) const noexcept {
        return std::span<const float, Channels>(&m_data[f * Channels], Channels);
    }

    constexpr const float *data() const noexcept { return m_data; }
    constexpr size_t size() const noexcept { return m_frames; }
    constexpr bool empty() const noexcept { return m_frames == 0; }

    constexpr AudioBlockSpan subspan(size_t offset,
                                     size_t count = std::dynamic_extent) const noexcept {
        const size_t clamped_offset = std::min(offset, m_frames);
        const size_t clamped_count = (count == std::dynamic_extent)
                                         ? (m_frames - clamped_offset)
                                         : std::min(count, m_frames - clamped_offset);
        return AudioBlockSpan(m_data + clamped_offset * Channels, clamped_count);
    }

    constexpr BusSlice dry() const noexcept
        requires(Channels == 8);
    constexpr BusSlice delay() const noexcept
        requires(Channels == 8);
    constexpr BusSlice reverb() const noexcept
        requires(Channels == 8);
    constexpr BusSlice slice(BusSelect sel) const noexcept
        requires(Channels == 8);

private:
    const float *m_data = nullptr;
    size_t m_frames = 0;
};

// Non-owning mutable 2D view over interleaved frame data
template <size_t Channels> class MutableAudioBlockSpan {
public:
    static constexpr size_t K_CHANNELS = Channels;

    constexpr MutableAudioBlockSpan() noexcept = default;
    constexpr MutableAudioBlockSpan(float *data, size_t frames) noexcept
        : m_data(data), m_frames(frames) {}
    template <typename Alloc>
    explicit constexpr MutableAudioBlockSpan(std::vector<float, Alloc> &vec) noexcept
        : m_data(vec.data()), m_frames(vec.size() / Channels) {}

    constexpr float &operator[](size_t f, size_t ch) noexcept { return m_data[f * Channels + ch]; }
    constexpr const float &operator[](size_t f, size_t ch) const noexcept {
        return m_data[f * Channels + ch];
    }

    template <typename Enum>
        requires std::is_enum_v<Enum>
    constexpr float &operator[](size_t f, Enum ch) noexcept {
        return m_data[f * Channels + static_cast<size_t>(ch)];
    }
    template <typename Enum>
        requires std::is_enum_v<Enum>
    constexpr const float &operator[](size_t f, Enum ch) const noexcept {
        return m_data[f * Channels + static_cast<size_t>(ch)];
    }

    constexpr std::span<float, Channels> operator[](size_t f) noexcept {
        return std::span<float, Channels>(&m_data[f * Channels], Channels);
    }
    constexpr std::span<const float, Channels> operator[](size_t f) const noexcept {
        return std::span<const float, Channels>(&m_data[f * Channels], Channels);
    }

    constexpr float *data() noexcept { return m_data; }
    constexpr const float *data() const noexcept { return m_data; }
    constexpr size_t size() const noexcept { return m_frames; }
    constexpr bool empty() const noexcept { return m_frames == 0; }

    constexpr MutableAudioBlockSpan subspan(size_t offset,
                                            size_t count = std::dynamic_extent) noexcept {
        const size_t clamped_offset = std::min(offset, m_frames);
        const size_t clamped_count = (count == std::dynamic_extent)
                                         ? (m_frames - clamped_offset)
                                         : std::min(count, m_frames - clamped_offset);
        return MutableAudioBlockSpan(m_data + clamped_offset * Channels, clamped_count);
    }
    constexpr AudioBlockSpan<Channels> subspan(size_t offset,
                                               size_t count = std::dynamic_extent) const noexcept {
        const size_t clamped_offset = std::min(offset, m_frames);
        const size_t clamped_count = (count == std::dynamic_extent)
                                         ? (m_frames - clamped_offset)
                                         : std::min(count, m_frames - clamped_offset);
        return AudioBlockSpan<Channels>(m_data + clamped_offset * Channels, clamped_count);
    }

    constexpr operator AudioBlockSpan<Channels>() const noexcept {
        return AudioBlockSpan<Channels>(m_data, m_frames);
    }

    constexpr MutableBusSlice dry() noexcept
        requires(Channels == 8);
    constexpr MutableBusSlice delay() noexcept
        requires(Channels == 8);
    constexpr MutableBusSlice reverb() noexcept
        requires(Channels == 8);
    constexpr MutableBusSlice slice(BusSelect sel) noexcept
        requires(Channels == 8);

    constexpr BusSlice dry() const noexcept
        requires(Channels == 8);
    constexpr BusSlice delay() const noexcept
        requires(Channels == 8);
    constexpr BusSlice reverb() const noexcept
        requires(Channels == 8);
    constexpr BusSlice slice(BusSelect sel) const noexcept
        requires(Channels == 8);

private:
    float *m_data = nullptr;
    size_t m_frames = 0;
};

// 64-byte aligned stack/heap storage owning interleaved frames
template <size_t Channels, size_t MaxFrames = BLOCK_FRAMES> struct alignas(64) AudioBlock {
    static constexpr size_t K_CHANNELS = Channels;
    static constexpr size_t K_MAX_FRAMES = MaxFrames;

    alignas(64) std::array<float, Channels * MaxFrames> samples{};
    size_t active_frames = 0;

    constexpr float &operator[](size_t f, size_t ch) noexcept { return samples[f * Channels + ch]; }
    constexpr const float &operator[](size_t f, size_t ch) const noexcept {
        return samples[f * Channels + ch];
    }

    template <typename Enum>
        requires std::is_enum_v<Enum>
    constexpr float &operator[](size_t f, Enum ch) noexcept {
        return samples[f * Channels + static_cast<size_t>(ch)];
    }
    template <typename Enum>
        requires std::is_enum_v<Enum>
    constexpr const float &operator[](size_t f, Enum ch) const noexcept {
        return samples[f * Channels + static_cast<size_t>(ch)];
    }

    constexpr std::span<float, Channels> operator[](size_t f) noexcept {
        return std::span<float, Channels>(&samples[f * Channels], Channels);
    }
    constexpr std::span<const float, Channels> operator[](size_t f) const noexcept {
        return std::span<const float, Channels>(&samples[f * Channels], Channels);
    }

    constexpr AudioBlockSpan<Channels> span() const noexcept {
        return AudioBlockSpan<Channels>(samples.data(), active_frames);
    }
    constexpr MutableAudioBlockSpan<Channels> span() noexcept {
        return MutableAudioBlockSpan<Channels>(samples.data(), active_frames);
    }

    constexpr AudioBlockSpan<Channels> subspan(size_t offset,
                                               size_t count = std::dynamic_extent) const noexcept {
        return span().subspan(offset, count);
    }
    constexpr MutableAudioBlockSpan<Channels> subspan(size_t offset,
                                                      size_t count = std::dynamic_extent) noexcept {
        return span().subspan(offset, count);
    }

    constexpr operator AudioBlockSpan<Channels>() const noexcept { return span(); }
    constexpr operator MutableAudioBlockSpan<Channels>() noexcept { return span(); }

    constexpr MutableBusSlice dry() noexcept
        requires(Channels == 8);
    constexpr MutableBusSlice delay() noexcept
        requires(Channels == 8);
    constexpr MutableBusSlice reverb() noexcept
        requires(Channels == 8);
    constexpr MutableBusSlice slice(BusSelect sel) noexcept
        requires(Channels == 8);

    constexpr BusSlice dry() const noexcept
        requires(Channels == 8);
    constexpr BusSlice delay() const noexcept
        requires(Channels == 8);
    constexpr BusSlice reverb() const noexcept
        requires(Channels == 8);
    constexpr BusSlice slice(BusSelect sel) const noexcept
        requires(Channels == 8);

    float *data() noexcept { return samples.data(); }
    const float *data() const noexcept { return samples.data(); }

    constexpr size_t size() const noexcept { return active_frames; }
    constexpr size_t capacity() const noexcept { return MaxFrames; }
    constexpr void resize(size_t n) noexcept { active_frames = std::min(n, MaxFrames); }
    constexpr void fill(float value = 0.0f) noexcept { samples.fill(value); }
};

using BusBlock = AudioBlock<8, BLOCK_FRAMES>;
using StereoBlock = AudioBlock<2, BLOCK_FRAMES>;
using BusSpan = AudioBlockSpan<8>;
using StereoSpan = AudioBlockSpan<2>;
using MutableStereoSpan = MutableAudioBlockSpan<2>;

struct BusSlice {
    BusSpan span;
    BusSelect select = BusSelect::All;

    constexpr BusSlice dry() const noexcept { return {span, BusSelect::Dry}; }
    constexpr BusSlice delay() const noexcept { return {span, BusSelect::Delay}; }
    constexpr BusSlice reverb() const noexcept { return {span, BusSelect::Reverb}; }
    constexpr BusSlice slice(BusSelect sel) const noexcept { return {span, sel}; }
    constexpr size_t size() const noexcept { return span.size(); }
    constexpr const float *data() const noexcept { return span.data(); }
};

struct MutableBusSlice {
    MutableAudioBlockSpan<8> span;
    BusSelect select = BusSelect::All;

    constexpr MutableBusSlice dry() const noexcept { return {span, BusSelect::Dry}; }
    constexpr MutableBusSlice delay() const noexcept { return {span, BusSelect::Delay}; }
    constexpr MutableBusSlice reverb() const noexcept { return {span, BusSelect::Reverb}; }
    constexpr MutableBusSlice slice(BusSelect sel) const noexcept { return {span, sel}; }
    constexpr size_t size() const noexcept { return span.size(); }
    constexpr float *data() noexcept { return span.data(); }
    constexpr const float *data() const noexcept { return span.data(); }
    constexpr operator BusSlice() const noexcept { return BusSlice{span, select}; }

    constexpr size_t channel_left() const noexcept {
        switch (select) {
        case BusSelect::Dry:
            return static_cast<size_t>(OtisBus::DryL);
        case BusSelect::Delay:
            return static_cast<size_t>(OtisBus::DelayL);
        case BusSelect::Reverb:
            return static_cast<size_t>(OtisBus::ReverbL);
        default:
            return static_cast<size_t>(OtisBus::NcL);
        }
    }
};

template <size_t Channels>
constexpr BusSlice AudioBlockSpan<Channels>::dry() const noexcept
    requires(Channels == 8)
{
    return BusSlice{*this, BusSelect::Dry};
}
template <size_t Channels>
constexpr BusSlice AudioBlockSpan<Channels>::delay() const noexcept
    requires(Channels == 8)
{
    return BusSlice{*this, BusSelect::Delay};
}
template <size_t Channels>
constexpr BusSlice AudioBlockSpan<Channels>::reverb() const noexcept
    requires(Channels == 8)
{
    return BusSlice{*this, BusSelect::Reverb};
}
template <size_t Channels>
constexpr BusSlice AudioBlockSpan<Channels>::slice(BusSelect sel) const noexcept
    requires(Channels == 8)
{
    return BusSlice{*this, sel};
}

template <size_t Channels>
constexpr MutableBusSlice MutableAudioBlockSpan<Channels>::dry() noexcept
    requires(Channels == 8)
{
    return MutableBusSlice{*this, BusSelect::Dry};
}
template <size_t Channels>
constexpr MutableBusSlice MutableAudioBlockSpan<Channels>::delay() noexcept
    requires(Channels == 8)
{
    return MutableBusSlice{*this, BusSelect::Delay};
}
template <size_t Channels>
constexpr MutableBusSlice MutableAudioBlockSpan<Channels>::reverb() noexcept
    requires(Channels == 8)
{
    return MutableBusSlice{*this, BusSelect::Reverb};
}
template <size_t Channels>
constexpr MutableBusSlice MutableAudioBlockSpan<Channels>::slice(BusSelect sel) noexcept
    requires(Channels == 8)
{
    return MutableBusSlice{*this, sel};
}

template <size_t Channels>
constexpr BusSlice MutableAudioBlockSpan<Channels>::dry() const noexcept
    requires(Channels == 8)
{
    return BusSlice{*this, BusSelect::Dry};
}
template <size_t Channels>
constexpr BusSlice MutableAudioBlockSpan<Channels>::delay() const noexcept
    requires(Channels == 8)
{
    return BusSlice{*this, BusSelect::Delay};
}
template <size_t Channels>
constexpr BusSlice MutableAudioBlockSpan<Channels>::reverb() const noexcept
    requires(Channels == 8)
{
    return BusSlice{*this, BusSelect::Reverb};
}
template <size_t Channels>
constexpr BusSlice MutableAudioBlockSpan<Channels>::slice(BusSelect sel) const noexcept
    requires(Channels == 8)
{
    return BusSlice{*this, sel};
}

template <size_t Channels, size_t MaxFrames>
constexpr MutableBusSlice AudioBlock<Channels, MaxFrames>::dry() noexcept
    requires(Channels == 8)
{
    return span().dry();
}
template <size_t Channels, size_t MaxFrames>
constexpr MutableBusSlice AudioBlock<Channels, MaxFrames>::delay() noexcept
    requires(Channels == 8)
{
    return span().delay();
}
template <size_t Channels, size_t MaxFrames>
constexpr MutableBusSlice AudioBlock<Channels, MaxFrames>::reverb() noexcept
    requires(Channels == 8)
{
    return span().reverb();
}
template <size_t Channels, size_t MaxFrames>
constexpr MutableBusSlice AudioBlock<Channels, MaxFrames>::slice(BusSelect sel) noexcept
    requires(Channels == 8)
{
    return span().slice(sel);
}

template <size_t Channels, size_t MaxFrames>
constexpr BusSlice AudioBlock<Channels, MaxFrames>::dry() const noexcept
    requires(Channels == 8)
{
    return span().dry();
}
template <size_t Channels, size_t MaxFrames>
constexpr BusSlice AudioBlock<Channels, MaxFrames>::delay() const noexcept
    requires(Channels == 8)
{
    return span().delay();
}
template <size_t Channels, size_t MaxFrames>
constexpr BusSlice AudioBlock<Channels, MaxFrames>::reverb() const noexcept
    requires(Channels == 8)
{
    return span().reverb();
}
template <size_t Channels, size_t MaxFrames>
constexpr BusSlice AudioBlock<Channels, MaxFrames>::slice(BusSelect sel) const noexcept
    requires(Channels == 8)
{
    return span().slice(sel);
}

struct ScaledBusSlice {
    BusSlice slice;
    std::array<float, 2> gain;

    constexpr ScaledBusSlice dry() const noexcept { return {slice.dry(), gain}; }
    constexpr ScaledBusSlice delay() const noexcept { return {slice.delay(), gain}; }
    constexpr ScaledBusSlice reverb() const noexcept { return {slice.reverb(), gain}; }
};

inline constexpr ScaledBusSlice operator*(BusSpan span, const std::array<float, 2> &gain) noexcept {
    return ScaledBusSlice{BusSlice{span, BusSelect::All}, gain};
}
inline constexpr ScaledBusSlice operator*(const std::array<float, 2> &gain, BusSpan span) noexcept {
    return ScaledBusSlice{BusSlice{span, BusSelect::All}, gain};
}
inline constexpr ScaledBusSlice operator*(MutableAudioBlockSpan<8> span,
                                          const std::array<float, 2> &gain) noexcept {
    return ScaledBusSlice{BusSlice{span, BusSelect::All}, gain};
}
inline constexpr ScaledBusSlice operator*(const std::array<float, 2> &gain,
                                          MutableAudioBlockSpan<8> span) noexcept {
    return ScaledBusSlice{BusSlice{span, BusSelect::All}, gain};
}
inline constexpr ScaledBusSlice operator*(BusSlice slice,
                                          const std::array<float, 2> &gain) noexcept {
    return ScaledBusSlice{slice, gain};
}
inline constexpr ScaledBusSlice operator*(const std::array<float, 2> &gain,
                                          BusSlice slice) noexcept {
    return ScaledBusSlice{slice, gain};
}

template <size_t Channels>
inline constexpr MutableAudioBlockSpan<Channels> operator+=(MutableAudioBlockSpan<Channels> dst,
                                                            AudioBlockSpan<Channels> src) noexcept {
    const size_t n = std::min(dst.size(), src.size());
    for (size_t f = 0; f < n; ++f) {
        for (size_t ch = 0; ch < Channels; ++ch) {
            dst[f, ch] += src[f, ch];
        }
    }
    return dst;
}

inline constexpr MutableBusSlice operator+=(MutableBusSlice dst, StereoSpan src) noexcept {
    const size_t n = std::min(dst.size(), src.size());
    const size_t ch_l = dst.channel_left();
    const size_t ch_r = ch_l + 1;
    for (size_t f = 0; f < n; ++f) {
        dst.span[f, ch_l] += src[f, 0];
        dst.span[f, ch_r] += src[f, 1];
    }
    return dst;
}

// 64-byte aligned stack/heap storage owning planar (non-interleaved) channel buffers
template <size_t Channels, size_t MaxFrames = BLOCK_FRAMES, typename T = float>
struct alignas(64) PlanarAudioBlock {
    static constexpr size_t K_CHANNELS = Channels;
    static constexpr size_t K_MAX_FRAMES = MaxFrames;

    alignas(64) std::array<std::array<T, MaxFrames>, Channels> storage{};
    T *ptrs[Channels];

    PlanarAudioBlock() noexcept {
        for (size_t i = 0; i < Channels; ++i) ptrs[i] = storage[i].data();
    }

    T **data() noexcept { return ptrs; }
    const T *const *data() const noexcept { return ptrs; }
    operator T **() noexcept { return ptrs; }
    operator const T *const *() const noexcept { return ptrs; }

    T *operator[](size_t channel) noexcept { return ptrs[channel]; }
    const T *operator[](size_t channel) const noexcept { return ptrs[channel]; }

    constexpr size_t capacity() const noexcept { return MaxFrames; }

    PlanarAudioBlock &operator=(const ScaledBusSlice &scaled) noexcept {
        const size_t n = std::min(MaxFrames, scaled.slice.size());
        const float *src = scaled.slice.data();
        const float gl = scaled.gain[0], gr = scaled.gain[1];

        const auto copy_pair = [&](size_t dsp_ch, size_t bus_ch) {
            for (size_t f = 0; f < n; ++f) {
                ptrs[dsp_ch][f] = src[f * 8 + bus_ch] * gl;
                ptrs[dsp_ch + 1][f] = src[f * 8 + bus_ch + 1] * gr;
            }
        };

        switch (scaled.slice.select) {
        case BusSelect::All:
            copy_pair(0, static_cast<size_t>(OtisBus::DryL));
            if constexpr (Channels >= 4) copy_pair(2, static_cast<size_t>(OtisBus::DelayL));
            if constexpr (Channels >= 6) copy_pair(4, static_cast<size_t>(OtisBus::ReverbL));
            break;
        case BusSelect::Dry:
            copy_pair(0, static_cast<size_t>(OtisBus::DryL));
            break;
        case BusSelect::Delay:
            if constexpr (Channels >= 4) copy_pair(2, static_cast<size_t>(OtisBus::DelayL));
            break;
        case BusSelect::Reverb:
            if constexpr (Channels >= 6) copy_pair(4, static_cast<size_t>(OtisBus::ReverbL));
            break;
        }
        return *this;
    }
};

template <size_t MaxFrames, typename T>
inline constexpr MutableBusSlice operator+=(MutableBusSlice dst,
                                            const PlanarAudioBlock<2, MaxFrames, T> &src) noexcept {
    const size_t n = std::min(dst.size(), src.capacity());
    const size_t ch_l = dst.channel_left();
    const size_t ch_r = ch_l + 1;
    for (size_t f = 0; f < n; ++f) {
        dst.span[f, ch_l] += src[0][f];
        dst.span[f, ch_r] += src[1][f];
    }
    return dst;
}

} // namespace f3rt::hle
