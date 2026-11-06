#pragma once
#include <cstdint>
#include <functional>

namespace f3rt {
enum class ThreadRole : uint8_t { Emulation, AudioSynth, Composer, ComposeHelper };

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
    void join();

private:
    struct State;
    State *m_state = nullptr;
};

inline constexpr unsigned MAX_COMPOSE_HELPERS = 3;
unsigned compose_helper_count();

enum class HelperContention : uint8_t {
    None,
    Synth,
    Emulation,
};
HelperContention compose_helper_contention(unsigned index);
} // namespace f3rt
