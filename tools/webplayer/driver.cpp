// Song trace driver: replays a song script on a fresh hle::Core and records it until the trace span ends
// (tools/webplayer/README.md, section 4.3).
#define F3RT_HLE_M2 1
#include "audio/hle/replay.hpp"
#include "trace_recorder.hpp"

#include <cmath>

namespace f3rt::webplayer {

namespace {

// Output frames below this level count as silence (the sound-extract criterion: |sample| <= 8 / 32768).
constexpr float SILENCE_PEAK = 8.0f / 32768.0f;

class ActivitySink final : public hle::PcmSink {
public:
    void write(const float *stereo, size_t frames) override {
        for (size_t i = 0; i < frames; ++i) {
            const bool loud = std::fabs(stereo[i * 2]) > SILENCE_PEAK || std::fabs(stereo[i * 2 + 1]) > SILENCE_PEAK;
            ++frame_;
            if (loud) {
                quiet_ = 0;
                last_loud_ = frame_;
            } else {
                ++quiet_;
            }
        }
    }
    uint64_t quiet_frames() const { return quiet_; }
    uint64_t end_of_sound() const { return last_loud_; } // frames up to and including the last loud one

private:
    uint64_t frame_ = 0, quiet_ = 0, last_loud_ = 0;
};

} // namespace

TraceResult trace_song(std::span<const uint8_t> sound_rom, std::span<const uint16_t> sample_words,
                       std::span<const uint8_t> script, const SongMeta &meta, const TraceOptions &options) {
    hle::Core::enable_flush_denormals();
    hle::Core core(sound_rom, sample_words);
    ActivitySink sink;
    core.set_pcm_sink(&sink);

    EngineQueries queries;
#ifdef F3RT_HLE_M2
    queries.effects = [&core] { return core.effects_info(); };
    queries.program = [&core](uint16_t id) { return core.describe_program(id); };
    queries.sequence = [&core](uint8_t id) { return core.describe_sequence(id); };
    queries.bend_range = core.bend_range();
#endif
    TraceRecorder recorder(options, queries);
#ifdef F3RT_HLE_M2
    core.set_trace(&recorder);
#else
    core.set_voice_observer([&recorder](const hle::VoiceEvent &e) { recorder.voice(e); });
#endif
    hle::replay(core, script);

    const uint64_t cap_ticks = uint64_t(options.max_seconds * double(hle::MAIN_CLOCK));
    const uint64_t drain_ticks = uint64_t(options.drain_seconds * double(hle::MAIN_CLOCK));
    const uint64_t tail_frames = uint64_t(std::llround(options.tail_seconds * double(hle::SAMPLE_RATE)));
    SpanEnd end;
    for (;;) {
        core.advance(core.next_boundary());
#ifdef F3RT_HLE_M2
        if (recorder.wrapped()) {
            // Everything started before the wrap keeps playing until its voices stop.
            if (recorder.open_voices() == 0 || core.tick() >= recorder.wrap_tick() + drain_ticks) {
                end.reason = EndReason::LoopWrap;
                end.ticks = recorder.wrap_tick();
                end.frames = hle::Core::frames_at(end.ticks);
                break;
            }
            continue;
        }
        if (!core.sequences_running() && sink.quiet_frames() >= tail_frames) {
            end.reason = EndReason::SongEnd;
            end.frames = sink.end_of_sound();
            end.ticks = end.frames * hle::MAIN_CLOCK / hle::SAMPLE_RATE;
            break;
        }
#else
        if (sink.end_of_sound() > 0 && sink.quiet_frames() >= tail_frames) {
            end.reason = EndReason::SongEnd;
            end.frames = sink.end_of_sound();
            end.ticks = end.frames * hle::MAIN_CLOCK / hle::SAMPLE_RATE;
            break;
        }
#endif
        if (core.tick() >= cap_ticks) {
            end.reason = EndReason::Cap;
            end.frames = core.frames();
            end.ticks = core.tick();
            break;
        }
    }
    end.audio_frames = core.frames();
#ifdef F3RT_HLE_M2
    core.set_trace(nullptr);
#endif
    return recorder.finish(meta, end, sample_words);
}

} // namespace f3rt::webplayer
