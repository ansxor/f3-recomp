#pragma once
#include <cstdint>
#include <functional>

namespace f3rt {
// Roles name the work a thread performs so a platform can place it. Desktop threads ignore the
// role except for the emulation thread's macOS QoS class; the 3DS backend maps each role to a
// core, a priority and a stack size (thread_ctru.cpp).
enum class ThreadRole : uint8_t { Emulation, AudioSynth, Composer, ComposeHelper };

// Move-only thread with a joining destructor. The body runs on its own thread and must not throw.
class Thread {
public:
    Thread() = default;
    Thread(ThreadRole role, unsigned index, std::function<void()> body);
    ~Thread();
    Thread(Thread &&other) noexcept;
    Thread &operator=(Thread &&other) noexcept;
    Thread(const Thread &) = delete;
    Thread &operator=(const Thread &) = delete;

    bool joinable() const noexcept;
    // Waits for the body and releases the handle; a no-op when nothing is running.
    void join();

private:
    struct State;
    State *m_state = nullptr;
};

// Compose helpers available next to a caller that composes too. Never more than
// MAX_COMPOSE_HELPERS.
inline constexpr unsigned MAX_COMPOSE_HELPERS = 3;
unsigned compose_helper_count();

// The work a compose helper outranks on its core, and so delays for as long as one of its chunks
// runs. The row workers keep such a helper off its core when the delayed thread is due.
enum class HelperContention : uint8_t {
    None,      // nothing time-critical shares the core
    Synth,     // HLE synthesis, which must finish its block before the next frame's audio poll
    Emulation, // the emulation thread, which must start its next frame on the frame tick
};
HelperContention compose_helper_contention(unsigned index);
} // namespace f3rt
