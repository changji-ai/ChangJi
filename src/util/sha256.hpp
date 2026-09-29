#pragma once

// SHA-256 和 base64url。**自己写一份，不拉 OpenSSL**：OpenSSL 是可选的（`CHANGJI_SSL`），
// 关着编的时候这几件照样要算得准。照 FIPS 180-4 写，用例里拿标准答案对过。
//
// 用它的：扩展「信任」认的那个指纹（agent/mcp_config.cpp，原来是 64 位 FNV-1a，不抗碰撞）、
// 场记云登录的 PKCE（`cloud/cloud.cpp`：code_challenge = base64url(sha256(verifier))）。
// 2026-09-27 从 agent/sha256.hpp 挪到这儿：引擎核心要用，而单独检出引擎时没有外层 agent/。

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>

namespace changji::util {

/// 原始的 32 字节。
inline std::array<unsigned char, 32> sha256_bytes(const std::string& data) {
    static constexpr std::uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4,
        0xab1c5ed5, 0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe,
        0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f,
        0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7,
        0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc,
        0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85, 0xa2bfe8a1, 0xa81a664b,
        0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116,
        0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7,
        0xc67178f2};
    std::array<std::uint32_t, 8> h = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                                      0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const auto rotr = [](std::uint32_t x, int n) { return (x >> n) | (x << (32 - n)); };

    // 补位：一个 0x80，补 0 到 56 mod 64，再接 64 位大端的位长。
    std::string msg = data;
    const std::uint64_t bits = static_cast<std::uint64_t>(data.size()) * 8;
    msg.push_back(static_cast<char>(0x80));
    while (msg.size() % 64 != 56) msg.push_back('\0');
    for (int i = 7; i >= 0; --i) msg.push_back(static_cast<char>((bits >> (i * 8)) & 0xff));

    for (std::size_t off = 0; off < msg.size(); off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            const auto b = [&](int j) {
                return static_cast<std::uint32_t>(static_cast<unsigned char>(msg[off + i * 4 + j]));
            };
            w[i] = (b(0) << 24) | (b(1) << 16) | (b(2) << 8) | b(3);
        }
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6],
                      hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const std::uint32_t ch = (e & f) ^ (~e & g);
            const std::uint32_t t1 = hh + S1 + ch + k[i] + w[i];
            const std::uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const std::uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const std::uint32_t t2 = S0 + maj;
            hh = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += hh;
    }

    std::array<unsigned char, 32> out{};
    for (int i = 0; i < 8; ++i) {
        for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<unsigned char>((h[i] >> (24 - j * 8)) & 0xff);
    }
    return out;
}

/// 小写十六进制那 64 个字。
inline std::string sha256_hex(const std::string& data) {
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (const unsigned char c : sha256_bytes(data)) {
        out.push_back(kHex[c >> 4]);
        out.push_back(kHex[c & 0xf]);
    }
    return out;
}

/// base64url（不带 `=`）：PKCE 的 code_challenge、随机口令都用它。
inline std::string base64url(const unsigned char* p, std::size_t n) {
    static constexpr char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    out.reserve((n + 2) / 3 * 4);
    std::size_t i = 0;
    for (; i + 2 < n; i += 3) {
        const std::uint32_t v = (std::uint32_t(p[i]) << 16) | (std::uint32_t(p[i + 1]) << 8) | p[i + 2];
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
        out.push_back(kB64[v & 63]);
    }
    if (i + 1 == n) {
        const std::uint32_t v = std::uint32_t(p[i]) << 16;
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
    } else if (i + 2 == n) {
        const std::uint32_t v = (std::uint32_t(p[i]) << 16) | (std::uint32_t(p[i + 1]) << 8);
        out.push_back(kB64[(v >> 18) & 63]);
        out.push_back(kB64[(v >> 12) & 63]);
        out.push_back(kB64[(v >> 6) & 63]);
    }
    return out;
}

}  // namespace changji::util
