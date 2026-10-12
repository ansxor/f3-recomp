#include "client.hpp"
#include <httplib.h>
#include <cstring>
#include <exception>
#include <thread>

namespace f3rt::netplay {

std::atomic<int> Client::s_netplay_refcount{0};

Client::Client() {
    if (s_netplay_refcount.fetch_add(1) == 0) {
        init_netplay();
    }
    m_start_time = std::chrono::steady_clock::now();
}

Client::~Client() {
    disconnect();
    m_yojimbo_client.reset();
    m_config.reset();
    m_adapter.reset();
    if (s_netplay_refcount.fetch_sub(1) == 1) {
        shutdown_netplay();
    }
}

double Client::get_elapsed_seconds() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - m_start_time).count();
}

bool Client::connect(const std::string &host, uint16_t port) {
    disconnect();
    m_error.clear();
    m_token_fetch_failed = false;

    m_fetch_state = std::make_shared<AsyncFetchState>();

    // Catch handlers may still throw (lock/allocation); that terminates the process, as an
    // uncaught exception on a detached thread would.
    // NOLINTNEXTLINE(bugprone-exception-escape): see comment above
    std::thread([fetch_state = m_fetch_state, host, port]() {
        try {
            httplib::Client cli(host, port);
            cli.set_connection_timeout(5, 0);
            cli.set_read_timeout(5, 0);

            std::string path = std::string(TOKEN_ROUTE) + "?" +
                               std::string(TOKEN_VERSION_QUERY_PARAM) + "=" +
                               std::to_string(PROTOCOL_VERSION);

            auto res = cli.Get(path);

            if (fetch_state->cancelled.load(std::memory_order_relaxed)) {
                return;
            }

            std::lock_guard<std::mutex> lock(fetch_state->mutex);

            if (!res) {
                fetch_state->success = false;
                fetch_state->error =
                    "Failed to connect to relay HTTP endpoint " + host + ":" + std::to_string(port);
                fetch_state->finished.store(true, std::memory_order_release);
                return;
            }

            if (res->status == HTTP_STATUS_VERSION_MISMATCH) {
                fetch_state->success = false;
                fetch_state->error =
                    "Relay requires a different protocol version, update required: " + res->body;
                fetch_state->finished.store(true, std::memory_order_release);
                return;
            }

            if (res->status != 200) {
                fetch_state->success = false;
                fetch_state->error = "Relay HTTP token request failed (HTTP " +
                                     std::to_string(res->status) + "): " + res->body;
                fetch_state->finished.store(true, std::memory_order_release);
                return;
            }

            std::span<const uint8_t> body(reinterpret_cast<const uint8_t *>(res->body.data()),
                                          res->body.size());
            if (!parse_token_response(body, fetch_state->client_id, fetch_state->token)) {
                fetch_state->success = false;
                fetch_state->error = "Invalid token response from relay (expected " +
                                     std::to_string(TOKEN_RESPONSE_SIZE) + " bytes, got " +
                                     std::to_string(res->body.size()) + ")";
                fetch_state->finished.store(true, std::memory_order_release);
                return;
            }

            fetch_state->success = true;
            fetch_state->finished.store(true, std::memory_order_release);
        } catch (const std::exception &e) {
            // Nothing may escape a detached thread; report through the fetch state.
            std::lock_guard<std::mutex> lock(fetch_state->mutex);
            fetch_state->success = false;
            fetch_state->error = std::string("Relay token request failed: ") + e.what();
            fetch_state->finished.store(true, std::memory_order_release);
        } catch (...) {
            std::lock_guard<std::mutex> lock(fetch_state->mutex);
            fetch_state->success = false;
            fetch_state->error = "Relay token request failed with an unknown exception";
            fetch_state->finished.store(true, std::memory_order_release);
        }
    }).detach();

    return true;
}

void Client::disconnect() {
    if (m_fetch_state) {
        m_fetch_state->cancelled.store(true, std::memory_order_relaxed);
        m_fetch_state.reset();
    }
    if (m_yojimbo_client) {
        if (m_yojimbo_client->IsConnected()) {
            m_yojimbo_client->Disconnect();
        }
    }
    m_token_fetch_failed = false;
}

bool Client::is_connecting() const {
    if (m_fetch_state) return true;
    return m_yojimbo_client && m_yojimbo_client->IsConnecting();
}

bool Client::is_connected() const { return m_yojimbo_client && m_yojimbo_client->IsConnected(); }

bool Client::is_disconnected() const {
    if (m_token_fetch_failed) return true;
    if (m_fetch_state) return false;
    return !m_yojimbo_client || m_yojimbo_client->IsDisconnected();
}

bool Client::connection_failed() const {
    if (m_token_fetch_failed) return true;
    return m_yojimbo_client && m_yojimbo_client->ConnectionFailed();
}

void Client::poll() {
    // 1. Process finished async token fetch on the poll thread
    if (m_fetch_state) {
        if (m_fetch_state->finished.load(std::memory_order_acquire)) {
            bool ok = false;
            std::string err;
            uint64_t cid = 0;
            std::array<uint8_t, yojimbo::ConnectTokenBytes> tok{};
            {
                std::lock_guard<std::mutex> lock(m_fetch_state->mutex);
                ok = m_fetch_state->success;
                err = std::move(m_fetch_state->error);
                cid = m_fetch_state->client_id;
                tok = m_fetch_state->token;
            }
            m_fetch_state.reset();

            if (!ok) {
                m_error = std::move(err);
                m_token_fetch_failed = true;
                return;
            }

            m_adapter = std::make_unique<NetplayAdapter>();
            m_config = std::make_unique<NetplayConfig>();

            double current_time = get_elapsed_seconds();
            m_yojimbo_client = std::make_unique<yojimbo::Client>(
                yojimbo::GetDefaultAllocator(), yojimbo::Address("0.0.0.0:0"), *m_config,
                *m_adapter, current_time);

            if (!m_yojimbo_client->Connect(cid, tok.data())) {
                m_error = "yojimbo client connect failed";
                m_token_fetch_failed = true;
                return;
            }

            m_client_id = cid;
        } else {
            // Still fetching token in background thread
            return;
        }
    }

    // 2. Pump yojimbo client
    if (!m_yojimbo_client) return;

    double time = get_elapsed_seconds();
    m_yojimbo_client->AdvanceTime(time);
    m_yojimbo_client->ReceivePackets();

    if (m_yojimbo_client->ConnectionFailed()) {
        int reason = m_yojimbo_client->GetDisconnectReason();
        m_error =
            "Connection failed: " + std::string(yojimbo::GetClientDisconnectReasonString(reason));
    }
}

void Client::send_control_message(yojimbo::Message *msg) {
    if (m_yojimbo_client && msg) {
        m_yojimbo_client->SendMessage(ChannelControl, msg);
    }
}

void Client::send_inputs_message(yojimbo::Message *msg) {
    if (m_yojimbo_client && msg) {
        m_yojimbo_client->SendMessage(ChannelInputs, msg);
    }
}

void Client::attach_block_to_message(yojimbo::BlockMessage *msg, const uint8_t *data,
                                     size_t bytes) {
    if (!m_yojimbo_client || !msg || !data || bytes == 0) return;
    uint8_t *block = m_yojimbo_client->AllocateBlock(static_cast<int>(bytes));
    if (block) {
        std::memcpy(block, data, bytes);
        m_yojimbo_client->AttachBlockToMessage(msg, block, static_cast<int>(bytes));
    }
}

yojimbo::Message *Client::receive_control_message() {
    if (!m_yojimbo_client || !m_yojimbo_client->IsConnected()) return nullptr;
    return m_yojimbo_client->ReceiveMessage(ChannelControl);
}

yojimbo::Message *Client::receive_inputs_message() {
    if (!m_yojimbo_client || !m_yojimbo_client->IsConnected()) return nullptr;
    return m_yojimbo_client->ReceiveMessage(ChannelInputs);
}

void Client::release_message(yojimbo::Message *msg) {
    if (m_yojimbo_client && !m_yojimbo_client->IsDisconnected() && msg) {
        m_yojimbo_client->ReleaseMessage(msg);
    }
}

void Client::send_packets() {
    if (m_yojimbo_client) {
        m_yojimbo_client->SendPackets();
    }
}

NetplayStats Client::stats() const {
    NetplayStats s{};
    if (m_yojimbo_client && m_yojimbo_client->IsConnected()) {
        yojimbo::NetworkInfo info;
        m_yojimbo_client->GetNetworkInfo(info);
        s.ping_ms = static_cast<uint32_t>(info.RTT);
        s.packets_sent = static_cast<uint64_t>(info.numPacketsSent);
        s.packets_received = static_cast<uint64_t>(info.numPacketsReceived);
        s.packets_dropped = (info.numPacketsSent > info.numPacketsAcked)
                                ? static_cast<uint64_t>(info.numPacketsSent - info.numPacketsAcked)
                                : 0;
    }
    return s;
}

} // namespace f3rt::netplay
