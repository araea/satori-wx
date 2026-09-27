#include "server.h"
#include "protocol.h"
#include "multipart.h"
#include "tempstore.h"
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
#include <time.h>
#include <unistd.h>

namespace satori {
namespace {
constexpr size_t kHeader = 8192, kMessage = 16384, kInput = kHeader + kMessage;
constexpr int kClients = 8;
constexpr int64_t kRequestMs = 10000, kHeartbeatMs = 30000;
// Registered by the module; null in tests and standalone tools.
StatusProvider g_status_provider = nullptr;
WakelockProvider g_wakelock_provider = nullptr;
struct Client {
    int fd;
    bool ws, identified, closing, fragmented;
    int64_t deadline;
    uint64_t cursor, replay_until;
    size_t used, pending, sent, fragments;
    char input[kInput + 1], output[kMessage + 1024], message[kMessage + 1];
};
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
void Drop(Client &c) { if (c.fd >= 0) close(c.fd); c.fd = -1; }
bool Queue(Client &c, const void *data, size_t size) {
    if (c.sent) {
        memmove(c.output, c.output + c.sent, c.pending - c.sent);
        c.pending -= c.sent; c.sent = 0;
    }
    if (size > sizeof(c.output) - c.pending) { Drop(c); return false; }
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
    unsigned char header[4] = {static_cast<unsigned char>(0x80 | opcode), static_cast<unsigned char>(size), 0, 0};
    size_t n = 2;
    if (size >= 126) { header[1] = 126; header[2] = size >> 8; header[3] = size & 255; n = 4; }
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
// `/v1/proxy/{url}` per Satori's resource route: external links must match an advertised
// proxy_urls prefix (none here, so they are 403), while `internal:{platform}/{user}/{path}`
// links are served by the owning login. The only internal route we own is `_tmp`, the target
// of the built-in upload.create. This route deliberately needs no Authorization header so a
// plain <img src> can use it.
void Proxy(Client &c, Hub *hub, const char *url) {
    if (!strncmp(url, "internal:", 9)) {
        const char *platform = url + 9, *slash = strchr(platform, '/');
        if (!slash || slash == platform) { RawJson(c, 400, "Bad Request", "{\"error\":\"invalid_internal_url\"}"); return; }
        const char *user = slash + 1, *slash2 = strchr(user, '/');
        if (!slash2 || slash2 == user || !slash2[1]) { RawJson(c, 400, "Bad Request", "{\"error\":\"invalid_internal_url\"}"); return; }
        char platform_buf[64], user_buf[160];
        const size_t platform_size = slash - platform, user_size = slash2 - user;
        if (platform_size >= sizeof(platform_buf) || user_size >= sizeof(user_buf)) { RawJson(c, 400, "Bad Request", "{\"error\":\"invalid_internal_url\"}"); return; }
        memcpy(platform_buf, platform, platform_size); platform_buf[platform_size] = 0;
        memcpy(user_buf, user, user_size); user_buf[user_size] = 0;
        if (!FindLogin(hub, platform_buf, user_buf)) { RawJson(c, 404, "Not Found", "{\"error\":\"login_not_found\"}"); return; }
        const char *path = slash2 + 1;
        if (!strncmp(path, "_tmp/", 5)) {
            const TempFile *file = TempStoreGet(path + 5);
            if (!file) { RawJson(c, 404, "Not Found", "{\"error\":\"not_found\"}"); return; }
            char buffer[kMessage + 1];
            const int fd = open(file->path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd < 0) { RawJson(c, 404, "Not Found", "{\"error\":\"not_found\"}"); return; }
            ssize_t got = read(fd, buffer, kMessage);
            close(fd);
            if (got < 0) { RawJson(c, 500, "Internal Server Error", "{}"); return; }
            Raw(c, 200, "OK", file->content_type, buffer, static_cast<size_t>(got), true);
            return;
        }
        RawJson(c, 404, "Not Found", "{\"error\":\"unknown_internal_route\"}");
        return;
    }
    const bool http = !strncmp(url, "http://", 7) || !strncmp(url, "https://", 8);
    const char *host = url + (http ? (url[4] == 's' ? 8 : 7) : 0);
    if (!http || !*host || *host == '/' || *host == '?' || *host == '#') {
        RawJson(c, 400, "Bad Request", "{\"error\":\"invalid_url\"}"); return;
    }
    const cJSON *urls = cJSON_GetObjectItemCaseSensitive(Meta(hub), "proxy_urls");
    for (const cJSON *p = urls ? urls->child : nullptr; p; p = p->next)
        if (cJSON_IsString(p) && *p->valuestring && !strncmp(url, p->valuestring, strlen(p->valuestring))) {
            // Advertised prefixes are never registered: this build has no outbound HTTP client.
            RawJson(c, 501, "Not Implemented", "{\"error\":\"proxy_not_implemented\"}"); return;
        }
    RawJson(c, 403, "Forbidden", "{\"error\":\"forbidden\"}");
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
        Reply(c, 400, "Bad Request", "{\"error\":\"invalid_upload\"}"); return;
    }
    if (!TempStoreAvailable()) { Reply(c, 501, "Not Implemented", "{\"error\":\"upload_unavailable\"}"); return; }
    cJSON *result = cJSON_CreateObject();
    if (!result) { Reply(c, 500, "Internal Server Error", "{}"); return; }
    for (size_t i = 0; i < uploads->count; ++i) {
        const Part &part = uploads->parts[i];
        char name[160];
        if (!TempStorePut(part.filename, part.content_type, part.data, part.size, name, sizeof(name))) {
            cJSON_Delete(result); Reply(c, 500, "Internal Server Error", "{\"error\":\"upload_failed\"}"); return;
        }
        char url[512];
        snprintf(url, sizeof(url), "internal:%s/%s/_tmp/%s", platform, user, name);
        if (!cJSON_AddStringToObject(result, part.name, url)) {
            cJSON_Delete(result); Reply(c, 500, "Internal Server Error", "{}"); return;
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
        } else if (sn && !CanResume(hub, static_cast<uint64_t>(sn->valuedouble))) {
            Close(c, 4009);
        } else {
            c.identified = true; c.deadline = Now() + kHeartbeatMs;
            c.replay_until = Latest(hub);
            c.cursor = sn ? static_cast<uint64_t>(sn->valuedouble) : Latest(hub);
            char *ready = Envelope(4, Meta(hub));
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
        const size_t consumed = header + 4 + size;
        memmove(c.input, c.input + consumed, c.used - consumed); c.used -= consumed;
    }
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
    c.input[c.used] = 0;
    const char *end = static_cast<const char *>(memmem(c.input, c.used, "\r\n\r\n", 4));
    if (!end) {
        if (c.used >= kHeader) Reply(c, 431, "Request Header Fields Too Large", "{\"error\":\"headers_too_large\"}");
        return;
    }
    const size_t length = end - c.input + 4;
    if (length > kHeader) { Reply(c, 431, "Request Header Fields Too Large", "{}"); return; }
    char scratch[kHeader + 1]; memcpy(scratch, c.input, length); scratch[length] = 0;
    auto bad = [&]() { Reply(c, 400, "Bad Request", "{\"error\":\"invalid_request\"}"); };
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
    if (*header("Expect")) { Reply(c, 417, "Expectation Failed", "{}"); return; }
    size_t body_size = 0;
    for (const char *p = header("Content-Length"); *p; ++p) {
        if (*p < '0' || *p > '9') { bad(); return; }
        body_size = body_size * 10 + (*p - '0');
        if (body_size > kMessage) { Reply(c, 413, "Content Too Large", "{}"); return; }
    }
    if (c.used < length + body_size) return;
    if (!strcmp(path, "/v1/events")) {
        if (strcmp(method, "GET")) { Reply(c, 405, "Method Not Allowed", "{}", "GET"); return; }
        if (body_size || strcasecmp(header("Upgrade"), "websocket") ||
            !HeaderToken(header("Connection"), "upgrade") || strcmp(header("Sec-WebSocket-Version"), "13") ||
            !WebSocketKey(header("Sec-WebSocket-Key"))) { bad(); return; }
        char accept[29], response[256]; WebSocketAccept(header("Sec-WebSocket-Key"), accept);
        const int n = snprintf(response, sizeof(response), "HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\n"
                               "Connection: Upgrade\r\nSec-WebSocket-Accept: %s\r\n\r\n", accept);
        Queue(c, response, n); c.ws = true; c.deadline = Now() + kRequestMs;
        memmove(c.input, c.input + length, c.used - length); c.used -= length;
        return;
    }
    if (!strncmp(path, "/v1/proxy/", 10)) {
        if (strcmp(method, "GET")) { Reply(c, 405, "Method Not Allowed", "{}", "GET"); return; }
        if (body_size) { Reply(c, 400, "Bad Request", "{\"error\":\"invalid_request\"}"); return; }
        Proxy(c, hub, path + 10);
        return;
    }
    const char *auth = header("Authorization");
    if (!*auth) { Reply(c, 401, "Unauthorized", "{\"error\":\"missing_token\"}"); return; }
    if (strncasecmp(auth, "Bearer ", 7) || !EqualToken(config.token, auth + 7)) {
        Reply(c, 403, "Forbidden", "{\"error\":\"invalid_token\"}"); return;
    }
    const bool meta = !strcmp(path, "/v1/meta");
    const bool status = !strcmp(path, "/v1/internal/status");
    const bool capabilities = !strcmp(path, "/v1/internal/capabilities");
    const bool webhook_create = !strcmp(path, "/v1/meta/webhook.create");
    const bool webhook_delete = !strcmp(path, "/v1/meta/webhook.delete");
    const bool wakelock = !strcmp(path, "/v1/internal/wakelock");
    const Method *rpc = !strncmp(path, "/v1/", 4) ? FindMethod(path + 4) : nullptr;
    if (!meta && !status && !capabilities && !webhook_create && !webhook_delete && !wakelock && !rpc) { Reply(c, 404, "Not Found", "{\"error\":\"unknown_api\"}"); return; }
    if (strcmp(method, "POST")) { Reply(c, 405, "Method Not Allowed", "{}"); return; }
    cJSON *body = nullptr;
    Multipart uploads{};
    const char *type = header("Content-Type");
    if (rpc && rpc->upload) {
        if (strncasecmp(type, "multipart/form-data;", 20) || !strstr(type, "boundary=")) {
            Reply(c, 415, "Unsupported Media Type", "{}"); return;
        }
        if (!ParseMultipart(type, c.input + length, body_size, &uploads)) { bad(); return; }
    } else {
        if (body_size && (strncasecmp(type, "application/json", 16) || (type[16] && type[16] != ';'))) {
            Reply(c, 415, "Unsupported Media Type", "{}"); return;
        }
        body = body_size ? Json(c.input + length, body_size) : cJSON_CreateObject();
        if (!body || (rpc && !ValidateParams(*rpc, body))) { cJSON_Delete(body); bad(); return; }
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
            Reply(c, 500, "Internal Server Error", "{}");
        }
    } else if (capabilities) {
        cJSON *result = cJSON_CreateObject(), *methods = cJSON_CreateArray();
        if (result && methods && cJSON_AddItemToObject(result, "standard_methods", methods)) {
            for (const auto *m = kMethods; m != kMethods + kMethodCount; ++m) cJSON_AddItemToArray(methods, cJSON_CreateString(m->name));
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
            cJSON_Delete(result); cJSON_Delete(methods); Reply(c, 500, "Internal Server Error", "{}");
        }
    } else if (wakelock) {
        const cJSON *on = cJSON_GetObjectItemCaseSensitive(body, "on");
        const cJSON *toggle = cJSON_GetObjectItemCaseSensitive(body, "toggle");
        if (!g_wakelock_provider) {
            Reply(c, 501, "Not Implemented", "{\"error\":\"backend_not_implemented\"}");
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
                Reply(c, 500, "Internal Server Error", "{}");
            }
        } else {
            Reply(c, 400, "Bad Request", "{\"error\":\"invalid_request\"}");
        }
    } else if (webhook_create || webhook_delete) {
        const cJSON *url = cJSON_GetObjectItemCaseSensitive(body, "url");
        const cJSON *token = cJSON_GetObjectItemCaseSensitive(body, "token");
        bool ok = cJSON_IsString(url) && *url->valuestring && (!token || cJSON_IsString(token));
        if (ok) {
            if (webhook_create) ok = AddWebHook(hooks, url->valuestring, token ? token->valuestring : "");
            else ok = RemoveWebHook(hooks, url->valuestring);
        }
        Reply(c, ok ? 200 : 400, ok ? "OK" : "Bad Request", ok ? "{}" : "{\"error\":\"invalid_webhook\"}");
    } else if (!*header("Satori-Platform") || !*header("Satori-User-ID")) {
        Reply(c, 400, "Bad Request", "{\"error\":\"missing_login_headers\"}");
    } else {
        const cJSON *login = FindLogin(hub, header("Satori-Platform"), header("Satori-User-ID"));
        if (!login) Reply(c, 403, "Forbidden", "{\"error\":\"login_not_found\"}");
        else if (!strcmp(rpc->name, "login.get")) {
            char *text = cJSON_PrintUnformatted(login);
            Reply(c, text ? 200 : 500, text ? "OK" : "Internal Server Error", text ? text : "{}"); free(text);
        } else if (cJSON_GetObjectItemCaseSensitive(login, "status")->valuedouble != 1) {
            Reply(c, 503, "Service Unavailable", "{\"error\":\"login_offline\"}");
        } else {
            bool supported = false;
            const cJSON *features = cJSON_GetObjectItemCaseSensitive(login, "features");
            for (const cJSON *f = features ? features->child : nullptr; f; f = f->next)
                if (cJSON_IsString(f) && !strcmp(f->valuestring, rpc->name)) supported = true;
            if (!supported) Reply(c, 404, "Not Found", "{\"error\":\"unsupported_api\"}");
            else if (rpc->upload) Upload(c, header("Satori-Platform"), header("Satori-User-ID"), &uploads);
            else if (!backend || !backend->call) Reply(c, 501, "Not Implemented", "{\"error\":\"backend_not_implemented\"}");
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
void SetTempDir(const char *dir) { TempStoreSetDir(dir); }

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
            const char *value = line + 5;
            if (!strcmp(value, "on")) parsed.send = true;
            else if (!strcmp(value, "off")) parsed.send = false;
            else return false;
            send_seen = true;
        } else if (!strncmp(line, "send_allow=", 11) && !allow_seen) {
            // Retired whitelist. The key is still accepted (and its value ignored) so configs
            // written before send_allow was removed keep starting the server; send=on now
            // allows every talker.
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
    for (int i = 0; i < kClients; ++i) clients[i].fd = -1;
    for (;;) {
        // Drain a bounded native producer queue before polling clients.
        DrainWake(bus);
        char event[kEventSize]; bool meta;
        for (int budget = 0; budget < 32 && Take(bus, event, &meta); ++budget) {
            char *signal = Apply(hub, event, meta);
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
                    if (strlen(event) + 4 > sizeof(c.output) - (c.pending - c.sent)) break;
                    Frame(c, 1, event, strlen(event)); c.cursor = next;
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
            fds[i + 1] = {c.fd, static_cast<short>((c.closing ? 0 : POLLIN) | (c.pending > c.sent ? POLLOUT : 0)), 0};
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
                memset(slot, 0, sizeof(*slot)); slot->fd = fd; slot->deadline = Now() + kRequestMs;
            }
        }
        for (int i = 0; i < kClients; ++i) {
            Client &c = clients[i]; const short events = fds[i + 1].revents;
            if (c.fd < 0 || fds[i + 1].fd != c.fd) continue;
            if (events & (POLLERR | POLLNVAL)) { Drop(c); continue; }
            if (!c.closing && (events & POLLIN)) {
                const ssize_t n = recv(c.fd, c.input + c.used, kInput - c.used, 0);
                if (n > 0) {
                    c.used += n;
                    if (c.ws) WebSocket(c, config, hub); else Http(c, config, hub, backend, hooks);
                    if (c.used == kInput && !c.closing) { if (c.ws) Close(c, 1009); else Reply(c, 413, "Content Too Large", "{}"); }
                } else if (!n || (errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) { Drop(c); continue; }
            }
            if (c.fd >= 0 && c.pending > c.sent && (events & POLLOUT)) {
                const ssize_t n = send(c.fd, c.output + c.sent, c.pending - c.sent, MSG_NOSIGNAL);
                if (n > 0) c.sent += n;
                else if (n < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR) { Drop(c); continue; }
                if (c.sent == c.pending) { c.sent = c.pending = 0; if (c.closing) Drop(c); }
            }
            if (c.fd >= 0 && (events & POLLHUP) && !(events & POLLIN)) Drop(c);
        }
    }
    for (int i = 0; i < kClients; ++i) Drop(clients[i]);
    free(clients); DestroyHub(hub); DestroyWebHooks(hooks); close(listener);
}
} // namespace satori
