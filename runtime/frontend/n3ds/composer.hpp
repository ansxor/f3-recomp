#pragma once
#include "cpu_counters.hpp"
#include "perf.hpp"
#include "renderer/game/indexed_compositor.hpp"
#include "renderer/game/tiles.hpp"
#include "session.hpp"
#include "settings.hpp"
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace f3rt::n3ds {
// The composer side, on ThreadRole::Composer: takes the newest captured frame and composes the
// Enhanced scene with the indexed kernel (row chunks run on the ComposeHelper roles) straight into
// the presenter's staging buffer, then publishes it. Latest wins: a slow composer drops frames and
// never delays emulation.
class Composer {
public:
    Composer(Shared &shared, Session &session, const Settings &settings, PerfStats &perf);
    ~Composer();
    Composer(const Composer &) = delete;
    Composer &operator=(const Composer &) = delete;

    void start();
    void stop();

private:
    void run();
    // One-time tile-row opacity table; skipped when the application region is too tight.
    void build_opacity();

    Shared &m_shared;
    Session &m_session;
    Settings m_settings;
    PerfStats &m_perf;
    f3rt::Thread m_thread;
    std::atomic<bool> m_stop{false};
    f3rt::TileRowOpacity m_opacity;
    bool m_opacity_checked = false;
    CpuCounters m_counters;
    bool m_counters_open = false;
};
} // namespace f3rt::n3ds
