#include "platform/thread.hpp"
#include <algorithm>
#include <thread>
#include <utility>
#ifdef __APPLE__
#include <pthread.h>
#endif

namespace f3rt {
struct Thread::State {
    std::thread thread;
};
Thread::Thread(ThreadRole role, unsigned, std::function<void()> body) {
    auto *state = new State;
    try {
        state->thread = std::thread([role, body = std::move(body)] {
#ifdef __APPLE__
            // Default-QoS threads sleeping ~14 ms between frames wake 1-4 ms late under timer
            // coalescing, which skews both the frame start and the input sampling lead;
            // user-interactive QoS wakes within a few hundred microseconds.
            if (role == ThreadRole::Emulation)
                pthread_set_qos_class_self_np(QOS_CLASS_USER_INTERACTIVE, 0);
#else
            (void)role;
#endif
            body();
        });
    } catch (...) {
        delete state;
        throw;
    }
    m_state = state;
}
Thread::~Thread() { join(); }
Thread::Thread(Thread &&other) noexcept : m_state(std::exchange(other.m_state, nullptr)) {}
Thread &Thread::operator=(Thread &&other) noexcept {
    if (this == &other) return *this;
    join();
    m_state = std::exchange(other.m_state, nullptr);
    return *this;
}
bool Thread::joinable() const noexcept { return m_state && m_state->thread.joinable(); }
void Thread::join() {
    if (!m_state) return;
    if (m_state->thread.joinable()) m_state->thread.join();
    delete std::exchange(m_state, nullptr);
}

unsigned compose_helper_count() {
    const unsigned hardware = std::thread::hardware_concurrency();
    return hardware ? std::min(MAX_COMPOSE_HELPERS, hardware - 1) : 1;
}

HelperContention compose_helper_contention(unsigned) { return HelperContention::None; }
} // namespace f3rt
