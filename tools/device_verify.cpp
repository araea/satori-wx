// Read-only live-device acceptance client. No test backend, event injection or message sending.
#include "server.h"
#include "protocol.h"
#include "version.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace {
bool Send(int fd, const void *buffer, size_t size) {
    auto *p = static_cast<const char *>(buffer);
    while (size) {
        const ssize_t n = send(fd, p, size, MSG_NOSIGNAL);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= n;
    }
    return true;
}
bool Read(int fd, void *buffer, size_t size) {
    auto *p = static_cast<char *>(buffer);
    while (size) {
        const ssize_t n = recv(fd, p, size, 0);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        p += n; size -= n;
    }
    return true;
}
int Connect(unsigned port) {
    const int fd = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    timeval timeout{5, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(port);
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (connect(fd, reinterpret_cast<sockaddr *>(&address), sizeof(address))) { close(fd); return -1; }
    return fd;
}
bool Header(int fd, char out[8192]) {
    size_t n = 0;
    while (n < 8191) {
        if (!Read(fd, out + n, 1)) return false;
        ++n; out[n] = 0;
        if (n >= 4 && !memcmp(out + n - 4, "\r\n\r\n", 4)) return true;
    }
    return false;
}
cJSON *Http(const satori::Config &config, const char *path, int expected, bool auth = true) {
    const int fd = Connect(config.port); if (fd < 0) return nullptr;
    char request[1024];
    const int n = snprintf(request, sizeof(request), "POST %s HTTP/1.1\r\nHost: localhost\r\n"
                           "Content-Type: application/json\r\nContent-Length: 2\r\n%s%s%s\r\n{}",
                           path, auth ? "Authorization: Bearer " : "", auth ? config.token : "", auth ? "\r\n" : "");
    char header[8192]; int status = 0;
    if (!Send(fd, request, n) || !Header(fd, header) || sscanf(header, "HTTP/1.1 %d", &status) != 1 || status != expected) {
        close(fd); return nullptr;
    }
    const char *length = strstr(header, "\r\nContent-Length: ");
    const long size = length ? strtol(length + 18, nullptr, 10) : -1;
    char body[16385];
    if (size < 0 || size > 16384 || !Read(fd, body, size)) { close(fd); return nullptr; }
    close(fd); body[size] = 0;
    return satori::Json(body, size);
}
bool Frame(int fd, const char *payload) {
    const size_t size = strlen(payload); if (size > 1024) return false;
    unsigned char packet[1032]; size_t pos = 0;
    packet[pos++] = 0x81;
    if (size < 126) packet[pos++] = 0x80 | size;
    else { packet[pos++] = 0xfe; packet[pos++] = size >> 8; packet[pos++] = size & 255; }
    const unsigned char mask[4] = {0x51, 0x72, 0x93, 0xb4}; // Masking is protocol framing, not encryption.
    memcpy(packet + pos, mask, 4); pos += 4;
    for (size_t i = 0; i < size; ++i) packet[pos++] = payload[i] ^ mask[i % 4];
    return Send(fd, packet, pos);
}
cJSON *Receive(int fd) {
    unsigned char header[2]; if (!Read(fd, header, 2) || header[0] != 0x81 || (header[1] & 0x80)) return nullptr;
    size_t size = header[1];
    if (size == 126) { unsigned char ext[2]; if (!Read(fd, ext, 2)) return nullptr; size = unsigned(ext[0]) << 8 | ext[1]; }
    if (size > 16384) return nullptr;
    char body[16385]; if (!Read(fd, body, size)) return nullptr; body[size] = 0;
    return satori::Json(body, size);
}
bool Op(cJSON *root, int op) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(root, "op");
    return cJSON_IsNumber(value) && value->valuedouble == op;
}
bool Check(const satori::Config &config, bool soak, bool expect_login) {
    cJSON *unauthorized = Http(config, "/v1/meta", 401, false);
    if (!unauthorized) { puts("FAIL http_missing_auth"); return false; } cJSON_Delete(unauthorized);
    cJSON *status = Http(config, "/v1/internal/status", 200);
    const cJSON *version = cJSON_GetObjectItemCaseSensitive(status, "version");
    const cJSON *pid = cJSON_GetObjectItemCaseSensitive(status, "pid");
    if (!cJSON_IsString(version) || strcmp(version->valuestring, SATORI_WX_VERSION) || !cJSON_IsNumber(pid)) {
        cJSON_Delete(status); puts("FAIL live_version"); return false;
    }
    const int app_pid = pid->valueint;
    printf("PASS live_version=%s app_pid=%d\n", version->valuestring, app_pid); cJSON_Delete(status);
    cJSON *meta = Http(config, "/v1/meta", 200);
    const cJSON *logins = cJSON_GetObjectItemCaseSensitive(meta, "logins");
    if (!cJSON_IsArray(logins)) { cJSON_Delete(meta); puts("FAIL meta"); return false; }
    bool have_login = false;
    char login_id[96] = {};
    int login_status = -1;
    for (const cJSON *p = logins->child; p; p = p->next) {
        const cJSON *platform = cJSON_GetObjectItemCaseSensitive(p, "platform");
        const cJSON *user = cJSON_GetObjectItemCaseSensitive(p, "user");
        const cJSON *id = user ? cJSON_GetObjectItemCaseSensitive(user, "id") : nullptr;
        const cJSON *status = cJSON_GetObjectItemCaseSensitive(p, "status");
        if (cJSON_IsString(platform) && !strcmp(platform->valuestring, "wechat") && cJSON_IsString(id) && *id->valuestring) {
            have_login = true;
            snprintf(login_id, sizeof(login_id), "%s", id->valuestring);
            login_status = cJSON_IsNumber(status) ? status->valueint : -1;
            break;
        }
    }
    printf("PASS http_meta logins=%d\n", cJSON_GetArraySize(logins));
    if (have_login) printf("PASS account_identity user=%s status=%d\n", login_id, login_status);
    else if (expect_login) { cJSON_Delete(meta); puts("FAIL account_identity"); return false; }
    else puts("SKIP account_identity (no login in snapshot)");
    cJSON_Delete(meta);
    const int fd = Connect(config.port); if (fd < 0) return false;
    const char *upgrade = "GET /v1/events HTTP/1.1\r\nHost: localhost\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n"
                          "Sec-WebSocket-Version: 13\r\nSec-WebSocket-Key: dGhlIHNhbXBsZSBub25jZQ==\r\n\r\n";
    char header[8192];
    if (!Send(fd, upgrade, strlen(upgrade)) || !Header(fd, header) || strncmp(header, "HTTP/1.1 101 ", 13) ||
        !strstr(header, "Sec-WebSocket-Accept: s3pPLMBiTxaQ9kYGzzhZRbK+xOo=")) { close(fd); puts("FAIL upgrade"); return false; }
    char identify[256]; snprintf(identify, sizeof(identify), "{\"op\":3,\"body\":{\"token\":\"%s\"}}", config.token);
    if (!Frame(fd, identify)) { close(fd); return false; }
    cJSON *ready = Receive(fd);
    bool ok = Op(ready, 4) && cJSON_IsArray(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(ready, "body"), "logins"));
    cJSON_Delete(ready);
    for (int i = 0; ok && i < (soak ? 5 : 1); ++i) {
        if (soak && i) sleep(10);
        ok = Frame(fd, "{\"op\":1}");
        cJSON *pong = ok ? Receive(fd) : nullptr; ok = Op(pong, 2); cJSON_Delete(pong);
    }
    close(fd);
    if (!ok) { puts("FAIL ready_heartbeat"); return false; }
    status = Http(config, "/v1/internal/status", 200);
    pid = cJSON_GetObjectItemCaseSensitive(status, "pid");
    ok = cJSON_IsNumber(pid) && pid->valueint == app_pid; cJSON_Delete(status);
    if (!ok) { puts("FAIL process_changed"); return false; }
    printf("PASS websocket_ready_heartbeat stable_seconds=%d\n", soak ? 40 : 0);
    if (have_login) printf("VERDICT: PASS native_protocol_live + wechat_identity user=%s; message backend unavailable\n", login_id);
    else puts("VERDICT: PASS native_protocol_live; no wechat identity in snapshot");
    return true;
}
}
int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: %s config [--soak] [--expect-login]\n", argv[0]); return 2; }
    bool soak = false, expect_login = false;
    for (int i = 2; i < argc; ++i) {
        if (!strcmp(argv[i], "--soak")) soak = true;
        else if (!strcmp(argv[i], "--expect-login")) expect_login = true;
        else { fprintf(stderr, "unknown flag: %s\n", argv[i]); return 2; }
    }
    const int fd = open(argv[1], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    satori::Config config;
    const bool ok = fd >= 0 && satori::ReadConfig(fd, &config); if (fd >= 0) close(fd);
    if (!ok) { puts("FAIL config"); return 2; }
    return Check(config, soak, expect_login) ? 0 : 1;
}
