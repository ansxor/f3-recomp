#if defined(__3DS__)
#include "platform/thread.hpp"
#include <3ds.h>
#include <cstddef>
#include <cstdio>
#include <exception>
#include <memory>
#include <stdexcept>
#include <utility>

namespace f3rt {
namespace {
struct Entry {
    std::function<void()> body;
};
void trampoline(void *argument) {
    const std::unique_ptr<Entry> entry(static_cast<Entry *>(argument));
    try {
        entry->body();
    } catch (...) {
        std::terminate();
    }
}
// libctru priorities run 0x18 (most urgent) to 0x3F (idle). The std::thread default (core 0,
// 0x3F, 32 KiB) would starve HLE synthesis and overflow GameSprites::decode's 240 KiB frame.
//
// Core map (New 3DS; core 2 exists only there, and a thread refused it falls back to core 0):
//   core 0  the presenter (main thread, 0x30), HLE synthesis (0x2C) and compose helper 0 (0x2B).
//   core 1  the composer (0x30). It is the system core: APT_SetAppCpuTimeLimit(80) caps the
//           application's share of it at 80%, so the composer must not be the only participant that
//           can finish a frame.
//   core 2  emulation (0x30) and compose helper 1 (0x2F). Emulation sleeps on the frame tick for
//           whatever its frame leaves of the period, and that idle time is the helper's.
struct RoleSpec {
    int core;
    int priority;
    size_t stack;
};
RoleSpec role_spec(ThreadRole role, unsigned index) {
    switch (role) {
    case ThreadRole::Emulation:
        return {2, 0x30, 1024 * 1024};
    case ThreadRole::AudioSynth:
        return {0, 0x2C, 256 * 1024};
    case ThreadRole::Composer:
        return {1, 0x30, 256 * 1024};
    case ThreadRole::ComposeHelper:
        return index == 0 ? RoleSpec{0, 0x2B, 256 * 1024} : RoleSpec{2, 0x2F, 256 * 1024};
    }
    return {0, 0x30, 256 * 1024};
}
} // namespace

struct Thread::State {
    ::Thread native = nullptr;
};

Thread::Thread(ThreadRole role, unsigned index, std::function<void()> body) {
    const RoleSpec spec = role_spec(role, index);
    auto state = std::make_unique<State>();
    auto entry = std::make_unique<Entry>(Entry{std::move(body)});
    state->native =
        threadCreate(trampoline, entry.get(), spec.stack, spec.priority, spec.core, false);
    if (!state->native && spec.core != 0) {
        std::fprintf(stderr, "thread role=%d core=%d fallback=0\n", int(role), spec.core);
        state->native =
            threadCreate(trampoline, entry.get(), spec.stack, spec.priority, 0, false);
    }
    if (!state->native) throw std::runtime_error("threadCreate failed");
    entry.release();
    m_state = state.release();
}
Thread::~Thread() { join(); }
Thread::Thread(Thread &&other) noexcept : m_state(std::exchange(other.m_state, nullptr)) {}
Thread &Thread::operator=(Thread &&other) noexcept {
    if (this == &other) return *this;
    join();
    m_state = std::exchange(other.m_state, nullptr);
    return *this;
}
bool Thread::joinable() const noexcept { return m_state && m_state->native; }
void Thread::join() {
    if (!m_state) return;
    if (m_state->native) {
        threadJoin(m_state->native, U64_MAX);
        threadFree(m_state->native);
    }
    delete std::exchange(m_state, nullptr);
}

unsigned compose_helper_count() { return 2; }

HelperContention compose_helper_contention(unsigned index) {
    return index == 0 ? HelperContention::Synth : HelperContention::Emulation;
}
} // namespace f3rt

#endif // __3DS__
