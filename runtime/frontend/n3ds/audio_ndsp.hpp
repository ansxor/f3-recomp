#pragma once
#include <cstddef>
#include <cstdint>

namespace f3rt::n3ds {
class NdspAudioOutput {
public:
    static constexpr unsigned BUFFER_COUNT = 8, BUFFER_FRAMES = 1024;

    NdspAudioOutput();
    ~NdspAudioOutput();
    NdspAudioOutput(const NdspAudioOutput &) = delete;
    NdspAudioOutput &operator=(const NdspAudioOutput &) = delete;

    bool valid() const { return m_valid; }
    size_t queued_frames() const;
    void push(const int16_t *stereo, size_t frames);
    void clear();
    void set_source_rate(double rate);
    void set_rate(double ratio);
    void set_volume(float volume);

private:
    void *m_memory = nullptr;
    void *m_data = nullptr;
    bool m_valid = false;
    unsigned m_next = 0;
    double m_source_rate = 48000;
    double m_ratio = 1.0;
};
} // namespace f3rt::n3ds
