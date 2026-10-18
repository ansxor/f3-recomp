#pragma once

// Native recorder of the prebaked DAW data of one song (tools/webplayer/README.md, section 4): a TraceTap attached to
// a hle::Core that turns the engine's events into the columnar trace (songs/<id>.json + songs/<id>.bin), and the
// driver that replays a song script on a fresh Core until the trace span ends.

#include "audio/hle/core.hpp"
#include "audio/hle/observe.hpp"

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

namespace f3rt::webplayer {

struct TraceOptions {
    unsigned loops = 1;        // loop iterations recorded after the intro (looping songs)
    double max_seconds = 360;  // hard cap of the recorded span (songs that neither wrap nor end)
    double tail_seconds = 1.0; // output silence that ends a non-looping song after its sequences stopped
    // Drain limit: how long after the span end rendering continues for voices started before it (looping songs).
    double drain_seconds = 10.0;
    // Keep the unsimplified voice-lane samples in TraceResult::raw_lanes (tests).
    bool keep_raw_lanes = false;
};

struct SongMeta {
    std::string set, id, label;
    uint32_t code = 0;
    std::optional<uint32_t> bank, page;
};

// The read-only engine queries the recorder needs (hle::Core provides them; tests substitute fakes).
struct EngineQueries {
    std::function<hle::EffectsInfo()> effects;
    std::function<std::optional<hle::ProgramInfo>(uint16_t)> program;
    std::function<std::optional<hle::SequenceInfo>(uint8_t)> sequence;
    uint8_t bend_range = 0;
};

enum class EndReason : uint8_t { LoopWrap, SongEnd, Cap };
const char *end_reason_name(EndReason reason);

struct TraceCounts {
    uint32_t sections = 0, notes = 0, voices = 0, instruments = 0, regions = 0, channels = 0;
    uint32_t lane_points = 0, chan_events = 0;
};

// Raw (unsimplified) samples of one voice lane, for the lane-reconstruction check.
struct RawVoiceLane {
    uint32_t voice = 0; // row in the voices table
    std::string lane;
    uint32_t start_frame = 0;
    int32_t onset = 0;
    std::vector<std::pair<uint32_t, int32_t>> samples; // (frame, value) after the onset, strictly increasing
};

struct TraceResult {
    std::string json;
    std::vector<uint8_t> bin;
    uint64_t frames = 0, audio_frames = 0, ticks = 0;
    EndReason end_reason = EndReason::Cap;
    bool loop = false;
    TraceCounts counts;
    std::vector<RawVoiceLane> raw_lanes; // only with TraceOptions::keep_raw_lanes
};

// What the driver tells finish().
struct SpanEnd {
    uint64_t frames = 0;       // trace span in frames
    uint64_t audio_frames = 0; // where rendering ended
    uint64_t ticks = 0;
    EndReason reason = EndReason::Cap;
};

class TraceRecorder final : public hle::TraceTap {
public:
    TraceRecorder(const TraceOptions &options, EngineQueries queries);
    ~TraceRecorder() override;
    TraceRecorder(const TraceRecorder &) = delete;
    TraceRecorder &operator=(const TraceRecorder &) = delete;

    // True once the N-th arrangement wrap (N = options.loops) was seen: nothing starts after `wrap_tick()`.
    bool wrapped() const;
    uint64_t wrap_tick() const;
    // True when the arrangement ended without a loop marker.
    bool song_ended() const;
    // Voice rows of recorded notes that have started but not stopped.
    uint32_t open_voices() const;

    // ---- hle::TraceTap
    void note_on(const hle::Note &note) override;
    void note_off(uint64_t instance, uint64_t tick) override;
    void channel_update(uint8_t sequence, uint8_t track, const hle::Channel &channel, uint64_t tick) override;
    void effect(uint8_t preset, uint64_t tick) override;
    void effect_parameter(uint8_t index, uint8_t value, uint64_t tick) override;
    void voice(const hle::VoiceEvent &event) override;
    void gain(unsigned channel, uint8_t raw, float linear, uint64_t tick) override;
    void mix_block(uint64_t first_frame, const float *buses8, const float *stereo, size_t frames,
                   const std::array<float, 2> &input_gain, const std::array<float, 2> &output_gain) override;
    void section_start(const hle::SectionInfo &info, uint64_t tick) override;
    void section_end(uint8_t sequence, hle::SectionEnd how, uint64_t tick) override;
    void bar(uint8_t sequence, uint32_t bar_index, uint64_t tick) override;
    void rate_change(uint8_t sequence, uint16_t rate, uint64_t tick) override;
    void arrangement_wrap(uint8_t owner, uint8_t resume_phase, uint64_t tick) override;

    // Builds the trace. `sample_words` is the sample ROM the engine rendered from (region overviews).
    TraceResult finish(const SongMeta &meta, const SpanEnd &end, std::span<const uint16_t> sample_words);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// Replays `script` (hle::replay) on a fresh Core over the ROM and records the song. Applies FTZ/DAZ on the calling
// thread. Throws what the Core throws (e.g. an unsupported sound ROM bank layout).
TraceResult trace_song(std::span<const uint8_t> sound_rom, std::span<const uint16_t> sample_words,
                       std::span<const uint8_t> script, const SongMeta &meta, const TraceOptions &options);

} // namespace f3rt::webplayer
