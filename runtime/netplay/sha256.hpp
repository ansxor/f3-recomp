#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace f3rt::netplay {

constexpr size_t sha256_digest_size = 32;
constexpr size_t sha256_block_size = 64;

using Sha256Digest = std::array<uint8_t, sha256_digest_size>;

class Sha256 {
public:
    Sha256();
    void reset();
    void update(const void *data, size_t len);
    void update(std::span<const uint8_t> data);
    void update(std::string_view sv);
    Sha256Digest finalize();

private:
    void transform(const uint8_t block[64]);

    std::array<uint32_t, 8> state_{};
    uint64_t count_ = 0; // total bytes processed
    std::array<uint8_t, 64> buffer_{};
};

Sha256Digest sha256(const void *data, size_t len);
Sha256Digest sha256(std::span<const uint8_t> data);
Sha256Digest sha256(std::string_view sv);

// HMAC-SHA256 (RFC 2104 / RFC 4231)
Sha256Digest hmac_sha256(std::span<const uint8_t> key, std::span<const uint8_t> data);

// Returns first 16 bytes of HMAC-SHA256 (for UDP packet tag)
std::array<uint8_t, 16> hmac_sha256_tag16(std::span<const uint8_t> key, std::span<const uint8_t> data);

} // namespace f3rt::netplay
