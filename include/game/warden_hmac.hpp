#pragma once

// HMAC-SHA1 for the Warden code scans, with the key's work done once.
//
// A code scan asks whether HMAC-SHA1(seed, bytes) matches a hash at any of
// ten million positions in the executable, under one 4-byte seed. A one-shot
// HMAC() redoes the key schedule - two SHA1 blocks of padding - and an EVP
// context at every position, which made a scan take ten seconds. Here the
// inner and outer states are computed once per seed, and a position costs the
// compressions of its own bytes: two blocks for a pattern of up to 55 bytes.

#include <cstddef>
#include <cstdint>
#include <cstring>

namespace wowee {
namespace game {

class WardenHmacSha1 {
public:
    WardenHmacSha1(const uint8_t* key, size_t keyLen) {
        uint8_t block[64] = {};
        std::memcpy(block, key, keyLen <= 64 ? keyLen : 64);  // Warden keys are 4 bytes
        uint8_t pad[64];
        for (int i = 0; i < 64; ++i) pad[i] = block[i] ^ 0x36;
        reset(inner_);
        compress(inner_, pad);
        for (int i = 0; i < 64; ++i) pad[i] = block[i] ^ 0x5C;
        reset(outer_);
        compress(outer_, pad);
    }

    /// HMAC-SHA1(key, data) into out[20].
    void digest(const uint8_t* data, size_t len, uint8_t out[20]) const {
        uint32_t h[5];
        std::memcpy(h, inner_, sizeof(h));
        finish(h, data, len, 64);
        uint8_t innerDigest[20];
        store(h, innerDigest);
        std::memcpy(h, outer_, sizeof(h));
        finish(h, innerDigest, 20, 64);
        store(h, out);
    }

    bool matches(const uint8_t* data, size_t len, const uint8_t expected[20]) const {
        uint8_t out[20];
        digest(data, len, out);
        return std::memcmp(out, expected, 20) == 0;
    }

private:
    uint32_t inner_[5];
    uint32_t outer_[5];

    static void reset(uint32_t h[5]) {
        h[0] = 0x67452301u; h[1] = 0xEFCDAB89u; h[2] = 0x98BADCFEu; h[3] = 0x10325476u; h[4] = 0xC3D2E1F0u;
    }

    static uint32_t rol(uint32_t x, int n) { return (x << n) | (x >> (32 - n)); }

    static void compress(uint32_t h[5], const uint8_t block[64]) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t(block[i * 4]) << 24) | (uint32_t(block[i * 4 + 1]) << 16) |
                   (uint32_t(block[i * 4 + 2]) << 8) | uint32_t(block[i * 4 + 3]);
        }
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);           k = 0x5A827999u; }
            else if (i < 40) { f = b ^ c ^ d;                    k = 0x6ED9EBA1u; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d);  k = 0x8F1BBCDCu; }
            else             { f = b ^ c ^ d;                    k = 0xCA62C1D6u; }
            const uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }

    // Absorb the message and its padding into a state that has already taken
    // `prefixLen` bytes (the 64-byte key block).
    static void finish(uint32_t h[5], const uint8_t* data, size_t len, size_t prefixLen) {
        size_t pos = 0;
        while (len - pos >= 64) {
            compress(h, data + pos);
            pos += 64;
        }
        uint8_t block[128] = {};
        const size_t rest = len - pos;
        std::memcpy(block, data + pos, rest);
        block[rest] = 0x80;
        const size_t blocks = rest + 9 <= 64 ? 1 : 2;
        const uint64_t bits = uint64_t(prefixLen + len) * 8;
        for (int i = 0; i < 8; ++i) block[blocks * 64 - 1 - i] = uint8_t(bits >> (8 * i));
        compress(h, block);
        if (blocks == 2) compress(h, block + 64);
    }

    static void store(const uint32_t h[5], uint8_t out[20]) {
        for (int i = 0; i < 5; ++i) {
            out[i * 4] = uint8_t(h[i] >> 24);
            out[i * 4 + 1] = uint8_t(h[i] >> 16);
            out[i * 4 + 2] = uint8_t(h[i] >> 8);
            out[i * 4 + 3] = uint8_t(h[i]);
        }
    }
};

}  // namespace game
}  // namespace wowee
