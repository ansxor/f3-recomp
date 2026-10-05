#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <span>

namespace f3rt::netplay {

struct Identity {
    std::array<uint32_t, 7> rom_crc;
    std::array<uint8_t, 32> build_hash;
    uint32_t state_format;
}; // ROM, executable, and canonical snapshot representation must match.

struct TransportOptions {
    std::string server;
    std::string room;
    unsigned player = 0; // 0: automatic, 1 or 2: preferred/required slot
    unsigned delay = 2;
    bool host = false; // authority is independent of player slot
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
    bool paired() const;
    bool host() const;
    unsigned delay() const;
    double transfer_progress() const; // 0..1, validated/acknowledged chunks
    void offer_snapshot(std::span<const uint8_t> bytes);
    bool snapshot_available() const;
    std::span<const uint8_t> snapshot() const;
    uint32_t snapshot_crc() const;
    void accept_snapshot(uint32_t loaded_crc);
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
