#include "state.hpp"
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <iomanip>
#include <random>
#include <sstream>

namespace f3rt::netplay {

namespace {

uint64_t random_u64() {
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(rd());
    return gen();
}

std::string format_iso_now() {
    using namespace std::chrono;
    auto now = system_clock::now();
    auto tt = system_clock::to_time_t(now);
    auto ms = duration_cast<nanoseconds>(now.time_since_epoch()) % 1'000'000'000;
    std::tm tm_buf{};
    gmtime_r(&tt, &tm_buf);

    std::ostringstream ss;
    ss << std::put_time(&tm_buf, "%Y-%m-%dT%H:%M:%S") << '.' << std::setfill('0') << std::setw(9)
       << ms.count() << 'Z';
    return ss.str();
}

std::string format_hex64(uint64_t val) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0') << std::setw(16) << val;
    return ss.str();
}

std::string format_hex_bytes(std::span<const uint8_t> data) {
    std::ostringstream ss;
    ss << std::hex << std::setfill('0');
    for (uint8_t b : data) {
        ss << std::setw(2) << static_cast<int>(b);
    }
    return ss.str();
}

} // namespace

void SessionInternal::log_proto_err(std::string_view msg) {
    if (protocol_errors.size() < MAX_RECORDED_ERRORS) {
        protocol_errors.emplace_back(msg);
    } else {
        protocol_errors_dropped++;
    }
}

SessionSnapshot SessionInternal::make_snapshot() const {
    SessionSnapshot snap;
    snap.session_id_hex = format_hex64(id);
    snap.started_at = started_at_iso;
    snap.ended_at = ended_at_iso;
    snap.game_id = identity.game_id;
    snap.build_hash_hex = format_hex_bytes(identity.build_hash);
    snap.rom_crcs = identity.rom_crcs;
    snap.seed_hex = format_hex64(seed);
    snap.input_delay = input_delay;
    snap.player_names = names;
    snap.inputs = inputs;

    snap.crc_compared = crc_compared;
    snap.crc_mismatch_count = crc_mismatch_count;
    snap.crc_mismatches = crc_mismatches;
    snap.crc_unmatched = static_cast<int>(crc_pending.size());
    snap.crc_evicted = crc_evicted;

    snap.results = results;
    snap.result_ack = result_ack;

    if (!sync_blob.empty()) {
        snap.sync_blob_hex = format_hex_bytes(sync_blob);
    }

    snap.protocol_errors = protocol_errors;
    snap.protocol_errors_dropped = protocol_errors_dropped;

    snap.end_reason = end_reason;
    snap.ended_by = ended_by;

    snap.chat = chat;
    snap.chat_dropped = chat_dropped;
    return snap;
}

RelayStateMachine::RelayStateMachine(size_t max_rooms)
    : m_max_rooms(std::clamp(max_rooms, size_t{1}, MAX_ROOMS_ALLOWED)) {}

std::string RelayStateMachine::generate_unique_code() {
    std::string code;
    code.resize(ROOM_CODE_LENGTH);
    for (;;) {
        uint64_t r = random_u64();
        for (size_t i = 0; i < ROOM_CODE_LENGTH; ++i) {
            code[i] = ROOM_CODE_ALPHABET[(r >> (i * 5)) & 31];
        }
        if (!m_rooms.contains(code)) {
            return code;
        }
    }
}

uint64_t RelayStateMachine::generate_unique_session_id() {
    for (;;) {
        uint64_t id = random_u64();
        if (id != 0 && !m_sessions.contains(id)) {
            return id;
        }
    }
}

void RelayStateMachine::fail_client(ClientSlotId slot, NetplayErrorCode code, std::string_view msg,
                                    double now, std::vector<Action> &out) {
    out.push_back(ActionSendError{slot, code, std::string(msg)});
    on_client_disconnected(slot, "protocol_error", now, out);
}

void RelayStateMachine::remove_room(std::string_view code) { m_rooms.erase(std::string(code)); }

void RelayStateMachine::maybe_send_go(SessionInternal &sess, std::vector<Action> &out) {
    if (sess.go_sent) {
        return;
    }
    if (sess.sync_required && !sess.sync_seen) {
        return;
    }
    sess.go_sent = true;
    for (ClientSlotId c_slot : sess.clients) {
        if (c_slot >= 0) {
            out.push_back(ActionSendGo{c_slot});
        }
    }
}

void RelayStateMachine::on_client_connected(ClientSlotId slot, double now,
                                            std::vector<Action> & /*out*/) {
    if (slot < 0 || slot >= static_cast<ClientSlotId>(MAX_CLIENTS)) {
        return;
    }
    auto &c = m_clients[slot];
    c = ClientSessionInfo{};
    c.state = ClientState::WaitHello;
    c.deadline = now + HELLO_TIMEOUT_SECONDS;
}

void RelayStateMachine::on_client_disconnected(ClientSlotId slot, std::string_view reason,
                                               double now, std::vector<Action> &out) {
    if (slot < 0 || slot >= static_cast<ClientSlotId>(MAX_CLIENTS)) {
        return;
    }
    auto &c = m_clients[slot];
    if (c.state == ClientState::Disconnected) {
        return;
    }

    out.push_back(ActionDisconnect{slot});

    if (c.state == ClientState::InRoomHost && !c.room_code.empty()) {
        remove_room(c.room_code);
    } else if (c.state == ClientState::InSession && c.session_id != 0) {
        auto it = m_sessions.find(c.session_id);
        if (it != m_sessions.end()) {
            end_session(it->second, reason, c.role, now, out);
        }
    }

    c = ClientSessionInfo{};
    c.state = ClientState::Disconnected;
}

void RelayStateMachine::end_session(SessionInternal &sess, std::string_view reason, int ended_by,
                                    double now, std::vector<Action> &out) {
    (void)now;
    if (sess.ended) {
        return;
    }
    sess.ended = true;
    sess.end_reason = std::string(reason);
    sess.ended_by = ended_by;
    sess.ended_at_iso = format_iso_now();

    // Notify peer if still connected
    for (int role = 0; role < 2; ++role) {
        ClientSlotId slot = sess.clients[role];
        if (slot >= 0 && role != ended_by) {
            out.push_back(ActionSendPeerLeft{slot});
        }
    }

    // Clean up room
    remove_room(sess.room_code);

    // Emit session log write action
    out.push_back(ActionWriteLog{sess.make_snapshot()});

    // Remove session
    m_sessions.erase(sess.id);
}

void RelayStateMachine::on_hello(ClientSlotId slot, std::string_view game_id,
                                 std::span<const uint32_t, ROM_CRCS_COUNT> rom_crcs,
                                 std::span<const uint8_t, BUILD_HASH_SIZE> build_hash,
                                 std::string_view player_name, double now,
                                 std::vector<Action> &out) {
    if (slot < 0 || slot >= static_cast<ClientSlotId>(MAX_CLIENTS)) {
        return;
    }
    auto &c = m_clients[slot];
    if (c.state != ClientState::WaitHello) {
        fail_client(slot, NetplayErrorCode::BadMessage, "duplicate or unexpected HELLO", now, out);
        return;
    }

    if (!validate_player_name(player_name)) {
        fail_client(slot, NetplayErrorCode::BadMessage, "player name must be 1 to 24 bytes", now,
                    out);
        return;
    }

    if (game_id.empty() || game_id.size() > MAX_GAME_ID_LENGTH) {
        fail_client(slot, NetplayErrorCode::BadMessage, "invalid game_id length", now, out);
        return;
    }

    c.state = ClientState::LobbyIdle;
    c.player_name = std::string(player_name);
    c.identity.game_id = std::string(game_id);
    std::copy_n(rom_crcs.data(), ROM_CRCS_COUNT, c.identity.rom_crcs.begin());
    std::copy_n(build_hash.data(), BUILD_HASH_SIZE, c.identity.build_hash.begin());
    c.deadline = now + LOBBY_IDLE_SECONDS;

    out.push_back(ActionSendWelcome{slot});
}

void RelayStateMachine::on_create_room(ClientSlotId slot, uint8_t input_delay, uint8_t flags,
                                       double now, std::vector<Action> &out) {
    if (slot < 0 || slot >= static_cast<ClientSlotId>(MAX_CLIENTS)) {
        return;
    }
    auto &c = m_clients[slot];
    if (c.state != ClientState::LobbyIdle) {
        fail_client(slot, NetplayErrorCode::NotInLobby, "already in a room or not in lobby", now,
                    out);
        return;
    }

    if (input_delay < 1 || input_delay > 8) {
        fail_client(slot, NetplayErrorCode::BadMessage, "input_delay out of range", now, out);
        return;
    }

    if ((flags & ~0x01) != 0) {
        fail_client(slot, NetplayErrorCode::BadMessage, "unknown CREATE_ROOM flags", now, out);
        return;
    }

    if (m_rooms.size() >= m_max_rooms) {
        fail_client(slot, NetplayErrorCode::ServerFull, "server full", now, out);
        return;
    }

    std::string code = generate_unique_code();
    RoomInfo room;
    room.code = code;
    room.host_slot = slot;
    room.identity = c.identity;
    room.input_delay = input_delay;
    room.sync_required = (flags & 0x01) != 0;
    room.deadline = now + ROOM_TTL_SECONDS;

    m_rooms[code] = room;

    c.state = ClientState::InRoomHost;
    c.room_code = code;
    c.role = 0;
    c.deadline = now + ROOM_TTL_SECONDS;

    out.push_back(ActionSendRoomCreated{slot, code});
}

void RelayStateMachine::on_join_room(ClientSlotId slot, std::string_view room_code, double now,
                                     std::vector<Action> &out) {
    if (slot < 0 || slot >= static_cast<ClientSlotId>(MAX_CLIENTS)) {
        return;
    }
    auto &c = m_clients[slot];
    if (c.state != ClientState::LobbyIdle) {
        fail_client(slot, NetplayErrorCode::NotInLobby, "already in a room", now, out);
        return;
    }

    if (!validate_room_code(room_code)) {
        fail_client(slot, NetplayErrorCode::BadMessage, "room code has invalid characters", now,
                    out);
        return;
    }

    auto it = m_rooms.find(std::string(room_code));
    if (it == m_rooms.end()) {
        fail_client(slot, NetplayErrorCode::RoomNotFound, "room not found", now, out);
        return;
    }

    RoomInfo &room = it->second;
    if (room.session_id != 0) {
        fail_client(slot, NetplayErrorCode::RoomFull, "room full", now, out);
        return;
    }

    // Verify identity
    if (room.identity != c.identity) {
        fail_client(slot, NetplayErrorCode::IdentityMismatch,
                    "game, ROM or build differs from host", now, out);
        return;
    }

    ClientSlotId host_slot = room.host_slot;
    auto &host = m_clients[host_slot];

    uint64_t session_id = generate_unique_session_id();
    uint64_t seed = random_u64();

    SessionInternal sess;
    sess.id = session_id;
    sess.seed = seed;
    sess.room_code = room.code;
    sess.clients = {host_slot, slot};
    sess.names = {host.player_name, c.player_name};
    sess.identity = room.identity;
    sess.input_delay = room.input_delay;
    sess.sync_required = room.sync_required;
    sess.started_time = now;
    sess.started_at_iso = format_iso_now();

    m_sessions[session_id] = std::move(sess);
    room.session_id = session_id;

    // Transition host & joiner to InSession
    host.state = ClientState::InSession;
    host.session_id = session_id;
    host.role = 0;
    host.deadline = 0.0; // No idle timeout during match

    c.state = ClientState::InSession;
    c.room_code = room.code;
    c.session_id = session_id;
    c.role = 1;
    c.deadline = 0.0;

    // Send MatchStart to both
    out.push_back(
        ActionSendMatchStart{host_slot, session_id, seed, 0, room.input_delay, c.player_name});
    out.push_back(
        ActionSendMatchStart{slot, session_id, seed, 1, room.input_delay, host.player_name});

    // If sync blob is not required, GO is sent immediately
    auto sess_it = m_sessions.find(session_id);
    if (sess_it != m_sessions.end()) {
        maybe_send_go(sess_it->second, out);
    }
}

void RelayStateMachine::on_sync_blob(ClientSlotId slot, std::span<const uint8_t> data, double now,
                                     std::vector<Action> &out) {
    if (slot < 0 || slot >= static_cast<ClientSlotId>(MAX_CLIENTS)) {
        return;
    }
    auto &c = m_clients[slot];
    if (c.state != ClientState::InSession || c.session_id == 0) {
        fail_client(slot, NetplayErrorCode::BadMessage, "SYNC_BLOB outside a match", now, out);
        return;
    }

    if (c.role != 0) {
        fail_client(slot, NetplayErrorCode::BadMessage, "only the host may send SYNC_BLOB", now,
                    out);
        return;
    }

    auto it = m_sessions.find(c.session_id);
    if (it == m_sessions.end()) {
        return;
    }
    SessionInternal &sess = it->second;

    if (!sess.sync_required) {
        fail_client(slot, NetplayErrorCode::BadMessage, "room does not require a sync blob", now,
                    out);
        return;
    }

    if (sess.go_sent) {
        fail_client(slot, NetplayErrorCode::BadMessage, "SYNC_BLOB after GO", now, out);
        return;
    }

    if (sess.sync_seen) {
        fail_client(slot, NetplayErrorCode::BadMessage, "duplicate SYNC_BLOB", now, out);
        return;
    }

    if (data.size() < MIN_SYNC_BLOB_SIZE || data.size() > MAX_SYNC_BLOB_SIZE) {
        fail_client(slot, NetplayErrorCode::BadMessage, "SYNC_BLOB length must be 1 to 2048", now,
                    out);
        return;
    }

    sess.sync_seen = true;
    sess.sync_blob.assign(data.begin(), data.end());

    ClientSlotId joiner_slot = sess.clients[1];
    if (joiner_slot >= 0) {
        out.push_back(ActionSendSyncBlobFwd{joiner_slot, sess.sync_blob});
    }

    maybe_send_go(sess, out);
}

void RelayStateMachine::on_inputs(ClientSlotId slot, uint32_t first_frame, uint8_t count,
                                  std::span<const uint16_t> inputs, double /*now*/,
                                  std::vector<Action> &out) {
    if (slot < 0 || slot >= static_cast<ClientSlotId>(MAX_CLIENTS)) {
        return;
    }
    auto &c = m_clients[slot];
    if (c.state != ClientState::InSession || c.session_id == 0) {
        return;
    }

    auto it = m_sessions.find(c.session_id);
    if (it == m_sessions.end()) {
        return;
    }
    SessionInternal &sess = it->second;

    if (count < MIN_INPUTS_COUNT || count > MAX_INPUTS_COUNT || inputs.size() != count) {
        sess.log_proto_err("inputs: bad count");
        return;
    }

    if (static_cast<uint64_t>(first_frame) + count > MAX_SESSION_FRAMES) {
        sess.log_proto_err("inputs: frame out of range");
        return;
    }

    int p = c.role;
    auto &log = sess.inputs[p];

    // Conflict detection: verify re-sent frames have identical inputs
    for (size_t i = 0; i < count; ++i) {
        size_t f = static_cast<size_t>(first_frame) + i;
        uint16_t val = inputs[i];
        if (f < log.size() && log[f] >= 0 && static_cast<uint16_t>(log[f]) != val) {
            char buf[128];
            std::snprintf(buf, sizeof(buf), "inputs from player %d: conflicting input at frame %zu",
                          p, f);
            sess.log_proto_err(buf);
            return; // Drop packet completely without changing state or forwarding
        }
    }

    // Extend dense log if needed, filling gaps with -1
    size_t needed = static_cast<size_t>(first_frame) + count;
    if (log.size() < needed) {
        log.resize(needed, -1);
    }
    for (size_t i = 0; i < count; ++i) {
        log[static_cast<size_t>(first_frame) + i] = static_cast<int32_t>(inputs[i]);
    }

    // Forward to peer
    ClientSlotId peer_slot = sess.clients[1 - p];
    if (peer_slot >= 0) {
        std::vector<uint16_t> input_vec(inputs.begin(), inputs.end());
        out.push_back(ActionSendInputs{peer_slot, first_frame, count, std::move(input_vec)});
    }
}

void RelayStateMachine::on_crc(ClientSlotId slot, uint32_t frame, uint32_t crc, double now,
                               std::vector<Action> &out) {
    if (slot < 0 || slot >= static_cast<ClientSlotId>(MAX_CLIENTS)) {
        return;
    }
    auto &c = m_clients[slot];
    if (c.state != ClientState::InSession || c.session_id == 0) {
        fail_client(slot, NetplayErrorCode::BadMessage, "CRC outside a match", now, out);
        return;
    }

    if (frame >= MAX_SESSION_FRAMES) {
        return; // Ignore frames out of range
    }

    auto it = m_sessions.find(c.session_id);
    if (it == m_sessions.end()) {
        return;
    }
    SessionInternal &sess = it->second;

    auto crc_it = sess.crc_pending.find(frame);
    if (crc_it == sess.crc_pending.end()) {
        if (sess.crc_pending.size() >= MAX_CRC_PENDING) {
            // Evict lowest frame
            sess.crc_pending.erase(sess.crc_pending.begin());
            sess.crc_evicted++;
        }
        crc_it = sess.crc_pending.emplace(frame, CrcEntry{}).first;
    }

    CrcEntry &entry = crc_it->second;
    int p = c.role;
    if (entry.has[p]) {
        return; // First report wins
    }
    entry.has[p] = true;
    entry.crc[p] = crc;

    if (!entry.has[0] || !entry.has[1]) {
        return; // Wait for other player's report
    }

    // Both players reported: compare!
    uint32_t c0 = entry.crc[0];
    uint32_t c1 = entry.crc[1];
    sess.crc_pending.erase(crc_it);
    sess.crc_compared++;

    if (c0 != c1) {
        sess.crc_mismatch_count++;
        if (sess.crc_mismatches.size() < MAX_RECORDED_MISMATCHES) {
            sess.crc_mismatches.push_back(CrcMismatchRecord{frame, c0, c1});
        }
        if (!sess.desync_sent) {
            sess.desync_sent = true;
            for (ClientSlotId s : sess.clients) {
                if (s >= 0) {
                    out.push_back(ActionSendDesync{s, frame, c0, c1});
                }
            }
        }
    }
}

void RelayStateMachine::on_result(ClientSlotId slot, uint32_t frame, uint8_t winner,
                                  uint32_t final_crc, double now, std::vector<Action> &out) {
    if (slot < 0 || slot >= static_cast<ClientSlotId>(MAX_CLIENTS)) {
        return;
    }
    auto &c = m_clients[slot];
    if (c.state != ClientState::InSession || c.session_id == 0) {
        fail_client(slot, NetplayErrorCode::BadMessage, "RESULT outside a match", now, out);
        return;
    }

    if (winner != 0 && winner != 1 && winner != 0xFF) {
        fail_client(slot, NetplayErrorCode::BadMessage, "invalid winner", now, out);
        return;
    }

    auto it = m_sessions.find(c.session_id);
    if (it == m_sessions.end()) {
        return;
    }
    SessionInternal &sess = it->second;

    int p = c.role;
    if (sess.results[p].has_value()) {
        return; // Only first RESULT from each player counts
    }
    sess.results[p] = ResultRecord{frame, winner, final_crc};

    const auto &res0 = sess.results[0];
    const auto &res1 = sess.results[1];
    if (!res0.has_value() || !res1.has_value() || sess.result_ack.has_value()) {
        return; // Wait for both
    }

    const auto &r0 = *res0;
    const auto &r1 = *res1;
    bool agreed = (r0.frame == r1.frame && r0.winner == r1.winner && r0.final_crc == r1.final_crc);
    uint8_t ack_winner = agreed ? r0.winner : 0xFE;

    sess.result_ack = ResultAckRecord{agreed, ack_winner};

    for (ClientSlotId s : sess.clients) {
        if (s >= 0) {
            out.push_back(ActionSendResultAck{s, agreed, ack_winner});
        }
    }
}

void RelayStateMachine::on_chat(ClientSlotId slot, std::string_view text, double now,
                                std::vector<Action> &out) {
    if (slot < 0 || slot >= static_cast<ClientSlotId>(MAX_CLIENTS)) {
        return;
    }
    auto &c = m_clients[slot];
    if (c.state != ClientState::InSession || c.session_id == 0) {
        // CHAT before MATCH_START gets Error(NotInLobby) and fatal disconnect
        fail_client(slot, NetplayErrorCode::NotInLobby, "CHAT before MATCH_START", now, out);
        return;
    }

    if (!validate_chat_text(text)) {
        fail_client(slot, NetplayErrorCode::BadMessage, "malformed CHAT", now, out);
        return;
    }

    auto it = m_sessions.find(c.session_id);
    if (it == m_sessions.end()) {
        return;
    }
    SessionInternal &sess = it->second;

    int p = c.role;
    if (!c.chat_bucket.allow(now)) {
        sess.chat_dropped[p]++;
        return; // Silent drop on rate limit exhaustion
    }

    int64_t t_ms = static_cast<int64_t>((now - sess.started_time) * 1000.0);
    sess.chat.push_back(ChatRecord{static_cast<uint8_t>(p), t_ms, std::string(text)});

    ClientSlotId peer_slot = sess.clients[1 - p];
    if (peer_slot >= 0) {
        out.push_back(ActionSendChatFwd{peer_slot, static_cast<uint8_t>(p), std::string(text)});
    }
}

void RelayStateMachine::on_bye(ClientSlotId slot, double now, std::vector<Action> &out) {
    if (slot < 0 || slot >= static_cast<ClientSlotId>(MAX_CLIENTS)) {
        return;
    }
    auto &c = m_clients[slot];
    if (c.state == ClientState::InSession && c.session_id != 0) {
        auto it = m_sessions.find(c.session_id);
        if (it != m_sessions.end()) {
            end_session(it->second, "bye", c.role, now, out);
        }
    }
    on_client_disconnected(slot, "bye", now, out);
}

void RelayStateMachine::tick(double now, std::vector<Action> &out) {
    // 1. Check client timeouts
    for (size_t i = 0; i < MAX_CLIENTS; ++i) {
        auto &c = m_clients[i];
        if (c.state == ClientState::WaitHello && c.deadline > 0.0 && now >= c.deadline) {
            on_client_disconnected(static_cast<ClientSlotId>(i), "timeout_hello", now, out);
        } else if (c.state == ClientState::LobbyIdle && c.deadline > 0.0 && now >= c.deadline) {
            on_client_disconnected(static_cast<ClientSlotId>(i), "timeout_lobby", now, out);
        }
    }

    // 2. Check room TTLs
    std::vector<std::string> expired_rooms;
    for (auto &[code, r] : m_rooms) {
        if (r.session_id == 0 && r.deadline > 0.0 && now >= r.deadline) {
            expired_rooms.push_back(code);
        }
    }

    for (const auto &code : expired_rooms) {
        auto it = m_rooms.find(code);
        if (it != m_rooms.end()) {
            ClientSlotId host_slot = it->second.host_slot;
            m_rooms.erase(it);
            if (host_slot >= 0 && host_slot < static_cast<ClientSlotId>(MAX_CLIENTS)) {
                // Host is cleanly disconnected without Error
                on_client_disconnected(host_slot, "expired", now, out);
            }
        }
    }
}

} // namespace f3rt::netplay
