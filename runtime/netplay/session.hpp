#pragma once

#include "client.hpp"
#include "f3rt/netplay.hpp"
#include <chrono>
#include <vector>

namespace f3rt::netplay {

class SessionImpl {
public:
    explicit SessionImpl(SessionConfig config);
    ~SessionImpl();

    void start();
    void poll();
    bool handle_running_control(yojimbo::Message *msg);

    SessionState state() const { return m_state; }
    EndReason end_reason() const { return m_end_reason; }

    uint64_t session_id() const { return m_session_id; }
    const std::string &room_code() const { return m_room_code; }
    uint64_t seed() const { return m_seed; }
    uint8_t player_index() const { return m_player_index; }
    uint8_t input_delay() const { return m_input_delay; }
    const std::string &peer_name() const { return m_peer_name; }
    const std::string &last_error() const { return m_last_error; }
    const DesyncInfo &desync_info() const { return m_desync_info; }
    const NetplayStats &stats() const {
        const NetplayStats net = m_client.stats();
        m_stats.ping_ms = net.ping_ms;
        m_stats.packets_sent = net.packets_sent;
        m_stats.packets_received = net.packets_received;
        m_stats.packets_dropped = net.packets_dropped;
        return m_stats;
    }

    bool ready_to_step(uint32_t frame) const;
    std::array<LocalInputWord, 2> inputs_for(uint32_t frame) const;
    std::array<LocalInputWord, LOCAL_PLAYER_COUNT> all_inputs_for(uint32_t frame) const;

    void submit_local(uint32_t frame, LocalInputWord word);
    void submit_local(LocalInputWord word);

    void report_crc(uint32_t frame, uint32_t crc);
    void send_result(uint32_t frame, uint8_t winner, uint32_t final_crc);
    void send_bye();
    void close();
    bool send_chat(std::string_view text);
    std::vector<ChatMessage> drain_chat();

private:
    void send_inputs_packet(uint32_t current_frame);
    void record_remote_input(uint32_t frame, uint16_t word);

    SessionConfig m_config;
    Client m_client;

    SessionState m_state = SessionState::Idle;
    EndReason m_end_reason = EndReason::None;

    bool m_hello_sent = false;
    std::string m_room_code;
    uint64_t m_session_id = 0;
    uint64_t m_seed = 0;
    uint8_t m_player_index = 0;
    uint8_t m_input_delay = 2;
    std::string m_peer_name;
    bool m_sync_blob_sent = false;
    bool m_sync_blob_applied = false;

    std::chrono::steady_clock::time_point m_last_inputs_send_time{};

    DesyncInfo m_desync_info{};
    std::string m_last_error;
    std::vector<ChatMessage> m_received_chat;
    std::vector<std::string> m_pending_chat;
    mutable NetplayStats m_stats{};

    uint32_t m_current_local_frame = 0;
    uint32_t m_last_sent_frame = 0;
    bool m_has_sent_inputs = false;

    std::vector<LocalInputWord> m_local_inputs;
    std::vector<bool> m_local_submitted;

    std::vector<LocalInputWord> m_remote_inputs;
    std::vector<bool> m_remote_known;
};

} // namespace f3rt::netplay
