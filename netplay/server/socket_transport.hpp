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

    bool is_open() const { return fd_ >= 0; }
    int fd() const { return fd_; }
    uint16_t bound_port() const { return bound_port_; }

    // Poll socket for readable data. Returns >0 if readable, 0 on timeout, <0 on error.
    int poll(int timeout_ms) const;

    // Send and receive packets using yojimbo::Address
    int send_packet(const yojimbo::Address &to, const uint8_t *data, int bytes);
    int receive_packet(yojimbo::Address &from, uint8_t *data, int max_bytes);

private:
    int fd_ = -1;
    uint16_t bound_port_ = 0;
};

// Relay Adapter enabling custom packet I/O
class RelayAdapter : public NetplayAdapter {
public:
    explicit RelayAdapter(SocketTransport &transport) : transport_(transport) {}

    bool UseCustomPacketIO() const override {
        return true;
    }

    void SendPacket(const yojimbo::Address &to, const uint8_t *packetData, int packetBytes) override {
        transport_.send_packet(to, packetData, packetBytes);
    }

    int ReceivePacket(yojimbo::Address &from, uint8_t *packetData, int maxPacketBytes) override {
        return transport_.receive_packet(from, packetData, maxPacketBytes);
    }

private:
    SocketTransport &transport_;
};

} // namespace f3rt::netplay
