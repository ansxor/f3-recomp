#include "client.hpp"
#include <httplib.h>
#include <cstring>
#include <thread>

namespace f3rt::netplay {

std::atomic<int> Client::s_netplay_refcount{0};

Client::Client() {
    if (s_netplay_refcount.fetch_add(1) == 0) {
        init_netplay();
    }
    start_time_ = std::chrono::steady_clock::now();
}

Client::~Client() {
    disconnect();
    yojimbo_client_.reset();
    config_.reset();
    adapter_.reset();
    if (s_netplay_refcount.fetch_sub(1) == 1) {
        shutdown_netplay();
    }
}

double Client::get_elapsed_seconds() {
    auto now = std::chrono::steady_clock::now();
    return std::chrono::duration<double>(now - start_time_).count();
}

bool Client::connect(const std::string &host, uint16_t port) {
    disconnect();
    error_.clear();
    token_fetch_failed_ = false;

    fetch_state_ = std::make_shared<AsyncFetchState>();

    std::thread([fetch_state = fetch_state_, host, port]() {
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
            fetch_state->error = "Failed to connect to relay HTTP endpoint " + host + ":" + std::to_string(port);
            fetch_state->finished.store(true, std::memory_order_release);
            return;
        }

        if (res->status == HTTP_STATUS_VERSION_MISMATCH) {
            fetch_state->success = false;
            fetch_state->error = "Relay requires a different protocol version, update required: " + res->body;
            fetch_state->finished.store(true, std::memory_order_release);
            return;
        }

        if (res->status != 200) {
            fetch_state->success = false;
            fetch_state->error = "Relay HTTP token request failed (HTTP " + std::to_string(res->status) + "): " + res->body;
            fetch_state->finished.store(true, std::memory_order_release);
            return;
        }

        std::span<const uint8_t> body(reinterpret_cast<const uint8_t *>(res->body.data()), res->body.size());
        if (!parse_token_response(body, fetch_state->client_id, fetch_state->token)) {
            fetch_state->success = false;
            fetch_state->error = "Invalid token response from relay (expected " + std::to_string(TOKEN_RESPONSE_SIZE) +
                                 " bytes, got " + std::to_string(res->body.size()) + ")";
            fetch_state->finished.store(true, std::memory_order_release);
            return;
        }

        fetch_state->success = true;
        fetch_state->finished.store(true, std::memory_order_release);
    }).detach();

    return true;
}

void Client::disconnect() {
    if (fetch_state_) {
        fetch_state_->cancelled.store(true, std::memory_order_relaxed);
        fetch_state_.reset();
    }
    if (yojimbo_client_) {
        if (yojimbo_client_->IsConnected()) {
            yojimbo_client_->Disconnect();
        }
    }
    token_fetch_failed_ = false;
}

bool Client::is_connecting() const {
    if (fetch_state_) return true;
    return yojimbo_client_ && yojimbo_client_->IsConnecting();
}

bool Client::is_connected() const {
    return yojimbo_client_ && yojimbo_client_->IsConnected();
}

bool Client::is_disconnected() const {
    if (token_fetch_failed_) return true;
    if (fetch_state_) return false;
    return !yojimbo_client_ || yojimbo_client_->IsDisconnected();
}

bool Client::connection_failed() const {
    if (token_fetch_failed_) return true;
    return yojimbo_client_ && yojimbo_client_->ConnectionFailed();
}

void Client::poll() {
    // 1. Process finished async token fetch on the poll thread
    if (fetch_state_) {
        if (fetch_state_->finished.load(std::memory_order_acquire)) {
            bool ok = false;
            std::string err;
            uint64_t cid = 0;
            std::array<uint8_t, yojimbo::ConnectTokenBytes> tok{};
            {
                std::lock_guard<std::mutex> lock(fetch_state_->mutex);
                ok = fetch_state_->success;
                err = std::move(fetch_state_->error);
                cid = fetch_state_->client_id;
                tok = fetch_state_->token;
            }
            fetch_state_.reset();

            if (!ok) {
                error_ = std::move(err);
                token_fetch_failed_ = true;
                return;
            }

            adapter_ = std::make_unique<NetplayAdapter>();
            config_ = std::make_unique<NetplayConfig>();

            double current_time = get_elapsed_seconds();
            yojimbo_client_ = std::make_unique<yojimbo::Client>(
                yojimbo::GetDefaultAllocator(),
                yojimbo::Address("0.0.0.0:0"),
                *config_,
                *adapter_,
                current_time
            );

            if (!yojimbo_client_->Connect(cid, tok.data())) {
                error_ = "yojimbo client connect failed";
                token_fetch_failed_ = true;
                return;
            }

            client_id_ = cid;
        } else {
            // Still fetching token in background thread
            return;
        }
    }

    // 2. Pump yojimbo client
    if (!yojimbo_client_) return;

    double time = get_elapsed_seconds();
    yojimbo_client_->AdvanceTime(time);
    yojimbo_client_->ReceivePackets();

    if (yojimbo_client_->ConnectionFailed()) {
        int reason = yojimbo_client_->GetDisconnectReason();
        error_ = "Connection failed: " + std::string(yojimbo::GetClientDisconnectReasonString(reason));
    }
}

void Client::send_control_message(yojimbo::Message *msg) {
    if (yojimbo_client_ && msg) {
        yojimbo_client_->SendMessage(CHANNEL_CONTROL, msg);
    }
}

void Client::send_inputs_message(yojimbo::Message *msg) {
    if (yojimbo_client_ && msg) {
        yojimbo_client_->SendMessage(CHANNEL_INPUTS, msg);
    }
}

void Client::attach_block_to_message(yojimbo::BlockMessage *msg, const uint8_t *data, size_t bytes) {
    if (!yojimbo_client_ || !msg || !data || bytes == 0) return;
    uint8_t *block = yojimbo_client_->AllocateBlock(static_cast<int>(bytes));
    if (block) {
        std::memcpy(block, data, bytes);
        yojimbo_client_->AttachBlockToMessage(msg, block, static_cast<int>(bytes));
    }
}

yojimbo::Message *Client::receive_control_message() {
    if (!yojimbo_client_ || !yojimbo_client_->IsConnected()) return nullptr;
    return yojimbo_client_->ReceiveMessage(CHANNEL_CONTROL);
}

yojimbo::Message *Client::receive_inputs_message() {
    if (!yojimbo_client_ || !yojimbo_client_->IsConnected()) return nullptr;
    return yojimbo_client_->ReceiveMessage(CHANNEL_INPUTS);
}

void Client::release_message(yojimbo::Message *msg) {
    if (yojimbo_client_ && !yojimbo_client_->IsDisconnected() && msg) {
        yojimbo_client_->ReleaseMessage(msg);
    }
}

void Client::send_packets() {
    if (yojimbo_client_) {
        yojimbo_client_->SendPackets();
    }
}

NetplayStats Client::stats() const {
    NetplayStats s{};
    if (yojimbo_client_ && yojimbo_client_->IsConnected()) {
        yojimbo::NetworkInfo info;
        yojimbo_client_->GetNetworkInfo(info);
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
