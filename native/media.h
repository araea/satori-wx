#pragma once
#include <stddef.h>

// Hashing and link signing for served message media.
//
// `/v1/proxy/...` needs no Authorization header (that is what lets a client drop the link
// into an <img>), and WeChat message ids are small sequential integers. Unsigned links would
// therefore let any app on the phone walk msgId = 1, 2, 3... and read every photo the user
// ever received. Every media link this module emits carries an HMAC of (login, kind, id)
// under a key derived from the config token, so only links the module itself handed out
// verify. The key is the token, not a per-process secret, so links stay valid across a
// WeChat restart; rotating the token revokes them all.
namespace satori {
// RFC 1321 / RFC 6234 / RFC 2104. Digests are written as lowercase hex.
void Md5Hex(const void *data, size_t size, char out[33]);
void Sha256(const void *data, size_t size, unsigned char out[32]);
void HmacSha256(const void *key, size_t key_size, const void *data, size_t size, unsigned char out[32]);

// Derives the link-signing key from the configured token. Until it is called no link verifies.
void MediaSetSecret(const char *token);
constexpr size_t kMediaSigSize = 17; // 16 hex digits + NUL
void MediaSign(const char *user, const char *kind, const char *id, char out[kMediaSigSize]);
bool MediaVerify(const char *user, const char *kind, const char *id, const char *signature);
// `internal:wechat/<user>/_msg/<kind>/<id>/<signature>`; false when `capacity` is too small.
bool MediaLink(const char *user, const char *kind, const char *id, char *out, size_t capacity);
} // namespace satori
