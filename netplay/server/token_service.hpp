#pragma once

#include "netplay/shared/contract.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace f3rt::netplay {

// IP Rate Limiter using token bucket: 10 burst, 1.0 token/sec
class IpRateLimiter {
public:
    explicit IpRateLimiter(int burst = 10, double refill_per_sec = 1.0, size_t max_buckets = 4096);

    bool allow(std::string_view ip, double now);
    void prune(double now, double idle_seconds = 5.0);
    size_t size() const;

private:
    struct Bucket {
        double tokens = 10.0;
        double last_time = 0.0;
    };

    int burst_;
    double refill_per_sec_;
    size_t max_buckets_;
    double last_prune_time_ = 0.0;
    mutable std::mutex mu_;
    std::unordered_map<std::string, Bucket> buckets_;
};

// Signature for connect token generation:
// Takes (client_id, public_address, protocol_id, private_key_32b, expire_sec, timeout_sec)
// Returns 2048-byte token or nullopt on failure
using TokenMintFn = std::function<std::optional<std::array<uint8_t, yojimbo::ConnectTokenBytes>>(
    uint64_t client_id,
    const std::string &public_address,
    uint64_t protocol_id,
    std::span<const uint8_t, 32> private_key,
    int expire_sec,
    int timeout_sec)>;

// Default Netcode connect token minting function using netcode C API
std::optional<std::array<uint8_t, yojimbo::ConnectTokenBytes>> default_netcode_mint_fn(
    uint64_t client_id,
    const std::string &public_address,
    uint64_t protocol_id,
    std::span<const uint8_t, 32> private_key,
    int expire_sec,
    int timeout_sec);

// HTTP Server serving GET /token?v=<PROTOCOL_VERSION>
class TokenServer {
public:
    struct Config {
        std::string host = "0.0.0.0";
        int port = DEFAULT_PORT;
        std::string public_address = "127.0.0.1:7845";
        uint64_t protocol_id = PROTOCOL_ID;
        int expire_seconds = CONNECT_TOKEN_EXPIRY_SECONDS;
        int timeout_seconds = CONNECT_TOKEN_TIMEOUT_SECONDS;
        int rate_burst = 10;
        double rate_refill = 1.0;
    };

    explicit TokenServer(Config config, std::span<const uint8_t, 32> private_key,
                         TokenMintFn mint_fn = default_netcode_mint_fn);
    ~TokenServer();

    // Start listening on background thread
    bool start();

    // Stop HTTP server and join thread
    void stop();

    bool is_running() const { return running_; }
    int bound_port() const { return bound_port_; }

private:
    struct Impl;
    Config config_;
    std::array<uint8_t, 32> private_key_{};
    TokenMintFn mint_fn_;
    IpRateLimiter limiter_;
    std::unique_ptr<Impl> impl_;
    bool running_ = false;
    int bound_port_ = 0;
};

} // namespace f3rt::netplay
