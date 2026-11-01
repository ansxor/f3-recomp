// The Nintendo 3DS thread backend. CMake selects this file instead of thread_std.cpp when
// building for the 3DS; other platforms compile it to nothing.
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
    // The thread seam's contract forbids throwing bodies; an escaping exception must not unwind
    // through libctru's C entry frame.
    const std::unique_ptr<Entry> entry(static_cast<Entry *>(argument));
    try {
        entry->body();
    } catch (...) {
        std::terminate();
    }
}
// libctru priorities run 0x18 (most urgent) to 0x3F (idle). The std::thread default (core 0,
// 0x3F, 32 KiB) would starve HLE synthesis and overflow GameSprites::decode's 240 KiB frame.
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
        // The second helper only uses the emulation core's idle time.
        return index == 0 ? RoleSpec{0, 0x31, 256 * 1024} : RoleSpec{2, 0x3F, 256 * 1024};
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
    entry.release(); // the running thread owns the entry now
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
} // namespace f3rt

#endif // __3DS__
