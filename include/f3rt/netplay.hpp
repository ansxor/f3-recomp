#pragma once

#include "f3rt/input.hpp"
#include <array>
#include <chrono>
#include <functional>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace f3rt {
class Machine;
struct GameNetplay;
} // namespace f3rt

namespace f3rt::netplay {

class MatchRecorder;

constexpr uint16_t default_port = 7845;

enum class SessionState {
    Idle,
    Connecting,
    Lobby,          // or WaitingForPeer
    Registering,
    Running,
    Ended
};

enum class EndReason {
    None,
    Completed,
    PeerLeft,
    Desync,
    Error,
    Disconnected
};

struct DesyncInfo {
    bool detected = false;
    uint32_t frame = 0;
    uint32_t crc_p1 = 0;
    uint32_t crc_p2 = 0;
};

struct NetplayStats {
    uint32_t ping_ms = 0;
    uint64_t stall_count = 0;
    uint64_t packets_sent = 0;
    uint64_t packets_received = 0;
    uint64_t packets_dropped = 0;
};

struct ChatMessage {
    uint8_t from_player = 0;
    std::string text;
};

struct SessionConfig {
    std::string server_host;
    uint16_t server_port = default_port;
    std::string game_id;
    std::array<uint32_t, 7> rom_crcs{};
    std::array<uint8_t, 32> build_hash{};
    std::string player_name = "player";
    bool is_host = false;
    uint8_t input_delay = 2;
    uint8_t flags = 0; // bit 0 = sync blob required
    std::string room_code; // for joiner
    uint32_t crc_interval = 60;

    // Sync callbacks
    std::function<std::vector<uint8_t>()> capture_sync;
    std::function<void(std::span<const uint8_t>, uint64_t seed)> apply_sync;
    std::function<uint32_t()> sync_hash;
};

class SessionImpl;

class Session {
public:
    explicit Session(SessionConfig config);
    ~Session();

    Session(const Session &) = delete;
    Session &operator=(const Session &) = delete;
    Session(Session &&) noexcept;
    Session &operator=(Session &&) noexcept;

    void start();
    void poll();

    SessionState state() const;
    EndReason end_reason() const;

    uint64_t session_id() const;
    const std::string &room_code() const;
    uint64_t seed() const;
    uint8_t player_index() const;
    uint8_t input_delay() const;
    const std::string &peer_name() const;
    const std::string &last_error() const;
    const DesyncInfo &desync_info() const;
    const NetplayStats &stats() const;
    uint16_t local_udp_port() const;

    // Returns true when both local and remote inputs are known for frame `frame`.
    bool ready_to_step(uint32_t frame) const;

    // Returns inputs for frame `frame` mapped by player_index:
    // [0] = P1 (player 0), [1] = P2 (player 1)
    std::array<LocalInputWord, 2> inputs_for(uint32_t frame) const;
    std::array<LocalInputWord, local_player_count> all_inputs_for(uint32_t frame) const;

    // Submits local input sampled at match frame `frame`.
    // Scheduled for frame `frame + input_delay`.
    void submit_local(uint32_t frame, LocalInputWord word);
    void submit_local(LocalInputWord word);

    void report_crc(uint32_t frame, uint32_t crc);
    void send_result(uint32_t frame, uint8_t winner, uint32_t final_crc);
    void send_bye();
    void close();
    bool send_chat(std::string_view text);
    std::vector<ChatMessage> drain_chat();

private:
    std::unique_ptr<SessionImpl> impl_;
};

// Applies `words` (P1/P2 ports), runs the game hook's per-frame write, then one emulated frame. This is the single place
// that defines lockstep frame semantics: apply inputs -> on_match_frame -> run_frame -> audio finish. The frontend, the
// headless tool and `f3rt-tool netplay-replay` all go through it.
bool run_lockstep_frame(Machine &m, const std::array<LocalInputWord, local_player_count> &words, uint64_t seed,
                        uint32_t match_frame, const GameNetplay *hook);

// Shared lockstep frame step function used by both frontend and headless tool.
// `recorder` (optional, local only) logs the applied inputs and, at CRC report frames, the hashed bytes + a snapshot.
bool step_lockstep_frame(Machine &m, Session &session, uint32_t match_frame,
                         const GameNetplay *hook, uint32_t crc_interval = 60, MatchRecorder *recorder = nullptr);

} // namespace f3rt::netplay
