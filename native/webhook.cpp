#include "webhook.h"
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <poll.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

namespace satori {
namespace {
constexpr size_t kMaxHooks = 4;
constexpr size_t kQueueDepth = 64;
constexpr size_t kBodyMax = 4096;
constexpr int kConnectTimeoutMs = 3000;
constexpr int kIoTimeoutSeconds = 3;

struct Hook {
    char host[256];
    unsigned short port;
    char path[512];
    char token[129];
};
struct Entry {
    int opcode;
    size_t size;
    char body[kBodyMax];
};
} // namespace

// Public opaque type, matching the forward declaration in the header.
struct WebHooks {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    pthread_t thread;
    bool stopping;
    size_t count;
    Hook hooks[kMaxHooks];
    size_t head, used;
    Entry queue[kQueueDepth];
    unsigned long long sent, failed, dropped;
};

namespace {

// Absolute http:// URL only. https:// is rejected because there is no TLS client here.
bool ParseUrl(const char *url, Hook *hook) {
    if (!url || strncasecmp(url, "http://", 7)) return false;
    const char *authority = url + 7;
    const char *authority_end = authority;
    while (*authority_end && *authority_end != '/' && *authority_end != '?' && *authority_end != '#') ++authority_end;
    if (authority_end == authority) return false;
    for (const char *p = authority; p < authority_end; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c <= ' ' || c == 127 || c == '@') return false;
    }
    const char *host = authority, *host_end = authority_end, *port_text = nullptr;
    if (*authority == '[') {
        const char *close = static_cast<const char *>(memchr(authority, ']', static_cast<size_t>(authority_end - authority)));
        if (!close) return false;
        host = authority + 1;
        host_end = close;
        if (close + 1 < authority_end) {
            if (close[1] != ':') return false;
            port_text = close + 2;
        }
    } else {
        const char *colon = static_cast<const char *>(memchr(authority, ':', static_cast<size_t>(authority_end - authority)));
        if (colon) { host_end = colon; port_text = colon + 1; }
    }
    if (host_end == host || static_cast<size_t>(host_end - host) >= sizeof(hook->host)) return false;
    unsigned port = 80;
    if (port_text) {
        if (port_text >= authority_end) return false;
        port = 0;
        for (const char *p = port_text; p < authority_end; ++p) {
            if (*p < '0' || *p > '9') return false;
            port = port * 10 + static_cast<unsigned>(*p - '0');
            if (port > 65535) return false;
        }
        if (!port) return false;
    }
    if (!*authority_end) {
        strcpy(hook->path, "/");
    } else if (*authority_end == '/' || *authority_end == '?') {
        if (strlen(authority_end) >= sizeof(hook->path)) return false;
        if (*authority_end == '?') snprintf(hook->path, sizeof(hook->path), "/%s", authority_end);
        else strcpy(hook->path, authority_end);
    } else {
        return false; // fragment-only remainder is not a valid request target here
    }
    for (const char *p = hook->path; *p; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c <= ' ' || c == 127) return false;
    }
    memcpy(hook->host, host, static_cast<size_t>(host_end - host));
    hook->host[host_end - host] = 0;
    hook->port = static_cast<unsigned short>(port);
    return true;
}

int Connect(const addrinfo *address, int timeout_ms) {
    const int fd = socket(address->ai_family, address->ai_socktype | SOCK_NONBLOCK | SOCK_CLOEXEC, address->ai_protocol);
    if (fd < 0) return -1;
    if (connect(fd, address->ai_addr, address->ai_addrlen) && errno != EINPROGRESS) { close(fd); return -1; }
    pollfd poll_fd{fd, POLLOUT, 0};
    if (poll(&poll_fd, 1, timeout_ms) <= 0) { close(fd); return -1; }
    int error = 0;
    socklen_t size = sizeof(error);
    if (getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &size) || error) { close(fd); return -1; }
    const int flags = fcntl(fd, F_GETFL, 0);
    if (flags >= 0) fcntl(fd, F_SETFL, flags & ~O_NONBLOCK);
    return fd;
}

bool SendAll(int fd, const char *data, size_t size) {
    while (size) {
        const ssize_t n = send(fd, data, size, MSG_NOSIGNAL);
        if (n < 0) { if (errno == EINTR) continue; return false; }
        if (!n) return false;
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

bool Deliver(const Hook &hook, int opcode, const char *body, size_t size) {
    char service[8];
    snprintf(service, sizeof(service), "%u", static_cast<unsigned>(hook.port));
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    // Numeric addresses must not go through the platform resolver: on Android that
    // path can take seconds, which would serialise every queued delivery.
    hints.ai_flags = AI_NUMERICHOST;
    addrinfo *list = nullptr;
    if (getaddrinfo(hook.host, service, &hints, &list) || !list) {
        hints.ai_flags = 0;
        list = nullptr;
        if (getaddrinfo(hook.host, service, &hints, &list) || !list) return false;
    }
    int fd = -1;
    for (addrinfo *p = list; p; p = p->ai_next) {
        fd = Connect(p, kConnectTimeoutMs);
        if (fd >= 0) break;
    }
    freeaddrinfo(list);
    if (fd < 0) return false;
    const timeval timeout{kIoTimeoutSeconds, 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    char host_header[288];
    if (hook.port == 80) snprintf(host_header, sizeof(host_header), "%s", hook.host);
    else snprintf(host_header, sizeof(host_header), "%s:%u", hook.host, static_cast<unsigned>(hook.port));
    char authorization[256] = {};
    if (hook.token[0]) snprintf(authorization, sizeof(authorization), "Authorization: Bearer %s\r\n", hook.token);
    char header[1024];
    const int n = snprintf(header, sizeof(header),
        "POST %s HTTP/1.1\r\nHost: %s\r\nContent-Type: application/json\r\nContent-Length: %zu\r\n"
        "Satori-Opcode: %d\r\nConnection: close\r\n%s\r\n",
        hook.path, host_header, size, opcode, authorization);
    bool ok = n > 0 && static_cast<size_t>(n) < sizeof(header) && SendAll(fd, header, static_cast<size_t>(n)) &&
              SendAll(fd, body, size);
    char response[64];
    int status = 0;
    if (ok) {
        const ssize_t got = recv(fd, response, sizeof(response) - 1, 0);
        if (got > 0) {
            response[got] = 0;
            if (sscanf(response, "HTTP/1.%*d %d", &status) != 1) status = 0;
        }
        ok = status >= 200 && status < 300;
    }
    close(fd);
    return ok;
}

void *DeliverLoop(void *argument) {
    auto *hooks = static_cast<WebHooks *>(argument);
    pthread_setname_np(pthread_self(), "satori-wx-hook");
    for (;;) {
        pthread_mutex_lock(&hooks->mutex);
        while (!hooks->stopping && !hooks->used) pthread_cond_wait(&hooks->cond, &hooks->mutex);
        if (!hooks->used && hooks->stopping) { pthread_mutex_unlock(&hooks->mutex); break; }
        Hook targets[kMaxHooks];
        const size_t count = hooks->count;
        memcpy(targets, hooks->hooks, count * sizeof(Hook));
        Entry entry = hooks->queue[hooks->head];
        hooks->head = (hooks->head + 1) % kQueueDepth;
        --hooks->used;
        pthread_mutex_unlock(&hooks->mutex);
        for (size_t i = 0; i < count; ++i) {
            if (Deliver(targets[i], entry.opcode, entry.body, entry.size)) {
                pthread_mutex_lock(&hooks->mutex);
                ++hooks->sent;
                pthread_mutex_unlock(&hooks->mutex);
            } else {
                pthread_mutex_lock(&hooks->mutex);
                ++hooks->failed;
                pthread_mutex_unlock(&hooks->mutex);
            }
        }
    }
    return nullptr;
}
} // namespace

WebHooks *CreateWebHooks() {
    auto *hooks = static_cast<WebHooks *>(calloc(1, sizeof(WebHooks)));
    if (!hooks) return nullptr;
    if (pthread_mutex_init(&hooks->mutex, nullptr) || pthread_cond_init(&hooks->cond, nullptr)) {
        pthread_mutex_destroy(&hooks->mutex);
        free(hooks);
        return nullptr;
    }
    if (pthread_create(&hooks->thread, nullptr, DeliverLoop, hooks)) {
        pthread_cond_destroy(&hooks->cond);
        pthread_mutex_destroy(&hooks->mutex);
        free(hooks);
        return nullptr;
    }
    return hooks;
}

void DestroyWebHooks(WebHooks *hooks) {
    if (!hooks) return;
    pthread_mutex_lock(&hooks->mutex);
    hooks->stopping = true;
    pthread_cond_broadcast(&hooks->cond);
    pthread_mutex_unlock(&hooks->mutex);
    pthread_join(hooks->thread, nullptr);
    pthread_cond_destroy(&hooks->cond);
    pthread_mutex_destroy(&hooks->mutex);
    free(hooks);
}

bool AddWebHook(WebHooks *hooks, const char *url, const char *token) {
    if (!hooks || !url || !*url) return false;
    Hook parsed{};
    if (!ParseUrl(url, &parsed)) return false;
    if (token) {
        if (strlen(token) >= sizeof(parsed.token)) return false;
        strcpy(parsed.token, token);
    }
    pthread_mutex_lock(&hooks->mutex);
    bool replaced = false;
    for (size_t i = 0; i < hooks->count && !replaced; ++i) {
        if (!strcmp(hooks->hooks[i].host, parsed.host) && hooks->hooks[i].port == parsed.port &&
            !strcmp(hooks->hooks[i].path, parsed.path)) {
            hooks->hooks[i] = parsed;
            replaced = true;
        }
    }
    if (!replaced && hooks->count < kMaxHooks) {
        hooks->hooks[hooks->count++] = parsed;
        replaced = true;
    }
    pthread_mutex_unlock(&hooks->mutex);
    return replaced;
}

bool RemoveWebHook(WebHooks *hooks, const char *url) {
    if (!hooks || !url) return false;
    Hook parsed{};
    if (!ParseUrl(url, &parsed)) return false;
    pthread_mutex_lock(&hooks->mutex);
    bool removed = false;
    for (size_t i = 0; i < hooks->count; ++i) {
        if (!strcmp(hooks->hooks[i].host, parsed.host) && hooks->hooks[i].port == parsed.port &&
            !strcmp(hooks->hooks[i].path, parsed.path)) {
            hooks->hooks[i] = hooks->hooks[hooks->count - 1];
            --hooks->count;
            removed = true;
            break;
        }
    }
    pthread_mutex_unlock(&hooks->mutex);
    return removed;
}

size_t WebHookCount(WebHooks *hooks) {
    if (!hooks) return 0;
    pthread_mutex_lock(&hooks->mutex);
    const size_t count = hooks->count;
    pthread_mutex_unlock(&hooks->mutex);
    return count;
}

void PushWebHook(WebHooks *hooks, int opcode, const char *body) {
    if (!hooks || !body) return;
    const size_t size = strlen(body);
    if (size >= kBodyMax) return;
    pthread_mutex_lock(&hooks->mutex);
    if (!hooks->count || hooks->used == kQueueDepth) {
        if (hooks->count) ++hooks->dropped;
        pthread_mutex_unlock(&hooks->mutex);
        return;
    }
    Entry &entry = hooks->queue[(hooks->head + hooks->used) % kQueueDepth];
    entry.opcode = opcode;
    entry.size = size;
    memcpy(entry.body, body, size);
    entry.body[size] = 0;
    ++hooks->used;
    pthread_cond_signal(&hooks->cond);
    pthread_mutex_unlock(&hooks->mutex);
}

void WebHookStats(WebHooks *hooks, WebHookCounters *counters) {
    if (!counters) return;
    *counters = WebHookCounters{};
    if (!hooks) return;
    pthread_mutex_lock(&hooks->mutex);
    counters->sent = hooks->sent;
    counters->failed = hooks->failed;
    counters->dropped = hooks->dropped;
    pthread_mutex_unlock(&hooks->mutex);
}
} // namespace satori
