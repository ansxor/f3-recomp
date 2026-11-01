#include "frontend/audio_pacer.hpp"
#include <algorithm>

namespace f3rt {
AudioPacer::AudioPacer(Config config)
    : m_config(config), m_average(double(config.target_frames)) {}
size_t AudioPacer::cap_frames() const {
    return m_config.target_frames +
           size_t(m_config.rate) * m_config.max_queue_ms / 1000;
}
AudioPacer::Action AudioPacer::before_push(size_t queued_frames, size_t chunk_frames) {
    Action action;
    if (!m_config.throttle) return action;
    if (queued_frames > cap_frames()) {
        action.drop_backlog = true;
        ++m_stats.drops;
        queued_frames = 0;
    }
    if (queued_frames == 0) {
        m_average = double(m_config.target_frames);
        if (chunk_frames < m_config.target_frames)
            action.lead_silence_frames = m_config.target_frames - chunk_frames;
    }
    return action;
}
double AudioPacer::after_push(size_t queued_frames) {
    m_stats.queue_sum += queued_frames;
    ++m_stats.queue_samples;
    m_stats.queue_max = std::max(m_stats.queue_max, uint64_t(queued_frames));
    if (!m_config.throttle || !m_config.target_frames) return 1.0;
    m_average += (double(queued_frames) - m_average) / 64.0;
    const double error = std::clamp((m_average - double(m_config.target_frames)) /
                                        double(m_config.target_frames),
                                    -1.0, 1.0);
    return 1.0 + 0.005 * error;
}
void AudioPacer::reset() { m_average = double(m_config.target_frames); }
} // namespace f3rt
