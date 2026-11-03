#pragma once
#include "f3rt/game_video.hpp"
#include "cpu_counters.hpp"
#include "frame_slots.hpp"
#include "perf.hpp"
#include "platform/thread.hpp"
#include "settings.hpp"
#include <atomic>
#include <array>
#include <condition_variable>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace f3rt {
class Machine;
class AudioPacer;
} // namespace f3rt

namespace f3rt::n3ds {
class Bench;
class NdspAudioOutput;

enum class Command : uint8_t { Pause, Resume, SystemPause, SystemResume, SetVolume, Stop };

// Frame hand-offs between the three threads: emulation -> composer carries captured frames,
// composer -> presenter carries composed pixels. Both pairs share one mutex and variable; slot
// access itself is serialized by that mutex, and FrameSlots keeps the published frames immutable.
struct Shared {
    std::mutex mutex;
    std::condition_variable cv;
    f3rt::FrameSlots emulation;
    f3rt::FrameSlots present;
    std::atomic<bool> quit{false};
};

// The emulation side, on ThreadRole::Emulation: game setup, fast boot, the paced frame loop and
// ndsp output. Everything it owns stays on that thread until stop() joins it.
class Session {
public:
    Session(Shared &shared, const Settings &settings, const std::string &set,
            const std::string &data_dir, NdspAudioOutput *audio, PerfStats &perf);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;

    void start();
    void stop();
    void post(Command command, unsigned value = 0);

    // Written by the main thread once per vblank; read by the emulation thread per frame.
    std::atomic<uint16_t> input{0};
    std::atomic<bool> running{false};
    std::string error() const;

    // Capture geometry; fixed for the session (a border change restarts the game).
    GameVideoOptions geometry() const { return m_video; }
    std::span<const uint8_t> playfield_tiles() const { return m_playfield; }
    std::span<const uint8_t> sprite_tiles() const { return m_sprite; }

private:
    void run();
    void startup();
    void stage(const char *step);
    void emulate();
    // Whether this frame's hardware counters are on (the Performance overlay toggle); opens them on
    // first use.
    bool profile_frame();
    void apply(Command command, unsigned value);
    void fail(const std::string &message);
    void push_audio(const int16_t *stereo, size_t frames);
    void log_audio_window(class Machine &machine);

    Shared &m_shared;
    Settings m_settings;
    std::string m_set, m_data_dir;
    GameVideoOptions m_video;
    NdspAudioOutput *m_audio;
    PerfStats &m_perf;
    // The emulation thread's own performance monitor and its per-stage split of each frame.
    CpuCounters m_counters;
    StageCounters m_stages{m_counters};
    bool m_counters_open = false;
    f3rt::Thread m_thread;
    std::unique_ptr<f3rt::Machine> m_machine;
#ifdef F3_N3DS_BENCH
    std::unique_ptr<Bench> m_bench;
#endif
    std::unique_ptr<f3rt::AudioPacer> m_pacer;
    std::span<const uint8_t> m_playfield, m_sprite;
    // Capture buffers handed to the presenter through FrameSlots; owned here for the session's life.
    std::array<std::unique_ptr<f3rt::CapturedFrame>, f3rt::FrameSlots::SLOT_COUNT> m_captures;
    std::atomic<bool> m_stop{false}, m_paused{false};
    std::atomic<unsigned> m_volume;
    uint64_t m_epoch = 0;
    std::vector<int16_t> m_silence;
    std::mutex m_command_mutex;
    std::vector<std::pair<Command, unsigned>> m_commands;
    // Device queue depth over the current reporting window, in frames; logged with the pacer's
    // drops and the underruns so a stutter can be attributed without guessing.
    std::chrono::steady_clock::time_point m_audio_window_start;
    uint64_t m_audio_window_emulated = 0, m_audio_window_drops_base = 0;
    uint64_t m_audio_window_frames = 0, m_audio_window_samples = 0;
    size_t m_audio_min = SIZE_MAX, m_audio_max = 0;
    uint64_t m_audio_pushes = 0, m_audio_window_underruns = 0, m_audio_window_late = 0;
    uint64_t m_audio_window_pushed = 0;
    double m_source_rate = 0;
    uint32_t m_audio_window_peak = 0;
    uint64_t m_audio_window_energy = 0, m_audio_window_samples_pcm = 0;
    uint64_t m_hle_commands_base = 0;
    double m_audio_ratio = 1.0;
    mutable std::mutex m_error_mutex;
    std::string m_error;
    // Setup step the emulation thread is in; a failure reports it alongside the exception.
    const char *m_stage = "construct";
};
} // namespace f3rt::n3ds
