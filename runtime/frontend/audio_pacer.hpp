#pragma once
#include <cstddef>
#include <cstdint>

namespace f3rt {
// Audio queue policy shared by the SDL and ndsp outputs, with no I/O of its own. Every quantity is
// in frames (one stereo sample pair); an output converts to its own byte or sample counts.
//
// Frames are paced by the system clock and consumed by the device clock, so the policy keeps the
// device queue at `target_frames` by nudging the playback rate (at most 0.5%, inaudible) against a
// ~1 s average of its depth. A stalled window (tab-out, fullscreen switch) must not leave seconds
// of audio queued ahead of the picture: past the latency cap the stale backlog is dropped. An empty
// queue (start, after a drop or an underrun) is already silent, so the chunk is led with silence up
// to the target instead of letting the device run dry on every period until it refills.
class AudioPacer {
public:
    struct Config {
        uint32_t rate = 0;
        size_t target_frames = 0;
        unsigned max_queue_ms = 50; // latency cap above the target that triggers a drop
        bool throttle = true;
    };
    struct Action {
        bool drop_backlog = false;      // clear the device queue before pushing
        size_t lead_silence_frames = 0; // silence to push before the chunk
    };
    struct Stats {
        uint64_t drops = 0, queue_sum = 0, queue_samples = 0, queue_max = 0;
    };

    explicit AudioPacer(Config config);

    // Before pushing `chunk_frames` into a queue holding `queued_frames`.
    Action before_push(size_t queued_frames, size_t chunk_frames);
    // After pushing: the playback-rate ratio to set, or 1.0 when not throttled. Records the depth.
    double after_push(size_t queued_frames);
    // Explicit clear of the queue (state load): forget the observed depth.
    void reset();
    const Stats &stats() const { return m_stats; }
    size_t target_frames() const { return m_config.target_frames; }

private:
    size_t cap_frames() const;
    Config m_config;
    Stats m_stats;
    double m_average;
};
} // namespace f3rt
