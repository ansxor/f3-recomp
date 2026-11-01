#pragma once
#include "perf.hpp"
#include <cstdint>
#include "platform/thread.hpp"
#include "session.hpp"
#include <atomic>
#include <cstdint>
#include <vector>

namespace f3rt::n3ds {
// The composer side, on ThreadRole::Composer: takes the newest captured frame, rasters the sprite
// plane and composes the Enhanced scene on the CPU (row helpers run on the ComposeHelper roles),
// then publishes pixels for the presenter. Latest wins: a slow composer drops frames and never
// delays emulation.
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
    std::vector<uint16_t> m_plane;
};
} // namespace f3rt::n3ds
