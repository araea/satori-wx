#include "tempstore.h"
#include <errno.h>
#include <dirent.h>
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
    char original[256];     // the client's file name, as sent
    size_t size;
    int64_t expires;
    bool used;
    bool writing;           // reserved by a TempWriter that has not finished yet
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

// Files a previous process (or a crash) left behind are in nobody's table, so nothing would ever
// delete them. Every ten minutes, and once at the first use, sweep the directory: anything older
// than a quarter of an hour cannot be a live upload (they live five minutes), and a staged send
// copy (see SendVideo) is given six hours.
int64_t g_last_sweep = 0;
void SweepOld(const char *dir, time_t max_age_s) {
    DIR *handle = opendir(dir);
    if (!handle) return;
    const time_t cutoff = time(nullptr) - max_age_s;
    while (const dirent *entry = readdir(handle)) {
        if (entry->d_name[0] == '.') continue;
        char path[1400];
        if (snprintf(path, sizeof(path), "%s/%s", dir, entry->d_name) >= static_cast<int>(sizeof(path))) continue;
        struct stat info {};
        if (lstat(path, &info) || !S_ISREG(info.st_mode)) continue;
        if (info.st_mtime < cutoff) unlink(path);
    }
    closedir(handle);
}
void SweepIfDue(int64_t now) {
    if (g_last_sweep && now - g_last_sweep < 10 * 60 * 1000) return;
    g_last_sweep = now ? now : 1;
    SweepOld(g_dir, 15 * 60);
    char staged[1100];
    if (snprintf(staged, sizeof(staged), "%s/send", g_dir) < static_cast<int>(sizeof(staged))) SweepOld(staged, 6 * 3600);
}

void Purge() {
    const int64_t now = NowMs();
    SweepIfDue(now);
    for (auto &entry : g_entries) {
        if (!entry.used || entry.writing || entry.expires > now) continue;
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

struct TempWriter {
    Entry *slot;
    int fd;
    size_t size;
};

namespace {
TempWriter g_writers[kMaxFiles];
// Removes control characters (and anything that could break a header or a path) but keeps UTF-8.
void CleanOriginal(const char *filename, char *out, size_t capacity) {
    size_t used = 0;
    const char *base = filename ? filename : "";
    if (const char *slash = strrchr(base, '/')) base = slash + 1;
    if (const char *slash = strrchr(base, '\\')) base = slash + 1;
    for (const char *p = base; *p && used + 1 < capacity; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if (c < 0x20 || c == 0x7f || c == '"') continue;
        out[used++] = static_cast<char>(c);
    }
    out[used] = 0;
}
} // namespace

TempWriter *TempStoreBegin(const char *filename, const char *content_type) {
    if (!EnsureDir()) return nullptr;
    Purge();
    Entry *slot = nullptr;
    for (auto &entry : g_entries) if (!entry.used) { slot = &entry; break; }
    if (!slot) return nullptr;  // The purge should have made room; never evict live data.
    TempWriter *writer = nullptr;
    for (auto &candidate : g_writers) if (!candidate.slot) { writer = &candidate; break; }
    if (!writer) return nullptr;
    char safe[80], random[33];
    Sanitize(filename, safe, sizeof(safe));
    RandomHex(random, 8);
    char name[128];
    snprintf(name, sizeof(name), "%s-%s", random, safe);
    if (!SafeName(name)) return nullptr;
    char path[1200];
    if (snprintf(path, sizeof(path), "%s/%s", g_dir, name) >= static_cast<int>(sizeof(path))) return nullptr;
    const int fd = open(path, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);
    if (fd < 0) return nullptr;
    *slot = {};
    strcpy(slot->name, name);
    strcpy(slot->path, path);
    const char *type = content_type && *content_type ? content_type : "application/octet-stream";
    strncpy(slot->content_type, type, sizeof(slot->content_type) - 1);
    CleanOriginal(filename, slot->original, sizeof(slot->original));
    slot->used = true;
    slot->writing = true;
    slot->expires = NowMs() + kTtlMs;
    writer->slot = slot;
    writer->fd = fd;
    writer->size = 0;
    return writer;
}

bool TempStoreWrite(TempWriter *writer, const char *data, size_t size) {
    if (!writer || !writer->slot) return false;
    size_t written = 0;
    while (written < size) {
        const ssize_t n = write(writer->fd, data + written, size - written);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        written += static_cast<size_t>(n);
    }
    writer->size += size;
    return true;
}

static void Release(TempWriter *writer) {
    if (writer->fd >= 0) close(writer->fd);
    writer->slot = nullptr;
    writer->fd = -1;
    writer->size = 0;
}

bool TempStoreFinish(TempWriter *writer, char *out, size_t capacity) {
    if (!writer || !writer->slot || !out || !capacity) return false;
    Entry *slot = writer->slot;
    const int fd = writer->fd;
    writer->fd = -1;  // closed here so a failure can report it
    if (close(fd)) { unlink(slot->path); *slot = {}; Release(writer); return false; }
    slot->size = writer->size;
    slot->expires = NowMs() + kTtlMs;
    slot->writing = false;
    snprintf(out, capacity, "%s", slot->name);
    Release(writer);
    return true;
}

void TempStoreAbort(TempWriter *writer) {
    if (!writer || !writer->slot) return;
    unlink(writer->slot->path);
    *writer->slot = {};
    Release(writer);
}

bool TempStoreOriginalName(const char *name, char *out, size_t capacity) {
    if (!name || !out || !capacity || !SafeName(name)) return false;
    for (auto &entry : g_entries) {
        if (!entry.used || entry.writing || strcmp(entry.name, name)) continue;
        snprintf(out, capacity, "%s", entry.original);
        return true;
    }
    return false;
}

bool TempStorePut(const char *filename, const char *content_type, const char *data, size_t size,
                  char *out, size_t capacity) {
    if (!data || !out || !capacity) return false;
    TempWriter *writer = TempStoreBegin(filename, content_type);
    if (!writer) return false;
    if (!TempStoreWrite(writer, data, size)) { TempStoreAbort(writer); return false; }
    return TempStoreFinish(writer, out, capacity);
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
