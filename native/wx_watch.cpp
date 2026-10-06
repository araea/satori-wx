#include "wx_watch.h"
#include <poll.h>
#include <stdio.h>
#include <string.h>
#include <sys/inotify.h>
#include <time.h>
#include <unistd.h>

namespace satori {
namespace {
long long MonotonicMs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}

// Empties the notice queue; true when it held a write to the database or its WAL (or overflowed,
// which may have swallowed one).
bool Drain(int fd) {
    bool changed = false;
    alignas(inotify_event) char buffer[4096];
    for (;;) {
        const ssize_t size = read(fd, buffer, sizeof(buffer));
        if (size <= 0) break;
        for (char *at = buffer; at + sizeof(inotify_event) <= buffer + size;) {
            const auto *event = reinterpret_cast<const inotify_event *>(at);
            if (event->mask & IN_Q_OVERFLOW)
                changed = true;
            else if (event->len && (!strcmp(event->name, "EnMicroMsg.db") || !strcmp(event->name, "EnMicroMsg.db-wal")))
                changed = true;
            at += sizeof(inotify_event) + event->len;
        }
    }
    return changed;
}
} // namespace

int WatchOpen(const char *database) {
    if (!database) return -1;
    char directory[1600];
    snprintf(directory, sizeof(directory), "%s", database);
    char *slash = strrchr(directory, '/');
    if (!slash) return -1;
    *slash = 0;
    const int fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    if (fd < 0) return -1;
    if (inotify_add_watch(fd, directory, IN_MODIFY | IN_CLOSE_WRITE | IN_CREATE | IN_MOVED_TO) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

bool WatchWait(int fd, long long timeout_ms) {
    if (timeout_ms < 0) timeout_ms = 0;
    if (fd < 0) {
        poll(nullptr, 0, static_cast<int>(timeout_ms));
        return false;
    }
    const long long deadline = MonotonicMs() + timeout_ms;
    for (;;) {
        const long long left = deadline - MonotonicMs();
        pollfd watch{fd, POLLIN, 0};
        const int ready = poll(&watch, 1, static_cast<int>(left > 0 ? left : 0));
        if (ready > 0 && (watch.revents & POLLIN) && Drain(fd)) return true;
        // Noise in the same directory (or an interrupted poll): keep waiting out the remainder.
        if (ready == 0 || MonotonicMs() >= deadline) return false;
    }
}
} // namespace satori
