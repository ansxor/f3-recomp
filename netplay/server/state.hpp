#pragma once

#include "netplay/shared/contract.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

namespace f3rt::netplay {

constexpr size_t MAX_CLIENTS = 64;
constexpr size_t MAX_ROOMS_ALLOWED = 32;

constexpr double HELLO_TIMEOUT_SECONDS = 5.0; // Shortened per security hardening
constexpr double LOBBY_IDLE_SECONDS = 600.0;  // 10 minutes
constexpr double ROOM_TTL_SECONDS = 600.0;    // 10 minutes

constexpr uint32_t MAX_SESSION_FRAMES = 1 << 20; // 1,048,576 frames
constexpr size_t MAX_CRC_PENDING = 256;
constexpr size_t MAX_RECORDED_MISMATCHES = 256;
constexpr size_t MAX_RECORDED_ERRORS = 256;

constexpr int CHAT_BURST = 5;
constexpr double CHAT_REFILL_PER_SEC = 1.0;

using ClientSlotId = int;

// Identity representing binary/ROM fingerprints
struct Identity {
    std::string game_id;
    std::array<uint32_t, ROM_CRCS_COUNT> rom_crcs{};
    std::array<uint8_t, BUILD_HASH_SIZE> build_hash{};

    bool operator==(const Identity &other) const = default;
};

// Records for session snapshot
struct CrcMismatchRecord {
    uint32_t frame = 0;
    uint32_t crc_p1 = 0;
    uint32_t crc_p2 = 0;
};

struct ResultRecord {
    uint32_t frame = 0;
    uint8_t winner = 0xFF; // 0 P1, 1 P2, 0xFF Draw
    uint32_t final_crc = 0;
};

struct ResultAckRecord {
    bool agreed = false;
    uint8_t winner = 0xFE; // 0 P1, 1 P2, 0xFF Draw, 0xFE Disagreed
};

struct ChatRecord {
    uint8_t player = 0;
    int64_t t_ms = 0;
    std::string text;
};

// Snapshot data emitted when a session ends, used for JSON log generation
struct SessionSnapshot {
    std::string session_id_hex;
    std::string started_at;
    std::string ended_at;
    std::string game_id;
    std::string build_hash_hex;
    std::array<uint32_t, ROM_CRCS_COUNT> rom_crcs{};
    std::string seed_hex;
    uint8_t input_delay = 2;
    std::array<std::string, 2> player_names;
    std::array<std::vector<int32_t>, 2> inputs; // Dense per frame (-1 represents null/missing)

    // CRC stats
    int crc_compared = 0;
    int crc_mismatch_count = 0;
    std::vector<CrcMismatchRecord> crc_mismatches;
    int crc_unmatched = 0;
    int crc_evicted = 0;

    std::array<std::optional<ResultRecord>, 2> results;
    std::optional<ResultAckRecord> result_ack;
    std::optional<std::string> sync_blob_hex;

    std::vector<std::string> protocol_errors;
    int protocol_errors_dropped = 0;

    std::string end_reason; // "bye", "disconnect", "protocol_error", "expired"
    int ended_by = -1;      // 0, 1, or -1

    std::vector<ChatRecord> chat;
    std::array<int, 2> chat_dropped{0, 0};
};

// --- Outbound Actions ---

struct ActionSendWelcome {
    ClientSlotId slot = -1;
};

struct ActionSendRoomCreated {
    ClientSlotId slot = -1;
    std::string room_code;
};

struct ActionSendMatchStart {
    ClientSlotId slot = -1;
    uint64_t session_id = 0;
    uint64_t seed = 0;
    uint8_t player_index = 0;
    uint8_t input_delay = 2;
    std::string peer_name;
};

struct ActionSendGo {
    ClientSlotId slot = -1;
};

struct ActionSendSyncBlobFwd {
    ClientSlotId slot = -1;
    std::vector<uint8_t> data;
};

struct ActionSendDesync {
    ClientSlotId slot = -1;
    uint32_t frame = 0;
    uint32_t crc_p1 = 0;
    uint32_t crc_p2 = 0;
};

struct ActionSendResultAck {
    ClientSlotId slot = -1;
    bool agreed = false;
    uint8_t winner = 0xFE;
};

struct ActionSendChatFwd {
    ClientSlotId slot = -1;
    uint8_t from_player = 0;
    std::string text;
};

struct ActionSendPeerLeft {
    ClientSlotId slot = -1;
};

struct ActionSendError {
    ClientSlotId slot = -1;
    NetplayErrorCode code = NetplayErrorCode::None;
    std::string message;
};

struct ActionSendInputs {
    ClientSlotId slot = -1;
    uint32_t first_frame = 0;
    uint8_t count = 0;
    std::vector<uint16_t> inputs;
};

struct ActionDisconnect {
    ClientSlotId slot = -1;
};

struct ActionWriteLog {
    SessionSnapshot snapshot;
};

using Action = std::variant<ActionSendWelcome, ActionSendRoomCreated, ActionSendMatchStart,
                            ActionSendGo, ActionSendSyncBlobFwd, ActionSendDesync,
                            ActionSendResultAck, ActionSendChatFwd, ActionSendPeerLeft,
                            ActionSendError, ActionSendInputs, ActionDisconnect, ActionWriteLog>;

// Token bucket for in-session chat
struct ChatBucket {
    double tokens = CHAT_BURST;
    double last_time = 0.0;

    bool allow(double now) {
        if (last_time <= 0.0) {
            tokens = CHAT_BURST;
        } else {
            double elapsed = now - last_time;
            if (elapsed > 0.0) {
                tokens = std::min(static_cast<double>(CHAT_BURST),
                                  tokens + elapsed * CHAT_REFILL_PER_SEC);
            }
        }
        last_time = now;
        if (tokens < 1.0) {
            return false;
        }
        tokens -= 1.0;
        return true;
    }
};

// Internal structures
enum class ClientState {
    Disconnected,
    WaitHello,
    LobbyIdle,
    InRoomHost,
    InSession,
};

struct ClientSessionInfo {
    ClientState state = ClientState::Disconnected;
    std::string player_name;
    Identity identity;
    std::string room_code;
    uint64_t session_id = 0;
    int role = -1; // 0 host, 1 joiner
    double deadline = 0.0;
    ChatBucket chat_bucket;
};

struct RoomInfo {
    std::string code;
    ClientSlotId host_slot = -1;
    Identity identity;
    uint8_t input_delay = 2;
    bool sync_required = false;
    double deadline = 0.0;
    uint64_t session_id = 0; // 0 if lobby only
};

struct CrcEntry {
    bool has[2]{false, false};
    uint32_t crc[2]{0, 0};
};

struct SessionInternal {
    uint64_t id = 0;
    uint64_t seed = 0;
    std::string room_code;
    std::array<ClientSlotId, 2> clients{-1, -1};
    std::array<std::string, 2> names;
    Identity identity;
    uint8_t input_delay = 2;
    bool sync_required = false;
    bool sync_seen = false;
    std::vector<uint8_t> sync_blob;
    bool go_sent = false;

    double started_time = 0.0;
    std::string started_at_iso;
    std::string ended_at_iso;
    std::string end_reason;
    int ended_by = -1;
    bool ended = false;

    std::array<std::vector<int32_t>, 2> inputs;
    std::map<uint32_t, CrcEntry> crc_pending;
    int crc_compared = 0;
    int crc_mismatch_count = 0;
    std::vector<CrcMismatchRecord> crc_mismatches;
    int crc_evicted = 0;
    bool desync_sent = false;

    std::array<std::optional<ResultRecord>, 2> results;
    std::optional<ResultAckRecord> result_ack;

    std::vector<ChatRecord> chat;
    std::array<int, 2> chat_dropped{0, 0};

    std::vector<std::string> protocol_errors;
    int protocol_errors_dropped = 0;

    void log_proto_err(std::string_view msg);
    SessionSnapshot make_snapshot() const;
};

class RelayStateMachine {
public:
    explicit RelayStateMachine(size_t max_rooms = MAX_ROOMS_ALLOWED);

    // Feed events into state machine; side-effects appended to out_actions
    void on_client_connected(ClientSlotId slot, double now, std::vector<Action> &out);
    void on_client_disconnected(ClientSlotId slot, std::string_view reason, double now,
                                std::vector<Action> &out);

    void on_hello(ClientSlotId slot, std::string_view game_id,
                  std::span<const uint32_t, ROM_CRCS_COUNT> rom_crcs,
                  std::span<const uint8_t, BUILD_HASH_SIZE> build_hash,
                  std::string_view player_name, double now, std::vector<Action> &out);

    void on_create_room(ClientSlotId slot, uint8_t input_delay, uint8_t flags, double now,
                        std::vector<Action> &out);
    void on_join_room(ClientSlotId slot, std::string_view room_code, double now,
                      std::vector<Action> &out);
    void on_sync_blob(ClientSlotId slot, std::span<const uint8_t> data, double now,
                      std::vector<Action> &out);
    void on_inputs(ClientSlotId slot, uint32_t first_frame, uint8_t count,
                   std::span<const uint16_t> inputs, double now, std::vector<Action> &out);
    void on_crc(ClientSlotId slot, uint32_t frame, uint32_t crc, double now,
                std::vector<Action> &out);
    void on_result(ClientSlotId slot, uint32_t frame, uint8_t winner, uint32_t final_crc,
                   double now, std::vector<Action> &out);
    void on_chat(ClientSlotId slot, std::string_view text, double now, std::vector<Action> &out);
    void on_bye(ClientSlotId slot, double now, std::vector<Action> &out);

    // Periodic timer tick
    void tick(double now, std::vector<Action> &out);

    // Query state
    size_t active_rooms_count() const { return m_rooms.size(); }
    size_t active_sessions_count() const { return m_sessions.size(); }
    const ClientSessionInfo &client_info(ClientSlotId slot) const { return m_clients[slot]; }

private:
    void fail_client(ClientSlotId slot, NetplayErrorCode code, std::string_view msg, double now,
                     std::vector<Action> &out);
    void end_session(SessionInternal &sess, std::string_view reason, int ended_by, double now,
                     std::vector<Action> &out);
    void remove_room(std::string_view code);
    std::string generate_unique_code();
    uint64_t generate_unique_session_id();
    void maybe_send_go(SessionInternal &sess, std::vector<Action> &out);

    size_t m_max_rooms = MAX_ROOMS_ALLOWED;
    std::array<ClientSessionInfo, MAX_CLIENTS> m_clients{};
    std::map<std::string, RoomInfo> m_rooms;
    std::map<uint64_t, SessionInternal> m_sessions;
};

} // namespace f3rt::netplay
