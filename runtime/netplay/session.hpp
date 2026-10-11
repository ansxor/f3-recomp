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
    // Handles one TCP message while Running. Returns true when it ended the session (desync / peer left / error).
    bool handle_running_tcp(const IncomingTcpMsg &msg);

    SessionState state() const { return state_; }
    EndReason end_reason() const { return end_reason_; }

    uint64_t session_id() const { return session_id_; }
    const std::string &room_code() const { return room_code_; }
    uint64_t seed() const { return seed_; }
    uint8_t player_index() const { return player_index_; }
    uint8_t input_delay() const { return input_delay_; }
    const std::string &peer_name() const { return peer_name_; }
    const std::string &last_error() const { return last_error_; }
    const DesyncInfo &desync_info() const { return desync_info_; }
    const NetplayStats &stats() const { return stats_; }
    uint16_t local_udp_port() const { return client_.local_udp_port(); }

    bool ready_to_step(uint32_t frame) const;
    std::array<LocalInputWord, 2> inputs_for(uint32_t frame) const;
    std::array<LocalInputWord, local_player_count> all_inputs_for(uint32_t frame) const;

    void submit_local(uint32_t frame, LocalInputWord word);
    void submit_local(LocalInputWord word);

    void report_crc(uint32_t frame, uint32_t crc);
    void send_result(uint32_t frame, uint8_t winner, uint32_t final_crc);
    void send_bye();
    void close();
    bool send_chat(std::string_view text);
    std::vector<ChatMessage> drain_chat();

private:
    void send_udp_register();
    void send_udp_inputs_packet(uint32_t current_frame);
    void record_remote_input(uint32_t frame, uint16_t word);

    SessionConfig config_;
    Client client_;

    SessionState state_ = SessionState::Idle;
    EndReason end_reason_ = EndReason::None;

    bool hello_sent_ = false;
    std::string room_code_;
    uint64_t session_id_ = 0;
    std::array<uint8_t, 32> session_key_{};
    uint64_t seed_ = 0;
    uint8_t player_index_ = 0;
    uint8_t input_delay_ = 2;
    uint16_t udp_port_ = 0;
    std::string peer_name_;
    bool sync_blob_sent_ = false;
    bool sync_blob_applied_ = false;

    std::chrono::steady_clock::time_point last_register_time_{};
    std::chrono::steady_clock::time_point last_inputs_send_time_{};

    DesyncInfo desync_info_{};
    mutable NetplayStats stats_{};
    std::string last_error_;
    std::vector<ChatMessage> received_chat_;
    std::vector<std::string> pending_chat_;

    uint32_t current_local_frame_ = 0;
    uint32_t last_sent_frame_ = 0;
    bool has_sent_inputs_ = false;

    std::vector<LocalInputWord> local_inputs_;
    std::vector<bool> local_submitted_;

    std::vector<LocalInputWord> remote_inputs_;
    std::vector<bool> remote_known_;
};

} // namespace f3rt::netplay
