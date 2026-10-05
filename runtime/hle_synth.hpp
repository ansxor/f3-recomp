#pragma once
#include "hle_events.hpp"
#include <cstddef>
#include <functional>
#include <memory>
#include <span>

namespace f3rt::hle {
// ROM-data synthesis, independent of sound CPU and sound-device execution.
// Input spans must outlive Synth. Output is four interleaved stereo buses.
class Synth {
public:
    Synth(std::span<const uint8_t> sound_rom, std::span<const uint16_t> sample_words);
    ~Synth();
    Synth(const Synth &) = delete;
    Synth &operator=(const Synth &) = delete;
    void note_on(const Note &note);
    void note_off(uint64_t instance, uint64_t tick);
    void channel_update(uint8_t sequence, uint8_t track, const Channel &channel, uint64_t tick);
    void reset(uint64_t tick);
    void cancel(uint64_t instance, uint64_t tick, uint32_t fade_frames = 240);
    void render(float *interleaved_eight_channels, size_t frames);
    void set_observer(std::function<void(const VoiceEvent &)> observer);
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
}
