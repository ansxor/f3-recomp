#include "socket.hpp"

#include <cstring>
#include <iostream>

#ifdef _WIN32
#pragma comment(lib, "ws2_32.lib")
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <unistd.h>
#endif

namespace f3rt::netplay {

namespace {

#ifdef _WIN32
int close_handle(socket_handle_t h) { return closesocket(h); }
int get_last_error() { return WSAGetLastError(); }
bool is_would_block_err(int e) { return e == WSAEWOULDBLOCK; }
bool is_in_progress_err(int e) { return e == WSAEWOULDBLOCK || e == WSAEINPROGRESS; }
#else
int close_handle(socket_handle_t h) { return ::close(h); }
int get_last_error() { return errno; }
bool is_would_block_err(int e) { return e == EWOULDBLOCK || e == EAGAIN; }
bool is_in_progress_err(int e) { return e == EINPROGRESS; }
#endif

} // namespace

void init_network() {
#ifdef _WIN32
    static bool initialized = false;
    if (!initialized) {
        WSADATA data;
        WSAStartup(MAKEWORD(2, 2), &data);
        initialized = true;
    }
#endif
}

SocketAddress::SocketAddress() {
    std::memset(&storage_, 0, sizeof(storage_));
    len_ = 0;
}

SocketAddress::SocketAddress(const struct sockaddr *addr, socklen_t len) {
    std::memset(&storage_, 0, sizeof(storage_));
    if (addr && len > 0) {
        len_ = std::min(len, socklen_t(sizeof(storage_)));
        std::memcpy(&storage_, addr, len_);
    } else {
        len_ = 0;
    }
}

const struct sockaddr *SocketAddress::sockaddr_ptr() const {
    return reinterpret_cast<const struct sockaddr *>(&storage_);
}

std::string SocketAddress::ip_string() const {
    if (len_ == 0) return "";
    char buf[INET6_ADDRSTRLEN] = {0};
    if (storage_.ss_family == AF_INET) {
        const auto *sa = reinterpret_cast<const sockaddr_in *>(&storage_);
        inet_ntop(AF_INET, &(sa->sin_addr), buf, sizeof(buf));
    } else if (storage_.ss_family == AF_INET6) {
        const auto *sa = reinterpret_cast<const sockaddr_in6 *>(&storage_);
        inet_ntop(AF_INET6, &(sa->sin6_addr), buf, sizeof(buf));
    }
    return buf;
}

uint16_t SocketAddress::port() const {
    if (len_ == 0) return 0;
    if (storage_.ss_family == AF_INET) {
        const auto *sa = reinterpret_cast<const sockaddr_in *>(&storage_);
        return ntohs(sa->sin_port);
    } else if (storage_.ss_family == AF_INET6) {
        const auto *sa = reinterpret_cast<const sockaddr_in6 *>(&storage_);
        return ntohs(sa->sin6_port);
    }
    return 0;
}

bool SocketAddress::operator==(const SocketAddress &o) const {
    if (len_ != o.len_) return false;
    if (len_ == 0) return true;
    if (storage_.ss_family != o.storage_.ss_family) return false;
    if (storage_.ss_family == AF_INET) {
        const auto *a = reinterpret_cast<const sockaddr_in *>(&storage_);
        const auto *b = reinterpret_cast<const sockaddr_in *>(&o.storage_);
        return a->sin_port == b->sin_port &&
               std::memcmp(&a->sin_addr, &b->sin_addr, sizeof(a->sin_addr)) == 0;
    }
    if (storage_.ss_family == AF_INET6) {
        const auto *a = reinterpret_cast<const sockaddr_in6 *>(&storage_);
        const auto *b = reinterpret_cast<const sockaddr_in6 *>(&o.storage_);
        return a->sin6_port == b->sin6_port &&
               std::memcmp(&a->sin6_addr, &b->sin6_addr, sizeof(a->sin6_addr)) == 0;
    }
    return std::memcmp(&storage_, &o.storage_, len_) == 0;
}

bool SocketAddress::resolve(const std::string &host, uint16_t port, SocketAddress &out_addr) {
    init_network();
    struct addrinfo hints{};
    hints.ai_family = AF_UNSPEC; // IPv4 or IPv6
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_ADDRCONFIG;

    struct addrinfo *result = nullptr;
    std::string port_str = std::to_string(port);
    int res = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
    if (res != 0 || !result) {
        // Fallback without AI_ADDRCONFIG if host is localhost or literal
        hints.ai_flags = 0;
        res = getaddrinfo(host.c_str(), port_str.c_str(), &hints, &result);
        if (res != 0 || !result) {
            return false;
        }
    }

    out_addr = SocketAddress(result->ai_addr, socklen_t(result->ai_addrlen));
    freeaddrinfo(result);
    return true;
}

Socket::Socket() : handle_(invalid_socket_handle) {}

Socket::Socket(socket_handle_t handle) : handle_(handle) {}

Socket::~Socket() {
    close();
}

Socket::Socket(Socket &&o) noexcept : handle_(o.handle_) {
    o.handle_ = invalid_socket_handle;
}

Socket &Socket::operator=(Socket &&o) noexcept {
    if (this != &o) {
        close();
        handle_ = o.handle_;
        o.handle_ = invalid_socket_handle;
    }
    return *this;
}

Socket Socket::create_tcp() {
    init_network();
    socket_handle_t h = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (h == invalid_socket_handle) {
        return Socket();
    }
    Socket s(h);
    s.set_nonblocking(true);
    s.set_nodelay(true);
    return s;
}

Socket Socket::create_udp() {
    init_network();
    socket_handle_t h = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (h == invalid_socket_handle) {
        return Socket();
    }
    Socket s(h);
    s.set_nonblocking(true);
    return s;
}

void Socket::close() {
    if (handle_ != invalid_socket_handle) {
        close_handle(handle_);
        handle_ = invalid_socket_handle;
    }
}

bool Socket::set_nonblocking(bool nb) {
    if (handle_ == invalid_socket_handle) return false;
#ifdef _WIN32
    u_long mode = nb ? 1 : 0;
    return ioctlsocket(handle_, FIONBIO, &mode) == 0;
#else
    int flags = fcntl(handle_, F_GETFL, 0);
    if (flags < 0) return false;
    flags = nb ? (flags | O_NONBLOCK) : (flags & ~O_NONBLOCK);
    return fcntl(handle_, F_SETFL, flags) == 0;
#endif
}

bool Socket::set_nodelay(bool nd) {
    if (handle_ == invalid_socket_handle) return false;
    int opt = nd ? 1 : 0;
    return setsockopt(handle_, IPPROTO_TCP, TCP_NODELAY,
                      reinterpret_cast<const char *>(&opt), sizeof(opt)) == 0;
}

bool Socket::bind(uint16_t port, bool loopback_only) {
    if (handle_ == invalid_socket_handle) return false;

    int opt = 1;
    setsockopt(handle_, SOL_SOCKET, SO_REUSEADDR,
               reinterpret_cast<const char *>(&opt), sizeof(opt));

    sockaddr_in sa{};
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    sa.sin_addr.s_addr = loopback_only ? htonl(INADDR_LOOPBACK) : htonl(INADDR_ANY);

    return ::bind(handle_, reinterpret_cast<const sockaddr *>(&sa), sizeof(sa)) == 0;
}

uint16_t Socket::local_port() const {
    if (handle_ == invalid_socket_handle) return 0;
    sockaddr_storage ss{};
    socklen_t len = sizeof(ss);
    if (getsockname(handle_, reinterpret_cast<sockaddr *>(&ss), &len) != 0) {
        return 0;
    }
    if (ss.ss_family == AF_INET) {
        const auto *sa = reinterpret_cast<const sockaddr_in *>(&ss);
        return ntohs(sa->sin_port);
    } else if (ss.ss_family == AF_INET6) {
        const auto *sa = reinterpret_cast<const sockaddr_in6 *>(&ss);
        return ntohs(sa->sin6_port);
    }
    return 0;
}

bool Socket::connect(const SocketAddress &addr, std::string &err) {
    if (handle_ == invalid_socket_handle) {
        err = "Socket is invalid";
        return false;
    }

    int rc = ::connect(handle_, addr.sockaddr_ptr(), addr.length());
    if (rc == 0) {
        return true; // immediate success
    }

    int e = get_last_error();
    if (is_in_progress_err(e)) {
        return false; // connection in progress
    }

    err = "Connect error: " + std::to_string(e);
    return false;
}

bool Socket::check_connect_finished(std::string &err) {
    if (handle_ == invalid_socket_handle) {
        err = "Socket is invalid";
        return false;
    }

    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(handle_, SOL_SOCKET, SO_ERROR,
                   reinterpret_cast<char *>(&so_error), &len) < 0) {
        err = "getsockopt(SO_ERROR) failed";
        return false;
    }

    if (so_error != 0) {
        err = "Connection failed: error " + std::to_string(so_error);
        return false;
    }

    return true;
}

ssize_t Socket::send(const void *buf, size_t len, bool &err_would_block) {
    err_would_block = false;
    if (handle_ == invalid_socket_handle) return -1;
#ifdef _WIN32
    int rc = ::send(handle_, static_cast<const char *>(buf), int(len), 0);
#else
    ssize_t rc = ::send(handle_, buf, len, 0);
#endif
    if (rc < 0) {
        int e = get_last_error();
        if (is_would_block_err(e)) {
            err_would_block = true;
        }
        return -1;
    }
    return rc;
}

ssize_t Socket::recv(void *buf, size_t len, bool &err_would_block) {
    err_would_block = false;
    if (handle_ == invalid_socket_handle) return -1;
#ifdef _WIN32
    int rc = ::recv(handle_, static_cast<char *>(buf), int(len), 0);
#else
    ssize_t rc = ::recv(handle_, buf, len, 0);
#endif
    if (rc < 0) {
        int e = get_last_error();
        if (is_would_block_err(e)) {
            err_would_block = true;
        }
        return -1;
    }
    return rc;
}

ssize_t Socket::sendto(const void *buf, size_t len, const SocketAddress &addr, bool &err_would_block) {
    err_would_block = false;
    if (handle_ == invalid_socket_handle) return -1;
#ifdef _WIN32
    int rc = ::sendto(handle_, static_cast<const char *>(buf), int(len), 0,
                      addr.sockaddr_ptr(), addr.length());
#else
    ssize_t rc = ::sendto(handle_, buf, len, 0,
                          addr.sockaddr_ptr(), addr.length());
#endif
    if (rc < 0) {
        int e = get_last_error();
        if (is_would_block_err(e)) {
            err_would_block = true;
        }
        return -1;
    }
    return rc;
}

ssize_t Socket::recvfrom(void *buf, size_t len, SocketAddress &from_addr, bool &err_would_block) {
    err_would_block = false;
    if (handle_ == invalid_socket_handle) return -1;
    sockaddr_storage ss{};
    socklen_t slen = sizeof(ss);
#ifdef _WIN32
    int rc = ::recvfrom(handle_, static_cast<char *>(buf), int(len), 0,
                        reinterpret_cast<sockaddr *>(&ss), &slen);
#else
    ssize_t rc = ::recvfrom(handle_, buf, len, 0,
                            reinterpret_cast<sockaddr *>(&ss), &slen);
#endif
    if (rc < 0) {
        int e = get_last_error();
        if (is_would_block_err(e)) {
            err_would_block = true;
        }
        return -1;
    }
    from_addr = SocketAddress(reinterpret_cast<const sockaddr *>(&ss), slen);
    return rc;
}

} // namespace f3rt::netplay
