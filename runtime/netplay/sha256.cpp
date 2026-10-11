#include "sha256.hpp"
#include <algorithm>
#include <cstring>

namespace f3rt::netplay {

namespace {

inline uint32_t rotr(uint32_t x, unsigned n) {
    return (x >> n) | (x << (32 - n));
}

inline uint32_t ch(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (~x & z);
}

inline uint32_t maj(uint32_t x, uint32_t y, uint32_t z) {
    return (x & y) ^ (x & z) ^ (y & z);
}

inline uint32_t sigma0(uint32_t x) {
    return rotr(x, 2) ^ rotr(x, 13) ^ rotr(x, 22);
}

inline uint32_t sigma1(uint32_t x) {
    return rotr(x, 6) ^ rotr(x, 11) ^ rotr(x, 25);
}

inline uint32_t s0(uint32_t x) {
    return rotr(x, 7) ^ rotr(x, 18) ^ (x >> 3);
}

inline uint32_t s1(uint32_t x) {
    return rotr(x, 17) ^ rotr(x, 19) ^ (x >> 10);
}

constexpr uint32_t K[64] = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5,
    0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3,
    0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
    0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13,
    0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3,
    0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5,
    0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
};

} // namespace

Sha256::Sha256() {
    reset();
}

void Sha256::reset() {
    state_[0] = 0x6a09e667;
    state_[1] = 0xbb67ae85;
    state_[2] = 0x3c6ef372;
    state_[3] = 0xa54ff53a;
    state_[4] = 0x510e527f;
    state_[5] = 0x9b05688c;
    state_[6] = 0x1f83d9ab;
    state_[7] = 0x5be0cd19;
    count_ = 0;
    buffer_.fill(0);
}

void Sha256::transform(const uint8_t block[64]) {
    uint32_t w[64];
    for (int t = 0; t < 16; ++t) {
        w[t] = (uint32_t(block[t * 4]) << 24) |
               (uint32_t(block[t * 4 + 1]) << 16) |
               (uint32_t(block[t * 4 + 2]) << 8) |
               uint32_t(block[t * 4 + 3]);
    }
    for (int t = 16; t < 64; ++t) {
        w[t] = s1(w[t - 2]) + w[t - 7] + s0(w[t - 15]) + w[t - 16];
    }

    uint32_t a = state_[0];
    uint32_t b = state_[1];
    uint32_t c = state_[2];
    uint32_t d = state_[3];
    uint32_t e = state_[4];
    uint32_t f = state_[5];
    uint32_t g = state_[6];
    uint32_t h = state_[7];

    for (int t = 0; t < 64; ++t) {
        uint32_t t1 = h + sigma1(e) + ch(e, f, g) + K[t] + w[t];
        uint32_t t2 = sigma0(a) + maj(a, b, c);
        h = g;
        g = f;
        f = e;
        e = d + t1;
        d = c;
        c = b;
        b = a;
        a = t1 + t2;
    }

    state_[0] += a;
    state_[1] += b;
    state_[2] += c;
    state_[3] += d;
    state_[4] += e;
    state_[5] += f;
    state_[6] += g;
    state_[7] += h;
}

void Sha256::update(const void *data, size_t len) {
    const auto *src = static_cast<const uint8_t *>(data);
    size_t buf_idx = size_t(count_ % 64);
    count_ += len;

    if (buf_idx > 0) {
        size_t take = std::min(len, 64 - buf_idx);
        std::memcpy(buffer_.data() + buf_idx, src, take);
        buf_idx += take;
        src += take;
        len -= take;

        if (buf_idx == 64) {
            transform(buffer_.data());
            buf_idx = 0;
        }
    }

    while (len >= 64) {
        transform(src);
        src += 64;
        len -= 64;
    }

    if (len > 0) {
        std::memcpy(buffer_.data(), src, len);
    }
}

void Sha256::update(std::span<const uint8_t> data) {
    update(data.data(), data.size());
}

void Sha256::update(std::string_view sv) {
    update(sv.data(), sv.size());
}

Sha256Digest Sha256::finalize() {
    uint64_t total_bits = count_ * 8;
    size_t buf_idx = size_t(count_ % 64);

    buffer_[buf_idx++] = 0x80;
    if (buf_idx > 56) {
        std::fill(buffer_.begin() + buf_idx, buffer_.end(), 0);
        transform(buffer_.data());
        buf_idx = 0;
    }
    std::fill(buffer_.begin() + buf_idx, buffer_.begin() + 56, 0);

    for (int i = 0; i < 8; ++i) {
        buffer_[56 + i] = uint8_t(total_bits >> ((7 - i) * 8));
    }
    transform(buffer_.data());

    Sha256Digest digest{};
    for (int i = 0; i < 8; ++i) {
        digest[i * 4] = uint8_t(state_[i] >> 24);
        digest[i * 4 + 1] = uint8_t(state_[i] >> 16);
        digest[i * 4 + 2] = uint8_t(state_[i] >> 8);
        digest[i * 4 + 3] = uint8_t(state_[i]);
    }
    return digest;
}

Sha256Digest sha256(const void *data, size_t len) {
    Sha256 hasher;
    hasher.update(data, len);
    return hasher.finalize();
}

Sha256Digest sha256(std::span<const uint8_t> data) {
    return sha256(data.data(), data.size());
}

Sha256Digest sha256(std::string_view sv) {
    return sha256(sv.data(), sv.size());
}

Sha256Digest hmac_sha256(std::span<const uint8_t> key, std::span<const uint8_t> data) {
    std::array<uint8_t, 64> k0{};
    if (key.size() > 64) {
        auto hashed = sha256(key.data(), key.size());
        std::copy(hashed.begin(), hashed.end(), k0.begin());
    } else {
        std::copy(key.begin(), key.end(), k0.begin());
    }

    std::array<uint8_t, 64> k_ipad{}, k_opad{};
    for (size_t i = 0; i < 64; ++i) {
        k_ipad[i] = k0[i] ^ 0x36;
        k_opad[i] = k0[i] ^ 0x5c;
    }

    Sha256 inner;
    inner.update(k_ipad.data(), k_ipad.size());
    inner.update(data.data(), data.size());
    auto inner_hash = inner.finalize();

    Sha256 outer;
    outer.update(k_opad.data(), k_opad.size());
    outer.update(inner_hash.data(), inner_hash.size());
    return outer.finalize();
}

std::array<uint8_t, 16> hmac_sha256_tag16(std::span<const uint8_t> key, std::span<const uint8_t> data) {
    auto full = hmac_sha256(key, data);
    std::array<uint8_t, 16> tag{};
    std::copy_n(full.begin(), 16, tag.begin());
    return tag;
}

} // namespace f3rt::netplay
