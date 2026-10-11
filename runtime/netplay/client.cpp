#include "client.hpp"

#include <iostream>

namespace f3rt::netplay {

Client::Client() = default;

Client::~Client() {
    close();
}

bool Client::connect_tcp(const std::string &host, uint16_t port) {
    close();

    if (!SocketAddress::resolve(host, port, server_tcp_addr_)) {
        error_ = "Failed to resolve server address: " + host;
        return false;
    }

    tcp_sock_ = Socket::create_tcp();
    if (!tcp_sock_.is_valid()) {
        error_ = "Failed to create TCP socket";
        return false;
    }

    std::string err;
    if (tcp_sock_.connect(server_tcp_addr_, err)) {
        tcp_connected_ = true;
        tcp_connecting_ = false;
        tcp_closed_ = false;
        return true;
    }

    if (err.empty()) {
        // In progress
        tcp_connecting_ = true;
        tcp_connected_ = false;
        tcp_closed_ = false;
        return true;
    }

    error_ = err;
    close();
    return false;
}

void Client::set_session(uint64_t session_id, std::span<const uint8_t, 32> session_key,
                         const SocketAddress &udp_addr) {
    session_id_ = session_id;
    std::copy_n(session_key.begin(), 32, session_key_.begin());
    server_udp_addr_ = udp_addr;
    has_session_ = true;

    if (!udp_sock_.is_valid()) {
        udp_sock_ = Socket::create_udp();
        udp_sock_.bind(0); // bind to ephemeral local port
    }
}

void Client::send_tcp(uint8_t type, std::span<const uint8_t> payload) {
    auto frame = encode_tcp_frame(type, payload);
    send_tcp_raw(frame);
}

void Client::send_tcp_raw(std::span<const uint8_t> frame) {
    tcp_out_buf_.insert(tcp_out_buf_.end(), frame.begin(), frame.end());
}

void Client::send_udp_raw(std::span<const uint8_t> packet) {
    if (!udp_sock_.is_valid() || !has_session_) return;
    bool would_block = false;
    udp_sock_.sendto(packet.data(), packet.size(), server_udp_addr_, would_block);
}

void Client::poll() {
    // 1. Check TCP connection progress
    if (tcp_connecting_) {
        std::string err;
        if (tcp_sock_.check_connect_finished(err)) {
            tcp_connecting_ = false;
            tcp_connected_ = true;
        } else if (!err.empty()) {
            error_ = err;
            tcp_connecting_ = false;
            tcp_closed_ = true;
            return;
        }
    }

    // 2. Handle TCP I/O if connected
    if (tcp_connected_) {
        // Flush outgoing data
        while (!tcp_out_buf_.empty()) {
            bool would_block = false;
            ssize_t n = tcp_sock_.send(tcp_out_buf_.data(), tcp_out_buf_.size(), would_block);
            if (n > 0) {
                tcp_out_buf_.erase(tcp_out_buf_.begin(), tcp_out_buf_.begin() + n);
            } else if (would_block) {
                break;
            } else {
                error_ = "TCP send error";
                tcp_connected_ = false;
                tcp_closed_ = true;
                break;
            }
        }

        // Receive incoming data
        while (true) {
            uint8_t tmp[4096];
            bool would_block = false;
            ssize_t n = tcp_sock_.recv(tmp, sizeof(tmp), would_block);
            if (n > 0) {
                tcp_in_buf_.insert(tcp_in_buf_.end(), tmp, tmp + n);
            } else if (would_block) {
                break;
            } else if (n == 0) {
                // Remote closed connection
                tcp_connected_ = false;
                tcp_closed_ = true;
                break;
            } else {
                error_ = "TCP recv error";
                tcp_connected_ = false;
                tcp_closed_ = true;
                break;
            }
        }

        // Parse TCP frames
        while (tcp_in_buf_.size() >= 4) {
            uint32_t length = (uint32_t(tcp_in_buf_[0]) << 24) |
                              (uint32_t(tcp_in_buf_[1]) << 16) |
                              (uint32_t(tcp_in_buf_[2]) << 8) |
                              uint32_t(tcp_in_buf_[3]);

            if (length == 0 || length > max_tcp_frame_length) {
                error_ = "Invalid TCP frame length: " + std::to_string(length);
                close();
                break;
            }

            if (tcp_in_buf_.size() < 4 + length) {
                break; // Incomplete frame
            }

            uint8_t type = tcp_in_buf_[4];
            std::vector<uint8_t> payload(tcp_in_buf_.begin() + 5,
                                         tcp_in_buf_.begin() + 4 + length);
            tcp_in_buf_.erase(tcp_in_buf_.begin(), tcp_in_buf_.begin() + 4 + length);

            tcp_queue_.push_back({type, std::move(payload)});
        }
    }

    // 3. Handle UDP I/O
    if (udp_sock_.is_valid() && has_session_) {
        while (true) {
            uint8_t tmp[max_udp_packet_length + 64];
            bool would_block = false;
            SocketAddress from_addr;
            ssize_t n = udp_sock_.recvfrom(tmp, sizeof(tmp), from_addr, would_block);
            if (n > 0) {
                uint8_t player_index = 0;
                uint8_t kind = 0;
                std::vector<uint8_t> body;
                if (decode_udp_packet({tmp, size_t(n)}, session_key_, session_id_,
                                      player_index, kind, body)) {
                    udp_queue_.push_back({player_index, kind, std::move(body)});
                }
            } else {
                break;
            }
        }
    }
}

bool Client::pop_tcp_msg(IncomingTcpMsg &out_msg) {
    if (tcp_queue_.empty()) return false;
    out_msg = std::move(tcp_queue_.front());
    tcp_queue_.erase(tcp_queue_.begin());
    return true;
}

bool Client::pop_udp_pkt(IncomingUdpPkt &out_pkt) {
    if (udp_queue_.empty()) return false;
    out_pkt = std::move(udp_queue_.front());
    udp_queue_.erase(udp_queue_.begin());
    return true;
}

void Client::close() {
    tcp_sock_.close();
    udp_sock_.close();
    tcp_connecting_ = false;
    tcp_connected_ = false;
    tcp_closed_ = true;
    tcp_out_buf_.clear();
    tcp_in_buf_.clear();
    has_session_ = false;
    tcp_queue_.clear();
    udp_queue_.clear();
}

} // namespace f3rt::netplay
