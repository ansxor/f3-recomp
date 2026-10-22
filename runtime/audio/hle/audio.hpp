#pragma once
#include "f3rt/audio.hpp"
#include "audio/hle/events.hpp"
#include <memory>
#include <span>

namespace f3rt::hle {
// Main-side mailbox state is serialized; the worker, PCM and effects history are
// intentionally not. Only the main thread touches RAM.
class AudioEngine {
public:
    AudioEngine(std::span<const uint8_t> rom, std::span<const uint16_t> samples, uint8_t *shared);
    ~AudioEngine();
    void advance(uint32_t cycles);
    uint64_t clock() const;
    bool is_reset() const;
    void set_reset(bool asserted);
    void set_bank(uint8_t bank);
    void shared_write(uint32_t offset);
    void finish_frame();
    void flush() const;
    size_t available() const;
    size_t render(float *stereo, size_t frames);
    size_t render(int16_t *stereo, size_t frames);
    size_t render_ready(int16_t *stereo, size_t frames);
    uint64_t generated() const;
    Audio::EnhancedStats stats() const;
#if F3RT_HLE_EVENTS
    void set_observer(std::function<void(const VoiceEvent &)> observer);
#endif
    static constexpr size_t STATE_BYTES = 32;
    void save(std::span<uint8_t> dst) const;
    void load(std::span<const uint8_t> src);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};
bool validate(std::span<const uint8_t> sound, std::span<const uint16_t> samples) noexcept;
} // namespace f3rt::hle
