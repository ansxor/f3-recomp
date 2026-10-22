#pragma once
// Single-threaded HLE audio core: the synthesis services, Effects and Sequencer, the tick -> frame render loop, the
// gain math and the command / reset / bank / gain handling of the former AudioEngine worker. No thread, mutex, SDL,
// Machine or mailbox dependency: AudioEngine wraps it with a thread and the mailbox, so there is one implementation of
// the mixing path.
#include "audio/hle/block.hpp"
#include "audio/hle/events.hpp"
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>

namespace f3rt::hle {

// Output stage gain, shared by the reference and enhanced backends (== Audio::OUTPUT_BOOST; hle/audio.cpp asserts it).
inline constexpr float OUTPUT_BOOST = 12.0f;

// Receives rendered stereo frames in render order, in blocks of 1..Core::BLOCK_FRAMES frames.
class PcmSink {
public:
    // Interleaved L,R in OTIS/ESP channel order (the left/right exchange of the DAC is applied by the final consumer).
    virtual void write(const float *stereo, size_t frames) = 0;

protected:
    ~PcmSink() = default;
};

class Core final {
public:
    static constexpr size_t BLOCK_FRAMES = f3rt::hle::BLOCK_FRAMES;

    // `sound_rom` and `sample_words` must outlive the Core. Throws std::runtime_error when the ROM is not supported
    // (the checks of hle::validate()).
    Core(std::span<const uint8_t> sound_rom, std::span<const uint16_t> sample_words);
    ~Core();
    Core(const Core &) = delete;
    Core &operator=(const Core &) = delete;

    // ---- configuration. Call only while no other Core call is running (AudioEngine: after flush()).
    // nullptr drops the rendered frames (fast-forward); the engine state evolves identically.
    void set_pcm_sink(PcmSink *sink);
    void set_voice_observer(std::function<void(const VoiceEvent &)> observer);

    // ---- time. Worker timebase: 16 MHz ticks since construction, monotonic. A tick argument smaller than tick() is
    // clamped to tick().
    uint64_t tick() const;          // sequencer events up to and including this tick have fired
    uint64_t frames() const;        // frames handed to the sink so far
    // floor(tick * SAMPLE_RATE / MAIN_CLOCK), overflow-free.
    static uint64_t frames_at(uint64_t tick);

    // ---- inputs. Each one first runs advance(tick) and then applies at tick().
    void advance(uint64_t tick);
    // `packet` = [length, opcode, args...] with length == packet.size(); throws std::runtime_error when invalid.
    void command(std::span<const uint8_t> packet, uint64_t instance, uint64_t tick);
    void reset(uint64_t tick);
    // `page` = the value Machine passes to Audio::set_sound_bank.
    void set_bank(uint8_t page, uint64_t tick);
    // `raw` = mailbox byte 0x7fa (channel 0) / 0x7fb (channel 1); only raw & 63 is used.
    void set_gain(unsigned channel, uint8_t raw, uint64_t tick);


    // Throws-nothing check that the ROM and samples can be driven by the HLE core.
    static bool validate(std::span<const uint8_t> sound_rom,
                         std::span<const uint16_t> sample_words) noexcept;

    // Sets flush-to-zero and denormals-are-zero on the calling thread (x86 SSE, aarch64 FPCR); no-op elsewhere,
    // including WebAssembly. Core never calls it: the thread driving the Core does.
    static void enable_flush_denormals();

private:
    struct Impl;
    std::unique_ptr<Impl> m_impl;
};

} // namespace f3rt::hle
