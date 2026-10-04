#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>

namespace f3rt::netplay {

struct Identity {
    std::array<uint32_t, 7> rom_crc;
    std::array<uint8_t, 32> build_hash;
    uint32_t settings;
    uint32_t eeprom_crc;
    uint32_t initial_crc;
}; // all fields must match handshake, plus protocol

struct TransportOptions {
    std::string server;
    std::string room;
    unsigned player = 0; // 0: automatic, 1 or 2: preferred/required slot
    unsigned delay = 2;
};

struct Input {
    uint32_t frame;
    uint16_t word;
};

struct Checksum {
    uint32_t frame;
    uint32_t crc;
};

class Transport {
public:
    Transport(const TransportOptions& opts, const Identity& identity);
    ~Transport();

    Transport(const Transport&) = delete;
    Transport& operator=(const Transport&) = delete;
    Transport(Transport&&) noexcept;
    Transport& operator=(Transport&&) noexcept;

    void pump(uint32_t simulated_frame, uint32_t confirmed_frame);
    bool ready() const;
    unsigned slot() const; // slot 0 or 1

    void submit(Input input);
    bool receive(Input& input);

    void checksum(Checksum cs);
    bool receive_checksum(Checksum& cs);

    void finish(uint32_t frame, uint32_t crc);
    bool finished() const;

    double rtt_ms() const;
    int frame_advantage() const;
    std::string status() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

} // namespace f3rt::netplay
