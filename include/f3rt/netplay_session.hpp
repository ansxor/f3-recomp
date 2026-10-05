#pragma once
#include "f3rt/netplay.hpp"
#include <string>

namespace f3rt::netplay {
// Transport is host-clock driven; advance() alone advances emulated time.
// Until handoff, each guest/lobby machine continues its own independent history.
class Session {
public:
    enum class Phase { Lobby, Preparing, WaitingSnapshot, Sending, Accepting, Playing, Finishing, Local };
    enum class Result { None, MatchEnded, FrameLimit, Disconnected, Error };
    Session(Machine &, const TransportOptions &, const Identity &, unsigned window = 16);
    ~Session();
    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    void pump();
    bool advance(const std::array<InputWord, 2> &local);
    size_t render_audio(int16_t *stereo, size_t max_frames);
    void disconnect(const std::string &reason = "Disconnected; local play resumed");
    // Finite capture/oracle limit in match-relative frames; zero is natural end only.
    void set_frame_limit(uint32_t frames);
    bool connected() const;
    bool synchronized() const;
    Phase phase() const;
    Result result() const;
    const Rollback *rollback() const;
    unsigned slot() const;
    unsigned delay() const;
    double rtt_ms() const;
    int frame_advantage() const;
    double transfer_progress() const;
    std::string status() const;
    const std::string &error() const;
    // Clear held host input and queued pre-handoff SDL audio at a timeline jump.
    bool take_discontinuity();
private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace f3rt::netplay
