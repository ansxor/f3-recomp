#pragma once

#include "log_writer.hpp"
#include "netplay/shared/contract.hpp"
#include "socket_transport.hpp"
#include "state.hpp"
#include "token_service.hpp"
#include "yojimbo.h"
#include <array>
#include <atomic>
#include <cstdint>
#include <memory>
#include <string>

namespace f3rt::netplay {

struct RelayConfig {
    uint16_t port = DEFAULT_PORT;
    std::string public_address = "127.0.0.1:7845";
    std::string logdir = "./netplay-logs";
    int max_rooms = 32;
    bool enable_token_server = true;
};

class RelayServer {
public:
    explicit RelayServer(RelayConfig config);
    ~RelayServer();

    // Start server: binds UDP socket, starts HTTP token server, starts Yojimbo server
    bool start();

    // Stop server cleanly
    void stop();

    // Execute one tick: poll socket, AdvanceTime, ReceivePackets, process messages,
    // tick state machine, execute outbound actions, SendPackets.
    void step(double now, int poll_timeout_ms = 1);

    // Continuous loop until stop_requested is true
    void run(std::atomic<bool> &stop_requested);

    bool is_running() const { return m_running; }
    uint16_t bound_port() const { return m_transport.bound_port(); }
    uint16_t http_port() const {
        return m_token_server ? static_cast<uint16_t>(m_token_server->bound_port()) : 0;
    }
    const std::array<uint8_t, 32> &private_key() const { return m_private_key; }
    RelayStateMachine &state_machine() { return m_state_machine; }

private:
    void execute_actions(const std::vector<Action> &actions, double now);
    void process_received_messages(double now);

    RelayConfig m_config;
    std::array<uint8_t, 32> m_private_key{};
    bool m_running = false;

    SocketTransport m_transport;
    RelayAdapter m_adapter;
    NetplayConfig m_netplay_config;
    std::unique_ptr<yojimbo::Server> m_yojimbo_server;
    std::unique_ptr<TokenServer> m_token_server;
    RelayStateMachine m_state_machine;
    LogWriter m_log_writer;

    std::array<bool, MAX_CLIENTS> m_client_connected_prev{};
    struct DelayedDisconnect {
        ClientSlotId slot = -1;
        double deadline = 0.0;
    };
    std::vector<DelayedDisconnect> m_pending_disconnects;
};

} // namespace f3rt::netplay
