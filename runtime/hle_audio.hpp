#pragma once
#include "f3rt/audio.hpp"
#include "hle_events.hpp"
#include <memory>
#include <span>

namespace f3rt::hle {
// Main-side mailbox state is serialized; the worker, PCM, instance ledger and
// effects history are intentionally not. Only the main thread touches RAM.
class AudioEngine {
public:
    AudioEngine(std::span<const uint8_t> rom, std::span<const uint16_t> samples, uint8_t *shared);
    ~AudioEngine();
    void advance(uint32_t cycles);
    uint64_t clock() const;
    bool is_reset() const;
    void set_reset(bool asserted);
    void shared_write(uint32_t offset, uint64_t frame);
    void begin_frame(uint64_t frame);
    void finish_frame(uint64_t frame);
    void begin_rollback(uint64_t begin, uint64_t end);
    void end_rollback();
    void flush() const;
    size_t available() const;
    size_t render(float *stereo, size_t frames);
    size_t render(int16_t *stereo, size_t frames);
    size_t render_ready(int16_t *stereo, size_t frames);
    uint64_t generated() const;
    Audio::HleStats stats() const;
    void set_observer(std::function<void(const VoiceEvent &)> observer);
    void set_gain_model(Audio::GainModel model);
    static constexpr size_t state_bytes = 32 + 100 * 8 * 2;
    void save(std::span<uint8_t> dst) const;
    void load(std::span<const uint8_t> src);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace f3rt::hle
