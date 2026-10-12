#pragma once

#include "netplay/shared/contract.hpp"
#include "f3rt/netplay.hpp"
#include <atomic>
#include <chrono>
#include <memory>
#include <mutex>
#include <string>

namespace f3rt::netplay {

class Client {
public:
    Client();
    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    bool connect(const std::string &host, uint16_t port);
    void disconnect();

    bool is_connecting() const;
    bool is_connected() const;
    bool is_disconnected() const;
    bool connection_failed() const;

    // Pumps network I/O: AdvanceTime, ReceivePackets, SendPackets
    void poll();

    // Message creation & sending
    template <typename T> T *create_message(NetplayMessageType type) {
        if (!m_yojimbo_client) return nullptr;
        return static_cast<T *>(m_yojimbo_client->CreateMessage(static_cast<int>(type)));
    }

    void send_control_message(yojimbo::Message *msg);
    void send_inputs_message(yojimbo::Message *msg);
    void attach_block_to_message(yojimbo::BlockMessage *msg, const uint8_t *data, size_t bytes);

    // Message reception
    yojimbo::Message *receive_control_message();
    yojimbo::Message *receive_inputs_message();
    void release_message(yojimbo::Message *msg);

    // Flush outgoing packets immediately
    void send_packets();

    const std::string &last_error() const { return m_error; }
    void set_error(std::string error) { m_error = std::move(error); }

    uint64_t client_id() const { return m_client_id; }
    NetplayStats stats() const;

private:
    static std::atomic<int> s_netplay_refcount;

    struct AsyncFetchState {
        std::atomic<bool> cancelled{false};
        std::atomic<bool> finished{false};
        std::mutex mutex;
        bool success = false;
        std::string error;
        uint64_t client_id = 0;
        std::array<uint8_t, yojimbo::ConnectTokenBytes> token{};
    };

    std::shared_ptr<AsyncFetchState> m_fetch_state;
    bool m_token_fetch_failed = false;

    std::unique_ptr<NetplayAdapter> m_adapter;
    std::unique_ptr<NetplayConfig> m_config;
    std::unique_ptr<yojimbo::Client> m_yojimbo_client;

    std::chrono::steady_clock::time_point m_start_time;
    double get_elapsed_seconds();

    uint64_t m_client_id = 0;
    std::string m_error;
};

} // namespace f3rt::netplay
