#include "token_service.hpp"
#include "httplib.h"
#if __has_include("netcode.h")
#include "netcode.h"
#else
extern "C" {
int netcode_generate_connect_token(int num_server_addresses, const char **public_server_addresses,
                                   const char **internal_server_addresses, int expire_seconds,
                                   int timeout_seconds, uint64_t client_id, uint64_t protocol_id,
                                   const uint8_t *private_key, uint8_t *user_data,
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
    : m_burst(burst), m_refill_per_sec(refill_per_sec), m_max_buckets(max_buckets) {}

bool IpRateLimiter::allow(std::string_view ip, double now) {
    std::lock_guard<std::mutex> lock(m_mu);
    if (m_last_prune_time <= 0.0) {
        m_last_prune_time = now;
    } else if (now - m_last_prune_time >= 1.0) {
        prune(now);
        m_last_prune_time = now;
    }

    std::string key(ip);
    auto it = m_buckets.find(key);
    if (it == m_buckets.end()) {
        if (m_buckets.size() >= m_max_buckets) {
            return false;
        }
        Bucket b;
        b.tokens = m_burst;
        b.last_time = now;
        it = m_buckets.emplace(std::move(key), b).first;
    }

    Bucket &b = it->second;
    double elapsed = now - b.last_time;
    if (elapsed > 0.0) {
        b.tokens = std::min(static_cast<double>(m_burst), b.tokens + elapsed * m_refill_per_sec);
    }
    b.last_time = now;

    if (b.tokens < 1.0) {
        return false;
    }
    b.tokens -= 1.0;
    return true;
}

void IpRateLimiter::prune(double now, double idle_seconds) {
    for (auto it = m_buckets.begin(); it != m_buckets.end();) {
        if (now - it->second.last_time > idle_seconds) {
            it = m_buckets.erase(it);
        } else {
            ++it;
        }
    }
}

size_t IpRateLimiter::size() const {
    std::lock_guard<std::mutex> lock(m_mu);
    return m_buckets.size();
}

std::optional<std::array<uint8_t, yojimbo::ConnectTokenBytes>>
default_netcode_mint_fn(uint64_t client_id, const std::string &public_address, uint64_t protocol_id,
                        std::span<const uint8_t, 32> private_key, int expire_sec, int timeout_sec) {

    std::array<uint8_t, yojimbo::ConnectTokenBytes> token{};
    const char *addrs[] = {public_address.c_str()};
    uint8_t user_data[NETCODE_USER_DATA_BYTES]{0};

    int res =
        netcode_generate_connect_token(1, addrs, addrs, expire_sec, timeout_sec, client_id,
                                       protocol_id, private_key.data(), user_data, token.data());

    if (res != NETCODE_OK) {
        return std::nullopt;
    }
    return token;
}

struct TokenServer::Impl {
    httplib::Server svr;
    std::thread worker;
};

TokenServer::TokenServer(Config config, std::span<const uint8_t, 32> private_key,
                         TokenMintFn mint_fn)
    : m_config(std::move(config)), m_mint_fn(std::move(mint_fn)),
      m_limiter(m_config.rate_burst, m_config.rate_refill), m_impl(std::make_unique<Impl>()) {
    std::copy_n(private_key.data(), 32, m_private_key.begin());

    m_impl->svr.Get(std::string(TOKEN_ROUTE), [this](const httplib::Request &req,
                                                     httplib::Response &res) {
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

        if (!m_limiter.allow(remote_ip, now)) {
            res.status = 429;
            res.set_content("rate limited\n", "text/plain");
            return;
        }

        // Server ALWAYS generates client_id (random 64-bit). Never accept client-supplied id.
        uint64_t client_id = generate_random_u64();

        auto token = m_mint_fn(client_id, m_config.public_address, m_config.protocol_id,
                               m_private_key, m_config.expire_seconds, m_config.timeout_seconds);

        if (!token.has_value()) {
            res.status = 500;
            res.set_content("failed to mint token\n", "text/plain");
            return;
        }

        // Binary response: u64 LE client_id + 2048-byte token = 2056 bytes
        std::vector<uint8_t> binary_body = encode_token_response(client_id, *token);

        res.status = 200;
        res.set_content(reinterpret_cast<const char *>(binary_body.data()), binary_body.size(),
                        "application/octet-stream");
    });
}

TokenServer::~TokenServer() { stop(); }

bool TokenServer::start() {
    if (m_running) {
        return true;
    }

    if (m_config.port == 0) {
        int port = m_impl->svr.bind_to_any_port(m_config.host);
        if (port <= 0) {
            return false;
        }
        m_bound_port = port;
    } else {
        if (!m_impl->svr.bind_to_port(m_config.host, m_config.port)) {
            return false;
        }
        m_bound_port = m_config.port;
    }

    m_impl->worker = std::thread([this]() { m_impl->svr.listen_after_bind(); });
    m_impl->svr.wait_until_ready();

    m_running = true;
    return true;
}

void TokenServer::stop() {
    if (!m_running) {
        return;
    }
    m_impl->svr.stop();
    if (m_impl->worker.joinable()) {
        m_impl->worker.join();
    }
    m_running = false;
    m_bound_port = 0;
}

} // namespace f3rt::netplay
