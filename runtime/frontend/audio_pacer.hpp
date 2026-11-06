#pragma once
#include <cstddef>
#include <cstdint>

namespace f3rt {
class AudioPacer {
public:
    struct Config {
        uint32_t rate = 0;
        size_t target_frames = 0;
        unsigned max_queue_ms = 50;
        bool throttle = true;
    };
    struct Action {
        bool drop_backlog = false;
        size_t lead_silence_frames = 0;
    };
    struct Stats {
        uint64_t drops = 0, queue_sum = 0, queue_samples = 0, queue_max = 0;
    };

    explicit AudioPacer(Config config);

    Action before_push(size_t queued_frames, size_t chunk_frames);
    double after_push(size_t queued_frames);
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
