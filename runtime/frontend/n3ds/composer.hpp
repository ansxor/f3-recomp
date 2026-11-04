#pragma once
#include "cpu_counters.hpp"
#include "perf.hpp"
#include "renderer/game/indexed_compositor.hpp"
#include "session.hpp"
#include "settings.hpp"
#include <atomic>
#include <cstdint>
#include <span>
#include <vector>

namespace f3rt::n3ds {
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

    Shared &m_shared;
    Session &m_session;
    Settings m_settings;
    PerfStats &m_perf;
    f3rt::Thread m_thread;
    std::atomic<bool> m_stop{false};
    CpuCounters m_counters;
    bool m_counters_open = false;
};
} // namespace f3rt::n3ds
