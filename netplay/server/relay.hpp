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

    bool is_running() const { return running_; }
    uint16_t bound_port() const { return transport_.bound_port(); }
    uint16_t http_port() const { return token_server_ ? static_cast<uint16_t>(token_server_->bound_port()) : 0; }
    const std::array<uint8_t, 32> &private_key() const { return private_key_; }
    RelayStateMachine &state_machine() { return state_machine_; }

private:
    void execute_actions(const std::vector<Action> &actions, double now);
    void process_received_messages(double now);

    RelayConfig config_;
    std::array<uint8_t, 32> private_key_{};
    bool running_ = false;

    SocketTransport transport_;
    RelayAdapter adapter_;
    NetplayConfig netplay_config_;
    std::unique_ptr<yojimbo::Server> yojimbo_server_;
    std::unique_ptr<TokenServer> token_server_;
    RelayStateMachine state_machine_;
    LogWriter log_writer_;

    std::array<bool, MAX_CLIENTS> client_connected_prev_{};
    struct DelayedDisconnect {
        ClientSlotId slot = -1;
        double deadline = 0.0;
    };
    std::vector<DelayedDisconnect> pending_disconnects_;
};

} // namespace f3rt::netplay
