#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
namespace f3rt::netplay {
using socket_handle_t = SOCKET;
inline constexpr socket_handle_t invalid_socket_handle = INVALID_SOCKET;
}
#else
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
namespace f3rt::netplay {
using socket_handle_t = int;
inline constexpr socket_handle_t invalid_socket_handle = -1;
}
#endif

namespace f3rt::netplay {

void init_network();

class SocketAddress {
public:
    SocketAddress();
    SocketAddress(const struct sockaddr *addr, socklen_t len);

    static bool resolve(const std::string &host, uint16_t port, SocketAddress &out_addr);

    const struct sockaddr *sockaddr_ptr() const;
    socklen_t length() const { return len_; }

    std::string ip_string() const;
    uint16_t port() const;

    bool operator==(const SocketAddress &o) const;
    bool operator!=(const SocketAddress &o) const { return !(*this == o); }

private:
    struct sockaddr_storage storage_{};
    socklen_t len_ = 0;
};

class Socket {
public:
    Socket();
    explicit Socket(socket_handle_t handle);
    ~Socket();

    Socket(const Socket &) = delete;
    Socket &operator=(const Socket &) = delete;

    Socket(Socket &&o) noexcept;
    Socket &operator=(Socket &&o) noexcept;

    static Socket create_tcp();
    static Socket create_udp();

    bool is_valid() const { return handle_ != invalid_socket_handle; }
    socket_handle_t handle() const { return handle_; }
    void close();

    bool set_nonblocking(bool nb = true);
    bool set_nodelay(bool nd = true);

    bool bind(uint16_t port, bool loopback_only = false);
    uint16_t local_port() const;

    // Returns true if connected immediately, false if in progress / would block,
    // or throws/sets err on error.
    bool connect(const SocketAddress &addr, std::string &err);
    bool check_connect_finished(std::string &err);

    // Non-blocking I/O:
    // > 0: bytes transferred
    // 0: connection closed
    // -1: would block or error (check err_would_block)
    ssize_t send(const void *buf, size_t len, bool &err_would_block);
    ssize_t recv(void *buf, size_t len, bool &err_would_block);

    ssize_t sendto(const void *buf, size_t len, const SocketAddress &addr, bool &err_would_block);
    ssize_t recvfrom(void *buf, size_t len, SocketAddress &from_addr, bool &err_would_block);

private:
    socket_handle_t handle_ = invalid_socket_handle;
};

} // namespace f3rt::netplay
