#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <vector>

#ifndef F3RT_HLE_EVENTS
#define F3RT_HLE_EVENTS 0
#endif

namespace f3rt {
namespace hle {
struct VoiceEvent;
}

struct AudioPerformanceStats {
    uint64_t commands = 0, rendered_frames = 0, voices_started = 0;
    unsigned voices_active = 0;
    uint64_t render_ns = 0;
};

// Raw big-endian 16-bit sample ROM bytes to host words.
std::vector<uint16_t> decode_sample_words(std::span<const uint8_t> sample_rom);

// The F3 sound board, driven by main-CPU mailbox writes and rendered on a synth worker thread.
// Output is interleaved stereo (Left, Right) at sample_rate().
class Audio {
public:
    // Whether the sound program and samples can be driven; the constructor throws when this is false.
    static bool supported(std::span<const uint8_t> sound_rom,
                          std::span<const uint16_t> sample_words) noexcept;
    // sound_rom: 0x80000+ bytes 68000 program code (interleaved high/low byte pairs), owned.
    // sample_words: decoded sample ROM, owned. shared_ram: 0x800-byte dual-port RAM owned by the main CPU
    // bus; must outlive Audio.
    Audio(std::vector<uint8_t> sound_rom, std::vector<uint16_t> sample_words, uint8_t *shared_ram);
    ~Audio();

    Audio(const Audio &) = delete;
    Audio &operator=(const Audio &) = delete;

    void shared_write(uint32_t offset);
    void finish_frame();
    void advance(uint32_t main_cycles);
    // Continuous effective device time, not the caller's main-block endpoint.
    uint64_t clock_ticks() const;
    uint64_t generated_frames() const;
    AudioPerformanceStats performance_stats() const;
#if F3RT_HLE_EVENTS
    // Diagnostic callback executes on the worker, never on the main CPU.
    void set_voice_observer(std::function<void(const hle::VoiceEvent &)> observer);
#endif

    // Reset control (active-high asserted = CPU held in reset)
    void set_reset(bool asserted);
    bool is_reset() const;
    // Whole-machine/watchdog reset: hold the board and keep the continuous audio clock.
    void reset_board();
    // Sound CPU program ROM banking (0xc20000 - 0xc3ffff window, default page 1)
    void set_sound_bank(uint32_t page);
    uint32_t sound_bank() const;

    uint32_t sample_rate() const;
    size_t available_frames() const;
    size_t render(int16_t *interleaved_stereo, size_t max_frames);
    size_t render(float *interleaved_stereo, size_t max_frames);
    // Non-blocking drain: returns only PCM already produced without waiting. It holds back the audio of
    // the video frame that finish_frame() just closed, so each call returns the frame before it whether
    // or not the worker is quick: the amount does not race with it. Callers needing a complete,
    // deterministic stream must finish with the blocking render.
    size_t render_ready(int16_t *interleaved_stereo, size_t max_frames);

    // State snapshot serialization
    size_t state_size() const;
    void save_state(std::span<uint8_t> dst) const;
    void load_state(std::span<const uint8_t> src);

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace f3rt
