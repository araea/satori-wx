#include "server.h"
#include "protocol.h"
#include "multipart.h"
#include "tempstore.h"
#include "upload_stream.h"
#include "version.h"
#include "webhook.h"
#include "ws_crypto.h"
#include "vendor/cjson/cJSON.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// Every error body is {"code": "<machine-readable slug>", "message": "<text>"}: clients branch on
// `code`. The two arguments are string literals, so the body is a compile-time constant.
#define ERROR_BODY(slug, text) "{\"code\":\"" slug "\",\"message\":\"" text "\"}"

namespace satori {
namespace {
constexpr size_t kHeader = 8192, kMessage = 16384, kInput = kHeader + kMessage;
// Only `upload.create` may carry a larger body, and only from an authenticated caller: the
// token is checked from the headers alone, before a single body byte is buffered.
constexpr size_t kUploadMax = 16u << 20;
// `upload.create` is the exception to the exception: a multipart body bigger than one JSON
// message is never buffered. It is parsed as it arrives and written straight to the temp store,
// so only the disk bounds it (a video is tens to hundreds of MiB). Same rule: the token is
// checked from the headers before the first body byte is read.
constexpr uint64_t kUploadStreamMax = 1ull << 30;
// A client that lets this much data back up unread is a stalled consumer, not a busy one.
constexpr size_t kOutputMax = 4u << 20;
// Stop replaying history into a connection while this much is still waiting to be written.
constexpr size_t kOutputSoft = 256u << 10;
constexpr size_t kChunk = 64u << 10;
constexpr int kClients = 8;
constexpr int64_t kRequestMs = 10000, kHeartbeatMs = 30000, kStreamMs = 20000;
// Registered by the module; null in tests and standalone tools.
StatusProvider g_status_provider = nullptr;
WakelockProvider g_wakelock_provider = nullptr;
PatProvider g_pat_provider = nullptr;
MediaResolver g_media_resolver = nullptr;
// Buffers are heap-allocated per connection and freed on Drop: eight fixed 60 KiB structs sat
// resident for the life of WeChat, and none of them could hold a real upload or a real image.
struct Client {
    int fd;
    bool ws, identified, closing, fragmented, continued;
    int64_t deadline;
    uint64_t cursor, replay_until;
    size_t used, pending, sent, fragments;
    size_t input_capacity, output_capacity;
    char *input, *output, *message;
    // A response body streamed from a file (proxy media): read a chunk whenever the socket
    // has drained, so a multi-megabyte video never has to fit in memory.
    int file_fd;
    uint64_t file_left;
    // A multipart upload being written to the temp store as it arrives (see kUploadStreamMax).
    struct UploadJob *upload;
};
struct UploadJob {
    UploadStream *stream;
    char platform[64], user[160];
};
void UploadFree(UploadJob *job) {
    if (!job) return;
    UploadEnd(job->stream);
    free(job);
}
int64_t Now() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}
bool TokenValid(const char *token) {
    const size_t size = strlen(token);
    if (size < 32 || size > 128) return false;
    for (size_t i = 0; i < size; ++i) {
        const unsigned char c = token[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_')) return false;
    }
    return true;
}
bool EqualToken(const char *a, const char *b) {
    const size_t size = strlen(a);
    if (size != strlen(b)) return false;
    unsigned diff = 0;
    for (size_t i = 0; i < size; ++i) diff |= static_cast<unsigned char>(a[i] ^ b[i]);
    return diff == 0;
}
void Drop(Client &c) {
    if (c.fd >= 0) {
        close(c.fd);
        // Only WebSocket clients count: transient HTTP requests would make the keeper toggle
        // the Wi-Fi lock on every status poll. c.ws stays set, so this cannot double-count.
        if (c.ws && g_client_count > 0) g_client_count = g_client_count - 1;
    }
    if (c.file_fd >= 0) close(c.file_fd);
    UploadFree(c.upload); c.upload = nullptr;
    free(c.input); free(c.output); free(c.message);
    c.fd = -1; c.file_fd = -1; c.file_left = 0;
    c.input = c.output = c.message = nullptr;
    c.input_capacity = c.output_capacity = c.used = c.pending = c.sent = c.fragments = 0;
}
// Makes room for `extra` more output bytes (compacting first). False when the connection is
// gone or the backlog would pass kOutputMax.
bool Reserve(Client &c, size_t extra) {
    if (c.fd < 0) return false;
    if (c.sent) {
        memmove(c.output, c.output + c.sent, c.pending - c.sent);
        c.pending -= c.sent; c.sent = 0;
    }
    if (extra > kOutputMax - c.pending) return false;
    const size_t need = c.pending + extra;
    if (need > c.output_capacity) {
        size_t capacity = c.output_capacity ? c.output_capacity : 4096;
        while (capacity < need) capacity *= 2;
        if (capacity > kOutputMax) capacity = kOutputMax;
        char *grown = static_cast<char *>(realloc(c.output, capacity));
        if (!grown) return false;
        c.output = grown; c.output_capacity = capacity;
    }
    return true;
}
bool Queue(Client &c, const void *data, size_t size) {
    if (!Reserve(c, size)) { Drop(c); return false; }
    memcpy(c.output + c.pending, data, size); c.pending += size;
    return true;
}
void Reply(Client &c, int code, const char *reason, const char *body, const char *allow = "POST") {
    char header[384];
    char allowed[48] = {};
    if (code == 405) snprintf(allowed, sizeof(allowed), "Allow: %s\r\n", allow);
    const int size = snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
                              "Content-Length: %zu\r\nConnection: close\r\nCache-Control: no-store\r\n%s\r\n",
                              code, reason, strlen(body), allowed);
    if (Queue(c, header, size)) Queue(c, body, strlen(body));
    c.closing = true;
}
void Frame(Client &c, unsigned opcode, const void *payload, size_t size) {
    unsigned char header[10] = {static_cast<unsigned char>(0x80 | opcode), static_cast<unsigned char>(size)};
    size_t n = 2;
    if (size >= 65536) {
        header[1] = 127;
        for (int i = 0; i < 8; ++i) header[2 + i] = static_cast<unsigned char>(static_cast<uint64_t>(size) >> (56 - 8 * i));
        n = 10;
    } else if (size >= 126) {
        header[1] = 126; header[2] = size >> 8; header[3] = size & 255; n = 4;
    }
    if (Queue(c, header, n)) Queue(c, payload, size);
}
void Close(Client &c, unsigned code) {
    const unsigned char payload[] = {static_cast<unsigned char>(code >> 8), static_cast<unsigned char>(code)};
    Frame(c, 8, payload, sizeof(payload)); c.closing = true; c.deadline = Now() + 1000;
}
void Raw(Client &c, int code, const char *reason, const char *content_type, const void *data, size_t size, bool cors) {
    char header[384];
    const int n = snprintf(header, sizeof(header), "HTTP/1.1 %d %s\r\nContent-Type: %s\r\nContent-Length: %zu\r\n"
                           "Connection: close\r\nCache-Control: no-store\r\n%s\r\n",
                           code, reason, content_type, size, cors ? "Access-Control-Allow-Origin: *\r\n" : "");
    if (Queue(c, header, n)) Queue(c, data, size);
    c.closing = true;
}
void RawJson(Client &c, int code, const char *reason, const char *body) {
    Raw(c, code, reason, "application/json", body, strlen(body), true);
}
// Reads the next chunk of a streamed body into the (drained) output buffer.
bool FillFromFile(Client &c) {
    const size_t want = c.file_left < kChunk ? static_cast<size_t>(c.file_left) : kChunk;
    if (!Reserve(c, want)) return false;
    ssize_t n;
    do { n = read(c.file_fd, c.output + c.pending, want); } while (n < 0 && errno == EINTR);
    if (n <= 0) return false;  // The file shrank under us: better to cut the connection than lie.
    c.pending += static_cast<size_t>(n);
    c.file_left -= static_cast<uint64_t>(n);
    if (!c.file_left) { close(c.file_fd); c.file_fd = -1; }
    return true;
}
// Parses a single `bytes=a-b` / `bytes=a-` / `bytes=-n` range against `size`. Returns 0 when
// there is no (usable) Range header, 1 for a satisfiable range, -1 when it lies outside the
// file. Multiple ranges are ignored: the whole file is a valid answer to any of them.
int ParseRange(const char *header, uint64_t size, uint64_t *first, uint64_t *last) {
    if (!header || strncasecmp(header, "bytes=", 6)) return 0;
    const char *p = header + 6;
    if (strchr(p, ',')) return 0;
    auto number = [&](uint64_t *out) {
        if (*p < '0' || *p > '9') return false;
        uint64_t value = 0;
        while (*p >= '0' && *p <= '9') {
            if (value > (UINT64_MAX - 9) / 10) return false;
            value = value * 10 + static_cast<uint64_t>(*p++ - '0');
        }
        *out = value; return true;
    };
    uint64_t a = 0, b = 0;
    if (*p == '-') {
        ++p;
        if (!number(&b) || *p || b == 0) return 0;
        if (size == 0) return -1;
        *first = b >= size ? 0 : size - b; *last = size - 1;
        return 1;
    }
    if (!number(&a) || *p++ != '-') return 0;
    if (*p) { if (!number(&b) || *p || b < a) return 0; } else b = size ? size - 1 : 0;
    if (a >= size) return -1;
    *first = a; *last = b >= size ? size - 1 : b;
    return 1;
}
// Answers with the header now and streams the body from `fd` as the socket drains. Takes
// ownership of `fd`. `range` is the raw Range header value (or empty).
void StreamFile(Client &c, int fd, const char *content_type, const char *range, bool head) {
    struct stat info {};
    if (fstat(fd, &info) || !S_ISREG(info.st_mode)) {
        close(fd); RawJson(c, 404, "Not Found", ERROR_BODY("not_found", "not found")); return;
    }
    const uint64_t size = static_cast<uint64_t>(info.st_size);
    uint64_t first = 0, last = size ? size - 1 : 0;
    const int ranged = ParseRange(range, size, &first, &last);
    if (ranged < 0) {
        close(fd);
        char header[256];
        const int n = snprintf(header, sizeof(header), "HTTP/1.1 416 Range Not Satisfiable\r\nContent-Range: bytes */%llu\r\n"
                               "Content-Length: 0\r\nConnection: close\r\nAccess-Control-Allow-Origin: *\r\n\r\n",
                               static_cast<unsigned long long>(size));
        Queue(c, header, n); c.closing = true; return;
    }
    const uint64_t length = size ? last - first + 1 : 0;
    char header[512];
    int n = snprintf(header, sizeof(header), "HTTP/1.1 %s\r\nContent-Type: %s\r\nContent-Length: %llu\r\n"
                     "Accept-Ranges: bytes\r\nX-Content-Type-Options: nosniff\r\nConnection: close\r\n"
                     "Cache-Control: no-store\r\nAccess-Control-Allow-Origin: *\r\n",
                     ranged ? "206 Partial Content" : "200 OK", content_type,
                     static_cast<unsigned long long>(length));
    if (ranged) n += snprintf(header + n, sizeof(header) - static_cast<size_t>(n), "Content-Range: bytes %llu-%llu/%llu\r\n",
                              static_cast<unsigned long long>(first), static_cast<unsigned long long>(last),
                              static_cast<unsigned long long>(size));
    n += snprintf(header + n, sizeof(header) - static_cast<size_t>(n), "\r\n");
    if (n <= 0 || static_cast<size_t>(n) >= sizeof(header) || !Queue(c, header, static_cast<size_t>(n))) { close(fd); return; }
    c.closing = true;
    if (head || !length || (first && lseek(fd, static_cast<off_t>(first), SEEK_SET) < 0)) { close(fd); return; }
    c.file_fd = fd; c.file_left = length;
    c.deadline = Now() + kStreamMs;
}
// Decodes %XX in place; false on a malformed escape or an embedded NUL.
bool PercentDecode(const char *in, char *out, size_t capacity) {
    size_t used = 0;
    for (; *in; ++in) {
        unsigned char ch = static_cast<unsigned char>(*in);
        if (ch == '%') {
            auto hex = [](char h) { return h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1; };
            const int high = in[1] ? hex(in[1]) : -1, low = high >= 0 && in[2] ? hex(in[2]) : -1;
            if (high < 0 || low < 0) return false;
            ch = static_cast<unsigned char>(high * 16 + low); in += 2;
            if (!ch) return false;
        }
        if (used + 1 >= capacity) return false;
        out[used++] = static_cast<char>(ch);
    }
    out[used] = 0;
    return true;
}
// `/v1/proxy/{url}` per Satori's resource route: external links must match an advertised
// proxy_urls prefix (none here, so they are 403), while `internal:{platform}/{user}/{path}`
// links are served by the owning login. Two internal routes exist: `_tmp` (the target of the
// built-in upload.create) and whatever the registered media resolver accepts (received
// message media, signed by this process). This route deliberately needs no Authorization
// header so a plain <img src> can use it. Clients differ on whether they percent-encode the
// link, so it is decoded first.
void Proxy(Client &c, Hub *hub, const char *raw_url, const char *range, bool head) {
    char url_buf[2048];
    if (!PercentDecode(raw_url, url_buf, sizeof(url_buf))) { RawJson(c, 400, "Bad Request", ERROR_BODY("invalid_url", "invalid url")); return; }
    const char *url = url_buf;
    if (!strncmp(url, "internal:", 9)) {
        const char *platform = url + 9, *slash = strchr(platform, '/');
        if (!slash || slash == platform) { RawJson(c, 400, "Bad Request", ERROR_BODY("invalid_internal_url", "invalid internal url")); return; }
        const char *user = slash + 1, *slash2 = strchr(user, '/');
        if (!slash2 || slash2 == user || !slash2[1]) { RawJson(c, 400, "Bad Request", ERROR_BODY("invalid_internal_url", "invalid internal url")); return; }
        char platform_buf[64], user_buf[160];
        const size_t platform_size = slash - platform, user_size = slash2 - user;
        if (platform_size >= sizeof(platform_buf) || user_size >= sizeof(user_buf)) { RawJson(c, 400, "Bad Request", ERROR_BODY("invalid_internal_url", "invalid internal url")); return; }
        memcpy(platform_buf, platform, platform_size); platform_buf[platform_size] = 0;
        memcpy(user_buf, user, user_size); user_buf[user_size] = 0;
        if (!FindLogin(hub, platform_buf, user_buf)) { RawJson(c, 404, "Not Found", ERROR_BODY("login_not_found", "login not found")); return; }
        const char *path = slash2 + 1;
        if (!strncmp(path, "_tmp/", 5)) {
            const TempFile *file = TempStoreGet(path + 5);
            if (!file) { RawJson(c, 404, "Not Found", ERROR_BODY("not_found", "not found")); return; }
            const int fd = open(file->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0) { RawJson(c, 404, "Not Found", ERROR_BODY("not_found", "not found")); return; }
            StreamFile(c, fd, file->content_type, range, head);
            return;
        }
        MediaFile media;
        if (g_media_resolver && g_media_resolver(user_buf, path, &media)) {
            const int fd = open(media.path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0) { RawJson(c, 404, "Not Found", ERROR_BODY("not_found", "not found")); return; }
            StreamFile(c, fd, media.content_type, range, head);
            return;
        }
        RawJson(c, 404, "Not Found", ERROR_BODY("unknown_internal_route", "unknown internal route"));
        return;
    }
    const bool http = !strncmp(url, "http://", 7) || !strncmp(url, "https://", 8);
    const char *host = url + (http ? (url[4] == 's' ? 8 : 7) : 0);
    if (!http || !*host || *host == '/' || *host == '?' || *host == '#') {
        RawJson(c, 400, "Bad Request", ERROR_BODY("invalid_url", "invalid url")); return;
    }
    const cJSON *urls = cJSON_GetObjectItemCaseSensitive(Meta(hub), "proxy_urls");
    for (const cJSON *p = urls ? urls->child : nullptr; p; p = p->next)
        if (cJSON_IsString(p) && *p->valuestring && !strncmp(url, p->valuestring, strlen(p->valuestring))) {
            // Advertised prefixes are never registered: this build has no outbound HTTP client.
            RawJson(c, 501, "Not Implemented", ERROR_BODY("proxy_not_implemented", "proxying external urls is not implemented")); return;
        }
    RawJson(c, 403, "Forbidden", ERROR_BODY("forbidden", "proxy url not allowed"));
}
void Upload(Client &c, const char *platform, const char *user, const Multipart *uploads) {
    auto valid_id = [](const char *id) {
        const size_t size = strlen(id);
        if (!size || size >= 128) return false;
        for (size_t i = 0; i < size; ++i) {
            const char ch = id[i];
            if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) return false;
        }
        return true;
    };
    if (!uploads || !uploads->count || !valid_id(platform) || !valid_id(user)) {
        Reply(c, 400, "Bad Request", ERROR_BODY("invalid_upload", "invalid upload")); return;
    }
    if (!TempStoreAvailable()) { Reply(c, 501, "Not Implemented", ERROR_BODY("upload_unavailable", "upload temp store unavailable")); return; }
    cJSON *result = cJSON_CreateObject();
    if (!result) { Reply(c, 500, "Internal Server Error", ERROR_BODY("internal_error", "internal error")); return; }
    for (size_t i = 0; i < uploads->count; ++i) {
        const Part &part = uploads->parts[i];
        char name[160];
        if (!TempStorePut(part.filename, part.content_type, part.data, part.size, name, sizeof(name))) {
            cJSON_Delete(result); Reply(c, 500, "Internal Server Error", ERROR_BODY("upload_failed", "upload failed")); return;
        }
        char url[512];
        snprintf(url, sizeof(url), "internal:%s/%s/_tmp/%s", platform, user, name);
        if (!cJSON_AddStringToObject(result, part.name, url)) {
            cJSON_Delete(result); Reply(c, 500, "Internal Server Error", ERROR_BODY("internal_error", "internal error")); return;
        }
    }
    char *text = cJSON_PrintUnformatted(result); cJSON_Delete(result);
    Reply(c, text ? 200 : 500, text ? "OK" : "Internal Server Error", text ? text : "{}"); free(text);
}
void Signal(Client &c, const char *data, size_t size, const Config &config, Hub *hub) {
    if (!Utf8(data, size)) { Close(c, 1007); return; }
    cJSON *root = Json(data, size);
    if (!root) { Close(c, 4000); return; }
    const cJSON *op = cJSON_GetObjectItemCaseSensitive(root, "op");
    const cJSON *body = cJSON_GetObjectItemCaseSensitive(root, "body");
    if (!cJSON_IsNumber(op)) Close(c, 4000);
    else if (op->valuedouble == 3 && !c.identified && cJSON_IsObject(body)) {
        const cJSON *token = cJSON_GetObjectItemCaseSensitive(body, "token");
        const cJSON *sn = cJSON_GetObjectItemCaseSensitive(body, "sn");
        if (!cJSON_IsString(token) || !EqualToken(config.token, token->valuestring)) Close(c, 4004);
        else if (sn && (!cJSON_IsNumber(sn) || !isfinite(sn->valuedouble) || sn->valuedouble < 0 ||
                        sn->valuedouble > 9007199254740991.0 || floor(sn->valuedouble) != sn->valuedouble)) {
            Close(c, 4000);
        } else {
            // A cursor this process cannot honour (it belongs to an earlier process, or has
            // fallen out of the replay window) is not an error: the reference server never
            // refuses IDENTIFY either, and clients such as adapter-satori keep the last `sn`
            // across restarts, so rejecting would leave them reconnecting forever. Answer READY
            // and continue live; `satori_wx.session_id` in it tells a client whether the
            // server restarted.
            const bool resume = sn && CanResume(hub, static_cast<uint64_t>(sn->valuedouble));
            c.identified = true; c.deadline = Now() + kHeartbeatMs;
            c.replay_until = Latest(hub);
            c.cursor = resume ? static_cast<uint64_t>(sn->valuedouble) : Latest(hub);
            cJSON *body = ReadyBody(hub);
            char *ready = body ? Envelope(4, body) : nullptr;
            cJSON_Delete(body);
            if (ready) { Frame(c, 1, ready, strlen(ready)); free(ready); }
            else Close(c, 1011);
        }
    } else if (op->valuedouble == 1 && c.identified) {
        c.deadline = Now() + kHeartbeatMs;
        Frame(c, 1, "{\"op\":2}", 8);
    } else Close(c, 4000);
    cJSON_Delete(root);
}
void WebSocket(Client &c, const Config &config, Hub *hub) {
    // Bound work per client per iteration, including coalesced small frames.
    for (int budget = 0; budget < 32 && c.fd >= 0 && !c.closing; ++budget) {
        if (c.used < 2) return;
        const auto *bytes = reinterpret_cast<const unsigned char *>(c.input);
        const unsigned op = bytes[0] & 15;
        const bool fin = (bytes[0] & 0x80) != 0;
        uint64_t size = bytes[1] & 127;
        size_t header = 2;
        if ((bytes[0] & 0x70) || !(bytes[1] & 0x80) ||
            (op != 0 && op != 1 && op != 2 && op != 8 && op != 9 && op != 10)) { Close(c, 1002); return; }
        if (op >= 8 && (!fin || size > 125)) { Close(c, 1002); return; }
        if (size == 126) {
            if (c.used < 4) return;
            size = (unsigned(bytes[2]) << 8) | bytes[3]; header = 4;
            if (size < 126) { Close(c, 1002); return; }
        } else if (size == 127) {
            if (c.used < 10) return;
            if (bytes[2] & 128) { Close(c, 1002); return; }
            size = 0;
            for (int i = 2; i < 10; ++i) size = (size << 8) | bytes[i];
            header = 10;
            if (size < 65536) { Close(c, 1002); return; }
        }
        if (size > kMessage) { Close(c, 1009); return; }
        if (c.used < header + 4 + size) return;
        char *payload = c.input + header + 4;
        for (size_t i = 0; i < size; ++i) payload[i] ^= c.input[header + i % 4];
        if (op == 8) {
            if (size == 1) { Close(c, 1002); return; }
            if (size >= 2) {
                const auto *p = reinterpret_cast<unsigned char *>(payload);
                const unsigned code = unsigned(p[0]) << 8 | p[1];
                if (!(code >= 3000 && code <= 4999) &&
                    !(code >= 1000 && code <= 1014 && code != 1004 && code != 1005 && code != 1006)) {
                    Close(c, 1002); return;
                }
                if (!Utf8(payload + 2, size - 2)) { Close(c, 1007); return; }
            }
            Frame(c, 8, payload, size); c.closing = true; c.deadline = Now() + 1000;
        } else if (op == 9) Frame(c, 10, payload, size);
        else if (op == 10) { /* Transport pong does not replace Satori PING. */ }
        else if (op == 2) { Close(c, 1003); return; }
        else {
            if ((op == 0 && !c.fragmented) || (op == 1 && c.fragmented)) { Close(c, 1002); return; }
            if (size > kMessage - c.fragments) { Close(c, 1009); return; }
            memcpy(c.message + c.fragments, payload, size); c.fragments += size;
            c.fragmented = !fin;
            if (fin) {
                c.message[c.fragments] = 0;
                Signal(c, c.message, c.fragments, config, hub); c.fragments = 0;
            }
        }
        // A Drop inside Frame/Signal frees the buffers, so nothing below may touch them.
        if (c.fd < 0 || !c.input) return;
        const size_t consumed = header + 4 + size;
        memmove(c.input, c.input + consumed, c.used - consumed); c.used -= consumed;
    }
}
// Feeds what has arrived of a streamed upload body to its parser and answers when it is over.
void UploadPump(Client &c) {
    UploadJob *job = c.upload;
    const UploadState state = UploadFeed(job->stream, c.input, c.used);
    c.used = 0;
    if (state == UploadState::More) { c.deadline = Now() + kRequestMs; return; }
    if (state == UploadState::Bad) { Reply(c, 400, "Bad Request", ERROR_BODY("invalid_upload", "invalid upload")); UploadFree(job); c.upload = nullptr; return; }
    if (state == UploadState::Failed) { Reply(c, 500, "Internal Server Error", ERROR_BODY("upload_failed", "upload failed")); UploadFree(job); c.upload = nullptr; return; }
    cJSON *result = cJSON_CreateObject();
    bool ok = result != nullptr;
    for (size_t i = 0; ok && i < UploadCount(job->stream); ++i) {
        char url[512];
        snprintf(url, sizeof(url), "internal:%s/%s/_tmp/%s", job->platform, job->user, UploadStoredName(job->stream, i));
        ok = cJSON_AddStringToObject(result, UploadField(job->stream, i), url) != nullptr;
    }
    char *text = ok ? cJSON_PrintUnformatted(result) : nullptr;
    Reply(c, text ? 200 : 500, text ? "OK" : "Internal Server Error", text ? text : "{}");
    free(text); cJSON_Delete(result);
    UploadFree(job); c.upload = nullptr;
}
struct Header { char *name, *value; };
bool HeaderToken(const char *list, const char *token) {
    const size_t n = strlen(token);
    while (*list) {
        while (*list == ' ' || *list == '\t' || *list == ',') ++list;
        const char *end = strchr(list, ',');
        if (!end) end = list + strlen(list);
        const char *trim = end;
        while (trim > list && (trim[-1] == ' ' || trim[-1] == '\t')) --trim;
        if (static_cast<size_t>(trim - list) == n && strncasecmp(list, token, n) == 0) return true;
        list = end;
    }
    return false;
}
void Http(Client &c, const Config &config, Hub *hub, const Backend *backend, WebHooks *hooks) {
    if (c.upload) { UploadPump(c); return; }
    c.input[c.used] = 0;
    const char *end = static_cast<const char *>(memmem(c.input, c.used, "\r\n\r\n", 4));
    if (!end) {
        if (c.used >= kHeader) Reply(c, 431, "Request Header Fields Too Large", ERROR_BODY("headers_too_large", "request headers too large"));
        return;
    }
    const size_t length = end - c.input + 4;
    if (length > kHeader) { Reply(c, 431, "Request Header Fields Too Large", ERROR_BODY("headers_too_large", "request headers too large")); return; }
    char scratch[kHeader + 1]; memcpy(scratch, c.input, length); scratch[length] = 0;
    auto bad = [&]() { Reply(c, 400, "Bad Request", ERROR_BODY("invalid_request", "invalid request")); };
    for (size_t i = 0; i < length; ++i) {
        const unsigned char ch = scratch[i];
        if ((ch < 32 && ch != '\r' && ch != '\n' && ch != '\t') || ch == 127) { bad(); return; }
    }
    char *line_end = strstr(scratch, "\r\n");
    if (!line_end) { bad(); return; }
    *line_end = 0;
    char *method = scratch, *path = strchr(method, ' ');
    if (!path) { bad(); return; }
    *path++ = 0; char *version = strchr(path, ' ');
    if (!version) { bad(); return; }
    *version++ = 0;
    if (!*method || *path != '/' || strchr(path, '\t') || strcmp(version, "HTTP/1.1")) { bad(); return; }
    Header headers[32]; int count = 0;
    for (char *p = line_end + 2; *p != '\r';) {
        line_end = strstr(p, "\r\n");
        if (!line_end || count == 32) { bad(); return; }
        *line_end = 0; char *value = strchr(p, ':');
        if (!value || value == p) { bad(); return; }
        *value++ = 0;
        for (const char *key = p; *key; ++key)
            if (!((*key >= 'a' && *key <= 'z') || (*key >= 'A' && *key <= 'Z') ||
                  (*key >= '0' && *key <= '9') || strchr("!#$%&'*+-.^_`|~", *key))) { bad(); return; }
        while (*value == ' ' || *value == '\t') ++value;
        char *tail = line_end;
        while (tail > value && (tail[-1] == ' ' || tail[-1] == '\t')) *--tail = 0;
        for (int i = 0; i < count; ++i) if (!strcasecmp(headers[i].name, p)) { bad(); return; }
        for (const char *v = value; *v; ++v)
            if ((static_cast<unsigned char>(*v) < 32 && *v != '\t') || *v == 127) { bad(); return; }
        if (!strcasecmp(p, "Content-Length") && !*value) { bad(); return; }
        headers[count++] = {p, value}; p = line_end + 2;
    }
    auto header = [&](const char *name) {
        for (int i = 0; i < count; ++i) if (!strcasecmp(headers[i].name, name)) return headers[i].value;
        return const_cast<char *>("");
    };
    if (!*header("Host") || *header("Transfer-Encoding")) { bad(); return; }
    const char *expect = header("Expect");
    const bool wants_continue = !strcasecmp(expect, "100-continue");
    if (*expect && !wants_continue) { Reply(c, 417, "Expectation Failed", ERROR_BODY("expectation_failed", "expectation failed")); return; }
    uint64_t announced = 0;
    for (const char *p = header("Content-Length"); *p; ++p) {
        if (*p < '0' || *p > '9') { bad(); return; }
        announced = announced * 10 + static_cast<uint64_t>(*p - '0');
        if (announced > kUploadStreamMax) { Reply(c, 413, "Content Too Large", ERROR_BODY("payload_too_large", "payload too large")); return; }
    }
    const bool streamed_upload = announced > kMessage && !strcmp(path, "/v1/upload.create");
    if (!streamed_upload && announced > kUploadMax) { Reply(c, 413, "Content Too Large", ERROR_BODY("payload_too_large", "payload too large")); return; }
    const size_t body_size = streamed_upload ? 0 : static_cast<size_t>(announced);
    if (streamed_upload) {
        // Everything that could refuse the request is checked now, from the headers alone, so a
        // refused upload costs no body at all (the connection closes with the answer).
        const char *auth = header("Authorization");
        if (!*auth) { Reply(c, 401, "Unauthorized", ERROR_BODY("missing_token", "missing token")); return; }
        if (strncasecmp(auth, "Bearer ", 7) || !EqualToken(config.token, auth + 7)) {
            Reply(c, 403, "Forbidden", ERROR_BODY("invalid_token", "invalid token")); return;
        }
        if (strcmp(method, "POST")) { Reply(c, 405, "Method Not Allowed", ERROR_BODY("method_not_allowed", "method not allowed")); return; }
        const char *content_type = header("Content-Type");
        if (strncasecmp(content_type, "multipart/form-data;", 20) || !strstr(content_type, "boundary=")) {
            Reply(c, 415, "Unsupported Media Type", ERROR_BODY("unsupported_media_type", "expected application/json")); return;
        }
        if (!*header("Satori-Platform") || !*header("Satori-User-ID")) { Reply(c, 400, "Bad Request", ERROR_BODY("missing_login_headers", "missing Satori-Platform or Satori-User-ID header")); return; }
        const cJSON *login = FindLogin(hub, header("Satori-Platform"), header("Satori-User-ID"));
        if (!login) { Reply(c, 403, "Forbidden", ERROR_BODY("login_not_found", "login not found")); return; }
        if (cJSON_GetObjectItemCaseSensitive(login, "status")->valuedouble != 1) { Reply(c, 503, "Service Unavailable", ERROR_BODY("login_offline", "login offline")); return; }
        bool supported = false;
        const cJSON *features = cJSON_GetObjectItemCaseSensitive(login, "features");
        for (const cJSON *f = features ? features->child : nullptr; f; f = f->next)
            if (cJSON_IsString(f) && !strcmp(f->valuestring, "upload.create")) supported = true;
        if (!supported) { Reply(c, 404, "Not Found", ERROR_BODY("unsupported_method", "unsupported method")); return; }
        if (!TempStoreAvailable()) { Reply(c, 501, "Not Implemented", ERROR_BODY("upload_unavailable", "upload temp store unavailable")); return; }
        auto valid_id = [](const char *id) {
            const size_t size = strlen(id);
            if (!size || size >= 128) return false;
            for (size_t i = 0; i < size; ++i) {
                const char ch = id[i];
                if (!((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '_' || ch == '-' || ch == '.')) return false;
            }
            return true;
        };
        if (!valid_id(header("Satori-Platform")) || !valid_id(header("Satori-User-ID")) ||
            strlen(header("Satori-Platform")) >= sizeof(UploadJob::platform) || strlen(header("Satori-User-ID")) >= sizeof(UploadJob::user)) {
            Reply(c, 400, "Bad Request", ERROR_BODY("invalid_upload", "invalid upload")); return;
        }
        auto *job = static_cast<UploadJob *>(calloc(1, sizeof(UploadJob)));
        if (!job) { Reply(c, 503, "Service Unavailable", ERROR_BODY("out_of_memory", "out of memory")); return; }
        job->stream = UploadBegin(content_type, announced);
        if (!job->stream) { free(job); Reply(c, 400, "Bad Request", ERROR_BODY("invalid_upload", "invalid upload")); return; }
        snprintf(job->platform, sizeof(job->platform), "%s", header("Satori-Platform"));
        snprintf(job->user, sizeof(job->user), "%s", header("Satori-User-ID"));
        c.upload = job;
        c.deadline = Now() + kRequestMs;
        // The body starts right behind the headers; whatever already arrived is the first chunk.
        memmove(c.input, c.input + length, c.used - length); c.used -= length;
        if (wants_continue && !c.continued && !c.used) {
            static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
            c.continued = true;
            Queue(c, interim, sizeof(interim) - 1);
        }
        if (c.used) UploadPump(c);
        return;
    }
    // Everything but the two routes that can carry a picture keeps the small JSON limit
    // (message.create takes an <img src="data:..."> inline, upload.create takes multipart). A
    // large body is only buffered for a caller that has already proven it holds the token.
    if (body_size > kMessage) {
        if (strcmp(path, "/v1/upload.create") && strcmp(path, "/v1/message.create")) { Reply(c, 413, "Content Too Large", ERROR_BODY("payload_too_large", "payload too large")); return; }
        const char *auth = header("Authorization");
        if (!*auth) { Reply(c, 401, "Unauthorized", ERROR_BODY("missing_token", "missing token")); return; }
        if (strncasecmp(auth, "Bearer ", 7) || !EqualToken(config.token, auth + 7)) {
            Reply(c, 403, "Forbidden", ERROR_BODY("invalid_token", "invalid token")); return;
        }
        const size_t need = length + body_size + 1;
        if (need > c.input_capacity) {
            char *grown = static_cast<char *>(realloc(c.input, need));
            if (!grown) { Reply(c, 503, "Service Unavailable", ERROR_BODY("out_of_memory", "out of memory")); return; }
            c.input = grown; c.input_capacity = need;
        }
    }
    if (c.used < length + body_size) {
        // curl and some SDKs hold the body back until the server says to go ahead.
        if (wants_continue && !c.continued) {
            static const char interim[] = "HTTP/1.1 100 Continue\r\n\r\n";
            c.continued = true;
            Queue(c, interim, sizeof(interim) - 1);
        }
        return;
    }
    if (!strcmp(path, "/v1/events")) {
        if (strcmp(method, "GET")) { Reply(c, 405, "Method Not Allowed", ERROR_BODY("method_not_allowed", "method not allowed"), "GET"); return; }
        if (body_size || strcasecmp(header("Upgrade"), "websocket") ||
            !HeaderToken(header("Connection"), "upgrade") || strcmp(header("Sec-WebSocket-Version"), "13") ||
            !WebSocketKey(header("Sec-WebSocket-Key"))) { bad(); return; }
        char accept[29], response[256]; WebSocketAccept(header("Sec-WebSocket-Key"), accept);
        const int n = snprintf(response, sizeof(response), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                               "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
        c.message = static_cast<char *>(malloc(kMessage + 1));
        if (!c.message) { Reply(c, 503, "Service Unavailable", ERROR_BODY("out_of_memory", "out of memory")); return; }
        if (!Queue(c, response, n)) return;
        c.ws = true; g_client_count = g_client_count + 1; c.deadline = Now() + kRequestMs;
        memmove(c.input, c.input + length, c.used - length); c.used -= length;
        return;
    }
    if (!strncmp(path, "/v1/proxy/", 10)) {
        const bool head = !strcmp(method, "HEAD");
        if (strcmp(method, "GET") && !head) { Reply(c, 405, "Method Not Allowed", ERROR_BODY("method_not_allowed", "method not allowed"), "GET, HEAD"); return; }
        if (body_size) { Reply(c, 400, "Bad Request", ERROR_BODY("invalid_request", "invalid request")); return; }
        Proxy(c, hub, path + 10, header("Range"), head);
        return;
    }
    const char *auth = header("Authorization");
    if (!*auth) { Reply(c, 401, "Unauthorized", ERROR_BODY("missing_token", "missing token")); return; }
    if (strncasecmp(auth, "Bearer ", 7) || !EqualToken(config.token, auth + 7)) {
        Reply(c, 403, "Forbidden", ERROR_BODY("invalid_token", "invalid token")); return;
    }
    const bool meta = !strcmp(path, "/v1/meta");
    const bool status = !strcmp(path, "/v1/internal/status");
    const bool capabilities = !strcmp(path, "/v1/internal/capabilities");
    const bool webhook_create = !strcmp(path, "/v1/meta/webhook.create");
    const bool webhook_delete = !strcmp(path, "/v1/meta/webhook.delete");
    const bool wakelock = !strcmp(path, "/v1/internal/wakelock");
    const bool pat = !strcmp(path, "/v1/internal/pat");
    const Method *rpc = !strncmp(path, "/v1/", 4) ? FindMethod(path + 4) : nullptr;
    if (!meta && !status && !capabilities && !webhook_create && !webhook_delete && !wakelock && !pat && !rpc) { Reply(c, 404, "Not Found", ERROR_BODY("not_found", "unknown API route")); return; }
    if (strcmp(method, "POST")) { Reply(c, 405, "Method Not Allowed", ERROR_BODY("method_not_allowed", "method not allowed")); return; }
    cJSON *body = nullptr;
    Multipart uploads{};
    const char *type = header("Content-Type");
    if (rpc && rpc->upload) {
        if (strncasecmp(type, "multipart/form-data;", 20) || !strstr(type, "boundary=")) {
            Reply(c, 415, "Unsupported Media Type", ERROR_BODY("unsupported_media_type", "expected application/json")); return;
        }
        if (!ParseMultipart(type, c.input + length, body_size, &uploads)) { bad(); return; }
    } else {
        if (body_size && (strncasecmp(type, "application/json", 16) || (type[16] && type[16] != ';'))) {
            Reply(c, 415, "Unsupported Media Type", ERROR_BODY("unsupported_media_type", "expected application/json")); return;
        }
        body = body_size ? Json(c.input + length, body_size) : cJSON_CreateObject();
        // Structural parse only here; a method's own parameter rules are checked after we
        // know the login actually supports it, so an unsupported method answers 404 (not a
        // misleading 400) whatever its parameters look like.
        if (!body) { bad(); return; }
    }
    if (meta) {
        char *text = cJSON_PrintUnformatted(Meta(hub));
        Reply(c, text ? 200 : 500, text ? "OK" : "Internal Server Error", text ? text : "{}"); free(text);
    } else if (status) {
        cJSON *result = cJSON_CreateObject();
        if (result) {
            cJSON_AddStringToObject(result, "version", SATORI_WX_VERSION);
            cJSON_AddBoolToObject(result, "native", true);
            cJSON_AddNumberToObject(result, "pid", static_cast<double>(getpid()));
            cJSON_AddStringToObject(result, "backend", backend ? "attached" : "unavailable");
            cJSON_AddBoolToObject(result, "event_replay", true);
            cJSON_AddNumberToObject(result, "replay_capacity", static_cast<double>(kHistory));
            cJSON_AddNumberToObject(result, "standard_methods", static_cast<double>(kMethodCount));
            cJSON_AddNumberToObject(result, "sequence", static_cast<double>(Latest(hub)));
            if (g_status_provider) g_status_provider(result, false);
            char *text = cJSON_PrintUnformatted(result);
            Reply(c, text ? 200 : 500, text ? "OK" : "Internal Server Error", text ? text : "{}"); free(text);
            cJSON_Delete(result);
        } else {
            Reply(c, 500, "Internal Server Error", ERROR_BODY("internal_error", "internal error"));
        }
    } else if (capabilities) {
        cJSON *result = cJSON_CreateObject();
        if (result) {
            // standard_methods is what the login serves: login.features without the `guild.plain`
            // flag, the same list satori-qq reports. adapter / version / platform / unsupported /
            // event_types / message_elements / limits come from the adapter (g_status_provider),
            // in the vocabulary satori-qq answers with, so a client negotiates without knowing which.
            cJSON *methods = cJSON_CreateArray();
            if (methods && cJSON_AddItemToObject(result, "standard_methods", methods)) {
                const cJSON *logins = cJSON_GetObjectItemCaseSensitive(Meta(hub), "logins");
                const cJSON *features = logins && logins->child ? cJSON_GetObjectItemCaseSensitive(logins->child, "features") : nullptr;
                for (const cJSON *f = features ? features->child : nullptr; f; f = f->next)
                    if (cJSON_IsString(f) && strcmp(f->valuestring, "guild.plain")) cJSON_AddItemToArray(methods, cJSON_CreateString(f->valuestring));
            } else {
                cJSON_Delete(methods);
            }
            cJSON_AddBoolToObject(result, "wechat_backend", backend != nullptr);
            cJSON_AddBoolToObject(result, "webhook", true);
            cJSON_AddNumberToObject(result, "webhooks", static_cast<double>(WebHookCount(hooks)));
            cJSON_AddBoolToObject(result, "proxy", true);
            cJSON_AddBoolToObject(result, "upload", TempStoreAvailable());
            if (g_status_provider) g_status_provider(result, true);
            char *text = cJSON_PrintUnformatted(result);
            Reply(c, text ? 200 : 500, text ? "OK" : "Internal Server Error", text ? text : "{}"); free(text);
            cJSON_Delete(result);
        } else {
            Reply(c, 500, "Internal Server Error", ERROR_BODY("internal_error", "internal error"));
        }
    } else if (wakelock) {
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(body, "on");
        const cJSON *toggle = cJSON_GetObjectItemCaseSensitive(body, "toggle");
        if (!g_wakelock_provider) {
            Reply(c, 501, "Not Implemented", ERROR_BODY("backend_not_implemented", "backend not implemented"));
        } else if (cJSON_IsBool(on) || (cJSON_IsBool(toggle) && toggle->valueint)) {
            const int action = cJSON_IsBool(on) ? (on->valueint ? 1 : 0) : 2;
            bool held = false;
            g_wakelock_provider(action, &held);
            cJSON *result = cJSON_CreateObject();
            if (result) {
                cJSON_AddBoolToObject(result, "on", held);
                cJSON_AddBoolToObject(result, "held", held);
                char *text = cJSON_PrintUnformatted(result);
                Reply(c, text ? 200 : 500, text ? "OK" : "Internal Server Error", text ? text : "{}"); free(text);
                cJSON_Delete(result);
            } else {
                Reply(c, 500, "Internal Server Error", ERROR_BODY("internal_error", "internal error"));
            }
        } else {
            Reply(c, 400, "Bad Request", ERROR_BODY("invalid_request", "invalid request"));
        }
    } else if (pat) {
        // WeChat's "拍一戳". Not a Satori method: an internal extra on the same account rules
        // as the rpc methods (token + login headers), handled by the module's provider.
        const cJSON *channel_id = cJSON_GetObjectItemCaseSensitive(body, "channel_id");
        const cJSON *user_id = cJSON_GetObjectItemCaseSensitive(body, "user_id");
        if (!*header("Satori-Platform") || !*header("Satori-User-ID")) {
            Reply(c, 400, "Bad Request", ERROR_BODY("missing_login_headers", "missing Satori-Platform or Satori-User-ID header"));
        } else if (!FindLogin(hub, header("Satori-Platform"), header("Satori-User-ID"))) {
            Reply(c, 403, "Forbidden", ERROR_BODY("login_not_found", "login not found"));
        } else if (!g_pat_provider) {
            Reply(c, 501, "Not Implemented", ERROR_BODY("backend_not_implemented", "backend not implemented"));
        } else if (!cJSON_IsString(channel_id) || !*channel_id->valuestring || !cJSON_IsString(user_id) || !*user_id->valuestring) {
            Reply(c, 400, "Bad Request", ERROR_BODY("invalid_request", "invalid request"));
        } else {
            char detail[160] = {};
            bool rejected = false;
            const bool ok = g_pat_provider(channel_id->valuestring, user_id->valuestring, &rejected, detail, sizeof(detail));
            if (ok) {
                Reply(c, 200, "OK", "{\"ok\":true}");
            } else {
                cJSON *body_out = cJSON_CreateObject();
                if (body_out) {
                    cJSON_AddStringToObject(body_out, "code", "pat_failed");
                    cJSON_AddStringToObject(body_out, "message", detail[0] ? detail : "pat failed");
                    if (rejected) cJSON_AddBoolToObject(body_out, "rejected", true);
                    char *text = cJSON_PrintUnformatted(body_out);
                    Reply(c, 502, "Request Failed", text ? text : "{}");
                    free(text);
                    cJSON_Delete(body_out);
                } else {
                    Reply(c, 500, "Internal Server Error", ERROR_BODY("internal_error", "internal error"));
                }
            }
        }
    } else if (webhook_create || webhook_delete) {
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(body, "url");
        const cJSON *token = cJSON_GetObjectItemCaseSensitive(body, "token");
        bool ok = cJSON_IsString(url) && *url->valuestring && (!token || cJSON_IsString(token));
        if (ok) {
            if (webhook_create) ok = AddWebHook(hooks, url->valuestring, token ? token->valuestring : "");
            else ok = RemoveWebHook(hooks, url->valuestring);
        }
        Reply(c, ok ? 200 : 400, ok ? "OK" : "Bad Request", ok ? "{}" : ERROR_BODY("invalid_webhook", "invalid webhook"));
    } else if (!*header("Satori-Platform") || !*header("Satori-User-ID")) {
        Reply(c, 400, "Bad Request", ERROR_BODY("missing_login_headers", "missing Satori-Platform or Satori-User-ID header"));
    } else {
        const cJSON *login = FindLogin(hub, header("Satori-Platform"), header("Satori-User-ID"));
        if (!login) Reply(c, 403, "Forbidden", ERROR_BODY("login_not_found", "login not found"));
        else if (!strcmp(rpc->name, "login.get")) {
            char *text = cJSON_PrintUnformatted(login);
            Reply(c, text ? 200 : 500, text ? "OK" : "Internal Server Error", text ? text : "{}"); free(text);
        } else if (cJSON_GetObjectItemCaseSensitive(login, "status")->valuedouble != 1) {
            Reply(c, 503, "Service Unavailable", ERROR_BODY("login_offline", "login offline"));
        } else {
            bool supported = false;
            const cJSON *features = cJSON_GetObjectItemCaseSensitive(login, "features");
            for (const cJSON *f = features ? features->child : nullptr; f; f = f->next)
                if (cJSON_IsString(f) && !strcmp(f->valuestring, rpc->name)) supported = true;
            if (!supported) Reply(c, 404, "Not Found", ERROR_BODY("unsupported_method", "unsupported method"));
            else if (rpc->upload) Upload(c, header("Satori-Platform"), header("Satori-User-ID"), &uploads);
            else if (!ValidateParams(*rpc, body)) Reply(c, 400, "Bad Request", ERROR_BODY("invalid_request", "invalid request"));
            else if (!backend || !backend->call) Reply(c, 501, "Not Implemented", ERROR_BODY("backend_not_implemented", "backend not implemented"));
            else {
                const Request request{rpc, header("Satori-Platform"), header("Satori-User-ID"), body, type, c.input + length, body_size, rpc->upload ? &uploads : nullptr};
                Response response = backend->call(backend->context, request);
                const int code = response.status >= 200 && response.status <= 599 ? response.status : 500;
                char *text = response.body ? cJSON_PrintUnformatted(response.body) : nullptr;
                Reply(c, code, code < 300 ? "OK" : code == 404 ? "Not Found" : code == 501 ? "Not Implemented" : "Request Failed", text ? text : "{}");
                free(text); cJSON_Delete(response.body);
            }
        }
    }
    cJSON_Delete(body);
}
} // namespace

void SetStatusProvider(StatusProvider provider) { g_status_provider = provider; }
void SetWakelockProvider(WakelockProvider provider) { g_wakelock_provider = provider; }
void SetPatProvider(PatProvider provider) { g_pat_provider = provider; }
void SetTempDir(const char *dir) { TempStoreSetDir(dir); }
void SetMediaResolver(MediaResolver resolver) { g_media_resolver = resolver; }

bool ReadConfig(int fd, Config *config) {
    char data[1025]; size_t used = 0;
    for (;;) {
        const ssize_t n = read(fd, data + used, sizeof(data) - 1 - used);
        if (n < 0 && errno == EINTR) continue;
        if (n < 0) return false;
        if (!n) break;
        used += n;
        if (used == sizeof(data) - 1) return false;
    }
    if (memchr(data, 0, used)) return false;
    data[used] = 0;
    Config parsed; bool token_seen = false, port_seen = false, send_seen = false, allow_seen = false;
    char *state = nullptr;
    for (char *line = strtok_r(data, "\n", &state); line; line = strtok_r(nullptr, "\n", &state)) {
        const size_t n = strlen(line);
        if (n && line[n - 1] == '\r') line[n - 1] = 0;
        if (!*line || *line == '#') continue;
        if (!strncmp(line, "token=", 6) && !token_seen) {
            if (!TokenValid(line + 6)) return false;
            strcpy(parsed.token, line + 6); token_seen = true;
        } else if (!strncmp(line, "port=", 5) && !port_seen) {
            unsigned port = 0;
            for (const char *p = line + 5; *p; ++p) {
                if (*p < '0' || *p > '9') return false;
                port = port * 10 + (*p - '0'); if (port > 65535) return false;
            }
            if (port < 1024) return false;
            parsed.port = port; port_seen = true;
        } else if (!strncmp(line, "send=", 5) && !send_seen) {
            // Retired switch: the value must still be on|off (a typo stays a hard error), but it
            // changes nothing.
            const char *value = line + 5;
            if (strcmp(value, "on") && strcmp(value, "off")) return false;
            send_seen = true;
        } else if (!strncmp(line, "send_allow=", 11) && !allow_seen) {
            // Retired whitelist; accepted and ignored for the same reason.
            allow_seen = true;
        } else return false;
    }
    if (!token_seen) return false;
    *config = parsed; return true;
}
int Listen(const Config &config) {
    if (!TokenValid(config.token)) { errno = EINVAL; return -1; }
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
    if (fd < 0) return -1;
    const int yes = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{}; address.sin_family = AF_INET;
    address.sin_port = htons(config.port); address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(fd, kClients)) {
        const int error = errno; close(fd); errno = error; return -1;
    }
    return fd;
}
void Run(int listener, const Config &config, EventBus *bus, const Backend *backend) {
    Hub *hub = CreateHub();
    if (!hub) { close(listener); return; }
    WebHooks *hooks = CreateWebHooks();
    if (!hooks) { DestroyHub(hub); close(listener); return; }
    auto *clients = static_cast<Client *>(calloc(kClients, sizeof(Client)));
    if (!clients) { DestroyHub(hub); close(listener); return; }
    for (int i = 0; i < kClients; ++i) { clients[i].fd = -1; clients[i].file_fd = -1; }
    for (;;) {
        // Drain a bounded native producer queue before polling clients.
        DrainWake(bus);
        bool meta = false;
        for (int budget = 0; budget < 32; ++budget) {
            char *event = Take(bus, &meta);
            if (!event) break;
            char *signal = Apply(hub, event, meta);
            free(event);
            if (!signal) continue;
            // Message events use per-client history cursors, preserving order and applying backpressure.
            const bool immediate = meta;
            if (immediate) {
                for (int i = 0; i < kClients; ++i) {
                    Client &c = clients[i];
                    if (c.fd >= 0 && c.identified && !c.closing) Frame(c, 1, signal, strlen(signal));
                }
            }
            // Optional WebHook delivery is best-effort and never blocks the poll loop.
            if (WebHookCount(hooks)) {
                char *body = EnvelopeBody(signal);
                if (body) { PushWebHook(hooks, immediate ? 5 : 0, body); free(body); }
            }
            free(signal);
        }
        pollfd fds[kClients + 2]{};
        fds[kClients + 1] = {BusFd(bus), POLLIN, 0};
        fds[0] = {listener, POLLIN, 0};
        int timeout = -1;
        for (int i = 0; i < kClients; ++i) {
            Client &c = clients[i];
            if (c.fd >= 0 && Now() >= c.deadline) {
                if (c.ws && !c.closing) Close(c, c.identified ? 4000 : 4004);
                else Drop(c);
            }
            if (c.fd >= 0 && c.identified && !c.closing) {
                if (!CanDeliver(hub, c.cursor, c.replay_until)) Close(c, 4009);
                else for (unsigned budget = 0; budget < kHistory; ++budget) {
                    uint64_t next = c.cursor;
                    const char *event = NextEvent(hub, &next, c.replay_until);
                    if (!event) { c.cursor = next; break; }
                    // Hold history back while a slow reader still has a backlog, but always let
                    // one event through so a single large message can never wedge the stream.
                    if (c.pending - c.sent > kOutputSoft) break;
                    Frame(c, 1, event, strlen(event)); c.cursor = next;
                    if (c.fd < 0) break;
                }
            }
            const size_t before = c.used;
            if (c.fd >= 0 && c.ws && !c.closing && c.used) WebSocket(c, config, hub);
            if (c.fd >= 0) {
                const int64_t remaining = c.deadline - Now();
                int wait = remaining > 0 ? static_cast<int>(remaining) : 0;
                if (c.used && c.used < before) wait = 0;
                if (timeout < 0 || wait < timeout) timeout = wait;
            }
            fds[i + 1] = {c.fd, static_cast<short>((c.closing ? 0 : POLLIN) | (c.pending > c.sent || c.file_left ? POLLOUT : 0)), 0};
        }
        const int ready = poll(fds, kClients + 2, timeout);
        if (ready < 0) { if (errno == EINTR) continue; break; }
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL)) break;
        if (fds[0].revents & POLLIN) {
            // Accept a bounded number so a connection flood cannot starve existing clients.
            for (int count = 0; count < kClients; ++count) {
                const int fd = accept4(listener, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK);
                if (fd < 0) break;
                Client *slot = nullptr;
                for (int i = 0; i < kClients; ++i) if (clients[i].fd < 0) { slot = &clients[i]; break; }
                if (!slot) { close(fd); continue; }
                memset(slot, 0, sizeof(*slot));
                slot->input = static_cast<char *>(malloc(kInput + 1));
                if (!slot->input) { close(fd); continue; }
                slot->input_capacity = kInput + 1;
                slot->fd = fd; slot->file_fd = -1; slot->deadline = Now() + kRequestMs;
            }
        }
        for (int i = 0; i < kClients; ++i) {
            Client &c = clients[i]; const short events = fds[i + 1].revents;
            if (c.fd < 0 || fds[i + 1].fd != c.fd) continue;
            if (events & (POLLERR | POLLNVAL)) { Drop(c); continue; }
            if (!c.closing && (events & POLLIN)) {
                const ssize_t n = recv(c.fd, c.input + c.used, c.input_capacity - 1 - c.used, 0);
                if (n > 0) {
                    c.used += n;
                    if (c.ws) WebSocket(c, config, hub); else Http(c, config, hub, backend, hooks);
                    if (c.fd >= 0 && c.used == c.input_capacity - 1 && !c.closing) { if (c.ws) Close(c, 1009); else Reply(c, 413, "Content Too Large", ERROR_BODY("payload_too_large", "payload too large")); }
                } else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { Drop(c); continue; }
            }
            if (c.fd >= 0 && (c.pending > c.sent || c.file_left) && (events & POLLOUT)) {
                // A streamed body is pulled from its file only once the socket has drained.
                if (c.pending == c.sent && c.file_left && !FillFromFile(c)) { Drop(c); continue; }
                if (c.pending > c.sent) {
                    const ssize_t n = send(c.fd, c.output + c.sent, c.pending - c.sent, MSG_NOSIGNAL);
                    if (n > 0) { c.sent += n; if (c.file_left) c.deadline = Now() + kStreamMs; }
                    else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { Drop(c); continue; }
                }
                if (c.sent == c.pending && !c.file_left) { c.sent = c.pending = 0; if (c.closing) Drop(c); }
            }
            if (c.fd >= 0 && (events & POLLHUP) && !(events & POLLIN)) Drop(c);
        }
    }
    for (int i = 0; i < kClients; ++i) Drop(clients[i]);
    free(clients); DestroyHub(hub); DestroyWebHooks(hooks); close(listener);
}
} // namespace satori
