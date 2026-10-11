#include "token_service.hpp"
#include "httplib.h"
#if __has_include("netcode.h")
#include "netcode.h"
#else
extern "C" {
int netcode_generate_connect_token(int num_server_addresses,
                                    const char **public_server_addresses,
                                    const char **internal_server_addresses,
                                    int expire_seconds,
                                    int timeout_seconds,
                                    uint64_t client_id,
                                    uint64_t protocol_id,
                                    const uint8_t *private_key,
                                    uint8_t *user_data,
                                    uint8_t *connect_token);
}
#define NETCODE_CONNECT_TOKEN_BYTES 2048
#define NETCODE_USER_DATA_BYTES 256
#endif

#include <chrono>
#include <random>
#include <thread>

namespace f3rt::netplay {

namespace {

uint64_t generate_random_u64() {
    static thread_local std::random_device rd;
    static thread_local std::mt19937_64 gen(rd());
    return gen();
}

double get_system_time_seconds() {
    using namespace std::chrono;
    auto now = steady_clock::now().time_since_epoch();
    return duration<double>(now).count();
}

} // namespace

IpRateLimiter::IpRateLimiter(int burst, double refill_per_sec, size_t max_buckets)
    : burst_(burst), refill_per_sec_(refill_per_sec), max_buckets_(max_buckets) {}

bool IpRateLimiter::allow(std::string_view ip, double now) {
    std::lock_guard<std::mutex> lock(mu_);
    if (last_prune_time_ <= 0.0) {
        last_prune_time_ = now;
    } else if (now - last_prune_time_ >= 1.0) {
        prune(now);
        last_prune_time_ = now;
    }

    std::string key(ip);
    auto it = buckets_.find(key);
    if (it == buckets_.end()) {
        if (buckets_.size() >= max_buckets_) {
            return false;
        }
        Bucket b;
        b.tokens = burst_;
        b.last_time = now;
        it = buckets_.emplace(std::move(key), b).first;
    }

    Bucket &b = it->second;
    double elapsed = now - b.last_time;
    if (elapsed > 0.0) {
        b.tokens = std::min(static_cast<double>(burst_), b.tokens + elapsed * refill_per_sec_);
    }
    b.last_time = now;

    if (b.tokens < 1.0) {
        return false;
    }
    b.tokens -= 1.0;
    return true;
}

void IpRateLimiter::prune(double now, double idle_seconds) {
    for (auto it = buckets_.begin(); it != buckets_.end(); ) {
        if (now - it->second.last_time > idle_seconds) {
            it = buckets_.erase(it);
        } else {
            ++it;
        }
    }
}

size_t IpRateLimiter::size() const {
    std::lock_guard<std::mutex> lock(mu_);
    return buckets_.size();
}

std::optional<std::array<uint8_t, yojimbo::ConnectTokenBytes>> default_netcode_mint_fn(
    uint64_t client_id,
    const std::string &public_address,
    uint64_t protocol_id,
    std::span<const uint8_t, 32> private_key,
    int expire_sec,
    int timeout_sec) {

    std::array<uint8_t, yojimbo::ConnectTokenBytes> token{};
    const char *addrs[] = {public_address.c_str()};
    uint8_t user_data[NETCODE_USER_DATA_BYTES]{0};

    int res = netcode_generate_connect_token(
        1,
        addrs,
        addrs,
        expire_sec,
        timeout_sec,
        client_id,
        protocol_id,
        private_key.data(),
        user_data,
        token.data()
    );

    if (res != NETCODE_OK) {
        return std::nullopt;
    }
    return token;
}

struct TokenServer::Impl {
    httplib::Server svr;
    std::thread worker;
};

TokenServer::TokenServer(Config config, std::span<const uint8_t, 32> private_key, TokenMintFn mint_fn)
    : config_(std::move(config)), mint_fn_(std::move(mint_fn)),
      limiter_(config_.rate_burst, config_.rate_refill),
      impl_(std::make_unique<Impl>()) {
    std::copy_n(private_key.data(), 32, private_key_.begin());

    impl_->svr.Get(std::string(TOKEN_ROUTE), [this](const httplib::Request &req, httplib::Response &res) {
        // Enforce protocol version check via ?v=<PROTOCOL_VERSION>
        if (!req.has_param(std::string(TOKEN_VERSION_QUERY_PARAM))) {
            res.status = HTTP_STATUS_VERSION_MISMATCH; // 409
            res.set_content("version mismatch\n", "text/plain");
            return;
        }

        std::string v_str = req.get_param_value(std::string(TOKEN_VERSION_QUERY_PARAM));
        try {
            int v = std::stoi(v_str);
            if (v != PROTOCOL_VERSION) {
                res.status = HTTP_STATUS_VERSION_MISMATCH; // 409
                res.set_content("version mismatch\n", "text/plain");
                return;
            }
        } catch (...) {
            res.status = HTTP_STATUS_VERSION_MISMATCH;
            res.set_content("version mismatch\n", "text/plain");
            return;
        }

        double now = get_system_time_seconds();
        std::string remote_ip = req.remote_addr;

        if (!limiter_.allow(remote_ip, now)) {
            res.status = 429;
            res.set_content("rate limited\n", "text/plain");
            return;
        }

        // Server ALWAYS generates client_id (random 64-bit). Never accept client-supplied id.
        uint64_t client_id = generate_random_u64();

        auto token = mint_fn_(client_id, config_.public_address, config_.protocol_id,
                              private_key_, config_.expire_seconds, config_.timeout_seconds);

        if (!token.has_value()) {
            res.status = 500;
            res.set_content("failed to mint token\n", "text/plain");
            return;
        }

        // Binary response: u64 LE client_id + 2048-byte token = 2056 bytes
        std::vector<uint8_t> binary_body = encode_token_response(client_id, *token);

        res.status = 200;
        res.set_content(reinterpret_cast<const char *>(binary_body.data()),
                        binary_body.size(), "application/octet-stream");
    });
}

TokenServer::~TokenServer() {
    stop();
}

bool TokenServer::start() {
    if (running_) {
        return true;
    }

    if (config_.port == 0) {
        int port = impl_->svr.bind_to_any_port(config_.host);
        if (port <= 0) {
            return false;
        }
        bound_port_ = port;
    } else {
        if (!impl_->svr.bind_to_port(config_.host, config_.port)) {
            return false;
        }
        bound_port_ = config_.port;
    }

    impl_->worker = std::thread([this]() {
        impl_->svr.listen_after_bind();
    });
    impl_->svr.wait_until_ready();

    running_ = true;
    return true;
}

void TokenServer::stop() {
    if (!running_) {
        return;
    }
    impl_->svr.stop();
    if (impl_->worker.joinable()) {
        impl_->worker.join();
    }
    running_ = false;
    bound_port_ = 0;
}

} // namespace f3rt::netplay
