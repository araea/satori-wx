#include "tempstore.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace satori {
namespace {
constexpr size_t kMaxFiles = 64;
constexpr int64_t kTtlMs = 5 * 60 * 1000;  // Satori recommends 5 minutes for _tmp uploads.

struct Entry {
    char name[128];
    char path[1200];
    char content_type[128];
    size_t size;
    int64_t expires;
    bool used;
};
Entry g_entries[kMaxFiles];
char g_dir[1024];
bool g_dir_set = false;

int64_t NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return int64_t(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// True when the directory exists (creating it once if needed). Keeps the store fail-closed
// rather than writing anywhere.
bool EnsureDir() {
    if (!g_dir_set) {
        const char *explicit_dir = getenv("SATORI_TMPDIR");
        if (explicit_dir && *explicit_dir && strlen(explicit_dir) < sizeof(g_dir)) {
            strcpy(g_dir, explicit_dir);
        } else {
            const char *base = getenv("TMPDIR");
            if (!base || !*base) base = "/data/local/tmp";
            snprintf(g_dir, sizeof(g_dir), "%s/satori-wx-tmp", base);
        }
        g_dir_set = true;
    }
    if (mkdir(g_dir, 0700) && errno != EEXIST) return false;
    struct stat info {};
    return !stat(g_dir, &info) && S_ISDIR(info.st_mode);
}

void Purge() {
    const int64_t now = NowMs();
    for (auto &entry : g_entries) {
        if (!entry.used || entry.expires > now) continue;
        unlink(entry.path);
        entry = {};
    }
}

// Keeps [A-Za-z0-9._-] and strips leading dots so a stored name can never escape the
// directory or resolve to a hidden/metadata path.
void Sanitize(const char *filename, char *out, size_t capacity) {
    size_t used = 0;
    for (const char *p = filename ? filename : ""; *p && used + 1 < capacity; ++p) {
        const char c = *p;
        const bool safe = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                          (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (safe) {
            if (!used && c == '.') continue;
            out[used++] = c;
        }
    }
    if (!used) snprintf(out, capacity, "file");
    else out[used] = 0;
}

void RandomHex(char *out, size_t bytes) {
    unsigned char buffer[16] = {};
    if (bytes > sizeof(buffer)) bytes = sizeof(buffer);
    const int fd = open("/dev/urandom", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) {
        size_t got = 0;
        while (got < bytes) {
            const ssize_t n = read(fd, buffer + got, bytes - got);
            if (n <= 0) break;
            got += static_cast<size_t>(n);
        }
        close(fd);
    }
    static unsigned counter = 0;
    uint64_t fallback = static_cast<uint64_t>(time(nullptr)) ^ (static_cast<uint64_t>(++counter) << 32);
    for (size_t i = 0; i < bytes; ++i) {
        if (!buffer[i]) {
            buffer[i] = static_cast<unsigned char>(fallback >> ((i % 8) * 8));
            if (!buffer[i]) buffer[i] = static_cast<unsigned char>(1 + i);
        }
        snprintf(out + i * 2, 3, "%02x", buffer[i]);
    }
}

bool SafeName(const char *name) {
    const size_t size = strlen(name);
    if (!size || size >= sizeof(g_entries[0].name)) return false;
    if (strstr(name, "..")) return false;
    for (size_t i = 0; i < size; ++i) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
              c == '.' || c == '_' || c == '-')) return false;
    }
    return true;
}
} // namespace

void TempStoreSetDir(const char *dir) {
    if (!dir || !*dir || strlen(dir) >= sizeof(g_dir)) return;
    strcpy(g_dir, dir);
    g_dir_set = true;
}

bool TempStoreAvailable() { return EnsureDir(); }

bool TempStorePut(const char *filename, const char *content_type, const char *data, size_t size,
                  char *out, size_t capacity) {
    if (!data || !out || !capacity || !EnsureDir()) return false;
    Purge();
    Entry *slot = nullptr;
    for (auto &entry : g_entries) if (!entry.used) { slot = &entry; break; }
    if (!slot) return false;  // The purge should have made room; never evict live data.
    char safe[80], random[33];
    Sanitize(filename, safe, sizeof(safe));
    RandomHex(random, 8);
    char name[128];
    snprintf(name, sizeof(name), "%s-%s", random, safe);
    if (!SafeName(name)) return false;
    char path[1200];
    if (snprintf(path, sizeof(path), "%s/%s", g_dir, name) >= static_cast<int>(sizeof(path))) return false;
    const int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return false;
    size_t written = 0;
    while (written < size) {
        const ssize_t n = write(fd, data + written, size - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) { close(fd); unlink(path); return false; }
        written += static_cast<size_t>(n);
    }
    if (close(fd)) { unlink(path); return false; }
    strcpy(slot->name, name);
    strcpy(slot->path, path);
    const char *type = content_type && *content_type ? content_type : "application/octet-stream";
    strncpy(slot->content_type, type, sizeof(slot->content_type) - 1);
    slot->size = size;
    slot->expires = NowMs() + kTtlMs;
    slot->used = true;
    snprintf(out, capacity, "%s", name);
    return true;
}

const TempFile *TempStoreGet(const char *name) {
    if (!name || !*name || !g_dir_set || !SafeName(name)) return nullptr;
    Purge();
    const int64_t now = NowMs();
    for (auto &entry : g_entries) {
        if (!entry.used || strcmp(entry.name, name)) continue;
        if (entry.expires <= now) { unlink(entry.path); entry = {}; return nullptr; }
        struct stat info {};
        if (stat(entry.path, &info) || !S_ISREG(info.st_mode)) { entry = {}; return nullptr; }
        static thread_local TempFile file;
        file.path = entry.path;
        file.content_type = entry.content_type;
        file.size = entry.size;
        return &file;
    }
    return nullptr;
}

bool TempStoreResolveLink(const char *url, char *out, size_t capacity) {
    if (!url || !out || !capacity) return false;
    // An array (not a pointer) so sizeof() is the string length: this is the bug that made
    // the first image send answer media_unavailable for a link that was on disk all along.
    const char prefix[] = "internal:wechat/";
    const size_t prefix_size = sizeof(prefix) - 1;
    if (strncmp(url, prefix, prefix_size)) return false;
    const char *user = url + prefix_size;
    const char *slash = strchr(user, '/');
    if (!slash || slash == user) return false;
    if (strncmp(slash + 1, "_tmp/", 5)) return false;
    const TempFile *file = TempStoreGet(slash + 1 + 5);
    if (!file || strlen(file->path) >= capacity) return false;
    strcpy(out, file->path);
    return true;
}
} // namespace satori
