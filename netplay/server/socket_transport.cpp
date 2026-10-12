#include "socket_transport.hpp"
#include <arpa/inet.h>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace f3rt::netplay {

SocketTransport::SocketTransport() = default;

SocketTransport::~SocketTransport() { close(); }

bool SocketTransport::open(const std::string &host, uint16_t port) {
    close();

    m_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (m_fd < 0) {
        return false;
    }

    // Set non-blocking
    int flags = ::fcntl(m_fd, F_GETFL, 0);
    if (flags < 0 || ::fcntl(m_fd, F_SETFL, flags | O_NONBLOCK) < 0) {
        close();
        return false;
    }

    // Set SO_REUSEADDR
    int reuse = 1;
    ::setsockopt(m_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    sockaddr_in sin{};
    sin.sin_family = AF_INET;
    sin.sin_port = htons(port);
    if (host.empty() || host == "0.0.0.0") {
        sin.sin_addr.s_addr = INADDR_ANY;
    } else {
        if (::inet_pton(AF_INET, host.c_str(), &sin.sin_addr) <= 0) {
            close();
            return false;
        }
    }

    if (::bind(m_fd, reinterpret_cast<const sockaddr *>(&sin), sizeof(sin)) < 0) {
        close();
        return false;
    }

    // Query bound port
    socklen_t slen = sizeof(sin);
    if (::getsockname(m_fd, reinterpret_cast<sockaddr *>(&sin), &slen) == 0) {
        m_bound_port = ntohs(sin.sin_port);
    } else {
        m_bound_port = port;
    }

    return true;
}

void SocketTransport::close() {
    if (m_fd >= 0) {
        ::close(m_fd);
        m_fd = -1;
        m_bound_port = 0;
    }
}

int SocketTransport::poll(int timeout_ms) const {
    if (m_fd < 0) {
        return -1;
    }
    struct pollfd pfd{};
    pfd.fd = m_fd;
    pfd.events = POLLIN;
    return ::poll(&pfd, 1, timeout_ms);
}

int SocketTransport::send_packet(const yojimbo::Address &to, const uint8_t *data, int bytes) {
    if (m_fd < 0 || !data || bytes <= 0) {
        return -1;
    }

    if (to.GetType() == yojimbo::ADDRESS_IPV4) {
        sockaddr_in sin{};
        sin.sin_family = AF_INET;
        sin.sin_port = htons(to.GetPort());
        std::memcpy(&sin.sin_addr.s_addr, to.GetAddress4(), 4);
        return static_cast<int>(
            ::sendto(m_fd, data, bytes, 0, reinterpret_cast<const sockaddr *>(&sin), sizeof(sin)));
    } else if (to.GetType() == yojimbo::ADDRESS_IPV6) {
        sockaddr_in6 sin6{};
        sin6.sin6_family = AF_INET6;
        sin6.sin6_port = htons(to.GetPort());
        std::memcpy(&sin6.sin6_addr, to.GetAddress6(), 16);
        return static_cast<int>(::sendto(m_fd, data, bytes, 0,
                                         reinterpret_cast<const sockaddr *>(&sin6), sizeof(sin6)));
    }
    return -1;
}

int SocketTransport::receive_packet(yojimbo::Address &from, uint8_t *data, int max_bytes) {
    if (m_fd < 0 || !data || max_bytes <= 0) {
        return 0;
    }

    sockaddr_storage ss{};
    socklen_t slen = sizeof(ss);
    ssize_t n = ::recvfrom(m_fd, data, max_bytes, 0, reinterpret_cast<sockaddr *>(&ss), &slen);
    if (n <= 0) {
        return 0;
    }
    if (ss.ss_family == AF_INET) {
        const auto *sin = reinterpret_cast<const sockaddr_in *>(&ss);
        from = yojimbo::Address(reinterpret_cast<const uint8_t *>(&sin->sin_addr.s_addr),
                                ntohs(sin->sin_port));
    } else if (ss.ss_family == AF_INET6) {
        const auto *sin6 = reinterpret_cast<const sockaddr_in6 *>(&ss);
        from = yojimbo::Address(reinterpret_cast<const uint16_t *>(&sin6->sin6_addr),
                                ntohs(sin6->sin6_port));
    }
    return static_cast<int>(n);
}

} // namespace f3rt::netplay
