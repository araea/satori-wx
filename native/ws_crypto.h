#pragma once
#include <stdint.h>
#include <string.h>
namespace satori {
inline int Base64Digit(char c) {
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const char *p = c ? strchr(alphabet, c) : nullptr;
    return p ? static_cast<int>(p - alphabet) : -1;
}
inline bool WebSocketKey(const char *key) {
    if (strlen(key) != 24 || key[22] != '=' || key[23] != '=') return false;
    for (int i = 0; i < 22; ++i)
        if (Base64Digit(key[i]) < 0) return false;
    return (Base64Digit(key[21]) & 15) == 0;
}
inline uint32_t Rotate(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }
// SHA-1 only for the fixed RFC 6455 handshake (24-byte key + 36-byte GUID).
// This is not a password hash or a general cryptographic API.
inline void WebSocketAccept(const char *key, char out[29]) {
    uint8_t data[128] = {};
    memcpy(data, key, 24);
    memcpy(data + 24, "258EAFA5-E914-47DA-95CA-C5AB0DC85B11", 36);
    data[60] = 0x80;
    data[126] = 1;
    data[127] = 0xe0; // 60 * 8 bits, big endian.
    uint32_t h[5] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0};
    for (int block = 0; block < 2; ++block) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i) {
            const uint8_t *p = data + block * 64 + i * 4;
            w[i] = uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
        }
        for (int i = 16; i < 80; ++i) w[i] = Rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            const uint32_t f = i < 20   ? (b & c) | (~b & d)
                               : i < 40 ? b ^ c ^ d
                               : i < 60 ? (b & c) | (b & d) | (c & d)
                                        : b ^ c ^ d;
            const uint32_t k = i < 20 ? 0x5a827999 : i < 40 ? 0x6ed9eba1 : i < 60 ? 0x8f1bbcdc : 0xca62c1d6;
            const uint32_t next = Rotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rotate(b, 30);
            b = a;
            a = next;
        }
        h[0] += a;
        h[1] += b;
        h[2] += c;
        h[3] += d;
        h[4] += e;
    }
    uint8_t digest[20];
    for (int i = 0; i < 20; ++i) digest[i] = h[i / 4] >> (24 - (i % 4) * 8);
    const char *alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    int pos = 0;
    for (int i = 0; i < 20; i += 3) {
        const uint32_t n = uint32_t(digest[i]) << 16 | (i + 1 < 20 ? uint32_t(digest[i + 1]) << 8 : 0) |
                           (i + 2 < 20 ? digest[i + 2] : 0);
        out[pos++] = alphabet[(n >> 18) & 63];
        out[pos++] = alphabet[(n >> 12) & 63];
        out[pos++] = i + 1 < 20 ? alphabet[(n >> 6) & 63] : '=';
        out[pos++] = i + 2 < 20 ? alphabet[n & 63] : '=';
    }
    out[pos] = 0;
}
} // namespace satori
