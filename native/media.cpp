#include "media.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

namespace satori {
namespace {
uint32_t Rotl(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }
uint32_t Rotr(uint32_t x, unsigned n) { return (x >> n) | (x << (32 - n)); }

// ---- MD5 ------------------------------------------------------------------------------------
struct Md5 {
    uint32_t state[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    uint64_t length = 0;
    unsigned char block[64];
    size_t used = 0;
    void Transform(const unsigned char *chunk) {
        static const uint32_t k[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        static const unsigned s[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                       5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
                                       4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                       6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
        uint32_t m[16];
        for (int i = 0; i < 16; ++i)
            m[i] = uint32_t(chunk[i * 4]) | uint32_t(chunk[i * 4 + 1]) << 8 | uint32_t(chunk[i * 4 + 2]) << 16 | uint32_t(chunk[i * 4 + 3]) << 24;
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3];
        for (unsigned i = 0; i < 64; ++i) {
            uint32_t f;
            unsigned g;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            const uint32_t next = d;
            d = c; c = b;
            b = b + Rotl(a + f + k[i] + m[g], s[i]);
            a = next;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d;
    }
    void Update(const unsigned char *data, size_t size) {
        length += size;
        while (size) {
            const size_t take = 64 - used < size ? 64 - used : size;
            memcpy(block + used, data, take);
            used += take; data += take; size -= take;
            if (used == 64) { Transform(block); used = 0; }
        }
    }
    void Final(unsigned char out[16]) {
        const uint64_t bits = length * 8;
        const unsigned char pad = 0x80;
        Update(&pad, 1);
        const unsigned char zero = 0;
        while (used != 56) Update(&zero, 1);
        unsigned char tail[8];
        for (int i = 0; i < 8; ++i) tail[i] = static_cast<unsigned char>(bits >> (8 * i));
        Update(tail, 8);
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<unsigned char>(state[i] >> (8 * j));
    }
};

// ---- SHA-256 --------------------------------------------------------------------------------
struct Sha {
    uint32_t state[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    uint64_t length = 0;
    unsigned char block[64];
    size_t used = 0;
    void Transform(const unsigned char *chunk) {
        static const uint32_t k[64] = {
            0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
            0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
            0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
            0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
            0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
            0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
            0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
            0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
        uint32_t w[64];
        for (int i = 0; i < 16; ++i)
            w[i] = uint32_t(chunk[i * 4]) << 24 | uint32_t(chunk[i * 4 + 1]) << 16 | uint32_t(chunk[i * 4 + 2]) << 8 | uint32_t(chunk[i * 4 + 3]);
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = Rotr(w[i - 15], 7) ^ Rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = Rotr(w[i - 2], 17) ^ Rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4], f = state[5], g = state[6], h = state[7];
        for (int i = 0; i < 64; ++i) {
            const uint32_t s1 = Rotr(e, 6) ^ Rotr(e, 11) ^ Rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + s1 + ch + k[i] + w[i];
            const uint32_t s0 = Rotr(a, 2) ^ Rotr(a, 13) ^ Rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = s0 + maj;
            h = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        state[0] += a; state[1] += b; state[2] += c; state[3] += d; state[4] += e; state[5] += f; state[6] += g; state[7] += h;
    }
    void Update(const unsigned char *data, size_t size) {
        length += size;
        while (size) {
            const size_t take = 64 - used < size ? 64 - used : size;
            memcpy(block + used, data, take);
            used += take; data += take; size -= take;
            if (used == 64) { Transform(block); used = 0; }
        }
    }
    void Final(unsigned char out[32]) {
        const uint64_t bits = length * 8;
        const unsigned char pad = 0x80, zero = 0;
        Update(&pad, 1);
        while (used != 56) Update(&zero, 1);
        unsigned char tail[8];
        for (int i = 0; i < 8; ++i) tail[i] = static_cast<unsigned char>(bits >> (56 - 8 * i));
        Update(tail, 8);
        for (int i = 0; i < 8; ++i)
            for (int j = 0; j < 4; ++j) out[i * 4 + j] = static_cast<unsigned char>(state[i] >> (24 - 8 * j));
    }
};

unsigned char g_key[32];
bool g_key_set = false;

void Hex(const unsigned char *bytes, size_t n, char *out) {
    static const char digits[] = "0123456789abcdef";
    for (size_t i = 0; i < n; ++i) { out[i * 2] = digits[bytes[i] >> 4]; out[i * 2 + 1] = digits[bytes[i] & 15]; }
    out[n * 2] = 0;
}
} // namespace

void Md5Hex(const void *data, size_t size, char out[33]) {
    Md5 md5;
    md5.Update(static_cast<const unsigned char *>(data), size);
    unsigned char digest[16];
    md5.Final(digest);
    Hex(digest, 16, out);
}

void Sha256(const void *data, size_t size, unsigned char out[32]) {
    Sha sha;
    sha.Update(static_cast<const unsigned char *>(data), size);
    sha.Final(out);
}

void HmacSha256(const void *key, size_t key_size, const void *data, size_t size, unsigned char out[32]) {
    unsigned char block[64] = {};
    if (key_size > 64) Sha256(key, key_size, block);
    else if (key_size) memcpy(block, key, key_size);
    unsigned char inner_pad[64], outer_pad[64];
    for (int i = 0; i < 64; ++i) { inner_pad[i] = block[i] ^ 0x36; outer_pad[i] = block[i] ^ 0x5c; }
    Sha inner;
    inner.Update(inner_pad, 64);
    inner.Update(static_cast<const unsigned char *>(data), size);
    unsigned char digest[32];
    inner.Final(digest);
    Sha outer;
    outer.Update(outer_pad, 64);
    outer.Update(digest, 32);
    outer.Final(out);
}

void MediaSetSecret(const char *token) {
    static const char label[] = "satori-wx/media-link/v1";
    HmacSha256(token ? token : "", token ? strlen(token) : 0, label, sizeof(label) - 1, g_key);
    g_key_set = token && *token;
}

void MediaSign(const char *user, const char *kind, const char *id, char out[kMediaSigSize]) {
    char message[512];
    const int n = snprintf(message, sizeof(message), "%s\n%s\n%s", user ? user : "", kind ? kind : "", id ? id : "");
    unsigned char digest[32];
    HmacSha256(g_key, sizeof(g_key), message, n > 0 && static_cast<size_t>(n) < sizeof(message) ? static_cast<size_t>(n) : 0, digest);
    Hex(digest, 8, out);
}

bool MediaVerify(const char *user, const char *kind, const char *id, const char *signature) {
    if (!g_key_set || !user || !kind || !id || !signature || strlen(signature) != kMediaSigSize - 1) return false;
    char expected[kMediaSigSize];
    MediaSign(user, kind, id, expected);
    unsigned diff = 0;
    for (size_t i = 0; i < kMediaSigSize - 1; ++i) diff |= static_cast<unsigned char>(expected[i] ^ signature[i]);
    return diff == 0;
}

bool MediaLink(const char *user, const char *kind, const char *id, char *out, size_t capacity) {
    char signature[kMediaSigSize];
    MediaSign(user, kind, id, signature);
    const int n = snprintf(out, capacity, "internal:wechat/%s/_msg/%s/%s/%s", user, kind, id, signature);
    return n > 0 && static_cast<size_t>(n) < capacity;
}
} // namespace satori
