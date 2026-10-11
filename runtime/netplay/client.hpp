#pragma once

#include "protocol.hpp"
#include "socket.hpp"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <vector>

namespace f3rt::netplay {

struct IncomingTcpMsg {
    uint8_t type = 0;
    std::vector<uint8_t> payload;
};

struct IncomingUdpPkt {
    uint8_t player_index = 0;
    uint8_t kind = 0;
    std::vector<uint8_t> body;
};

class Client {
public:
    Client();
    ~Client();

    Client(const Client &) = delete;
    Client &operator=(const Client &) = delete;

    bool connect_tcp(const std::string &host, uint16_t port);
    bool is_tcp_connected() const { return tcp_connected_; }
    bool is_tcp_connecting() const { return tcp_connecting_; }
    bool is_tcp_closed() const { return tcp_closed_; }

    void set_session(uint64_t session_id, std::span<const uint8_t, 32> session_key,
                     const SocketAddress &udp_addr);
    uint16_t local_udp_port() const { return udp_sock_.local_port(); }

    bool has_session() const { return has_session_; }
    uint64_t session_id() const { return session_id_; }
    std::span<const uint8_t, 32> session_key() const { return session_key_; }
    const SocketAddress &server_udp_addr() const { return server_udp_addr_; }

    void send_tcp(uint8_t type, std::span<const uint8_t> payload);
    void send_tcp_raw(std::span<const uint8_t> frame);

    void send_udp_raw(std::span<const uint8_t> packet);

    // Drain TCP and UDP sockets non-blocking
    void poll();

    // Check received messages
    bool pop_tcp_msg(IncomingTcpMsg &out_msg);
    bool pop_udp_pkt(IncomingUdpPkt &out_pkt);

    void close();

    const std::string &last_error() const { return error_; }

private:
    Socket tcp_sock_;
    Socket udp_sock_;
    SocketAddress server_tcp_addr_;
    SocketAddress server_udp_addr_;

    bool tcp_connecting_ = false;
    bool tcp_connected_ = false;
    bool tcp_closed_ = false;

    std::vector<uint8_t> tcp_out_buf_;
    std::vector<uint8_t> tcp_in_buf_;

    uint64_t session_id_ = 0;
    std::array<uint8_t, 32> session_key_{};
    bool has_session_ = false;

    std::vector<IncomingTcpMsg> tcp_queue_;
    std::vector<IncomingUdpPkt> udp_queue_;

    std::string error_;
};

} // namespace f3rt::netplay
