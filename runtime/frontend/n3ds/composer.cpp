#include "composer.hpp"
#include "renderer/game/captured_frame.hpp"
#include "renderer/game/sprites.hpp"
#include <chrono>

namespace f3rt::n3ds {
Composer::Composer(Shared &shared, Session &session, const Settings &settings, PerfStats &perf)
    : m_shared(shared), m_session(session), m_settings(settings), m_perf(perf) {}
Composer::~Composer() { stop(); }
void Composer::start() {
    m_thread = f3rt::Thread(f3rt::ThreadRole::Composer, 0, [this] { run(); });
}
void Composer::stop() {
    m_stop.store(true);
    m_shared.cv.notify_all();
    m_thread.join();
}
void Composer::run() {
    // Presentation stays pixel-exact at scale 1; only the side border may widen the frame.
    f3rt::GameVideoOptions options{1, m_settings.border};
    const size_t pixel_count = size_t(options.width()) * options.height();
    m_plane.resize(f3rt::sprite_plane_size(options));
    while (true) {
        std::unique_lock lock(m_shared.mutex);
        m_shared.cv.wait(lock, [&] { return m_stop.load() || m_shared.emulation.unseen(); });
        if (m_stop.load()) return;
        bool fresh = false;
        const f3rt::FrameSnapshot *shot = m_shared.emulation.take(fresh);
        if (!shot || !shot->captured) continue;
        // The emulation slot stays held (immutable) until the next take, so composing outside the
        // lock reads a frame nobody will rewrite.
        f3rt::FrameSnapshot &out = m_shared.present.begin();
        out.frame = shot->frame;
        out.epoch = shot->epoch;
        out.scale = options.scale;
        out.start = shot->start;
        out.pixels.resize(pixel_count);
        lock.unlock();
        const auto begin = std::chrono::steady_clock::now();
        f3rt::raster_captured_sprites(*shot->captured, m_session.sprite_tiles(), m_plane, options);
        const auto raster_end = std::chrono::steady_clock::now();
        f3rt::compose_captured_scene(*shot->captured, m_session.playfield_tiles(), m_plane,
                                     out.pixels, options, f3rt::ComposeMode::Parallel);
        const auto end = std::chrono::steady_clock::now();
        const auto to_us = [](auto span) {
            return uint64_t(std::chrono::duration_cast<std::chrono::microseconds>(span).count());
        };
        const auto ema = [](std::atomic<uint64_t> &slot, uint64_t sample) {
            const uint64_t previous = slot.load(std::memory_order_relaxed);
            const int64_t value =
                previous ? int64_t(previous) + (int64_t(sample) - int64_t(previous)) / 16
                         : int64_t(sample);
            slot.store(uint64_t(value), std::memory_order_relaxed);
        };
        ema(m_perf.raster_us, to_us(raster_end - begin));
        ema(m_perf.scene_us, to_us(end - raster_end));
        ema(m_perf.compose_us, to_us(end - begin));
        m_perf.composed.fetch_add(1, std::memory_order_relaxed);
        {
            std::lock_guard publish_lock(m_shared.mutex);
            m_shared.present.publish();
        }
        m_shared.cv.notify_all();
    }
}
} // namespace f3rt::n3ds
