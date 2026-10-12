#pragma once

#include "netplay/shared/contract.hpp"
#include <cstdint>
#include <string>

namespace f3rt::netplay {

class SocketTransport {
public:
    SocketTransport();
    ~SocketTransport();

    // Bind UDP socket to host:port. Pass port 0 for ephemeral.
    bool open(const std::string &host, uint16_t port);
    void close();

    bool is_open() const { return m_fd >= 0; }
    int fd() const { return m_fd; }
    uint16_t bound_port() const { return m_bound_port; }

    // Poll socket for readable data. Returns >0 if readable, 0 on timeout, <0 on error.
    int poll(int timeout_ms) const;

    // Send and receive packets using yojimbo::Address
    int send_packet(const yojimbo::Address &to, const uint8_t *data, int bytes);
    int receive_packet(yojimbo::Address &from, uint8_t *data, int max_bytes);

private:
    int m_fd = -1;
    uint16_t m_bound_port = 0;
};

// Relay Adapter enabling custom packet I/O
class RelayAdapter : public NetplayAdapter {
public:
    explicit RelayAdapter(SocketTransport &transport) : m_transport(transport) {}

    bool UseCustomPacketIO() const override { return true; }

    void SendPacket(const yojimbo::Address &to, const uint8_t *packet_data,
                    int packet_bytes) override {
        m_transport.send_packet(to, packet_data, packet_bytes);
    }

    int ReceivePacket(yojimbo::Address &from, uint8_t *packet_data, int max_packet_bytes) override {
        return m_transport.receive_packet(from, packet_data, max_packet_bytes);
    }

private:
    SocketTransport &m_transport;
};

} // namespace f3rt::netplay
