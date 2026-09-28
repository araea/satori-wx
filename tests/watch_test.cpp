// Host-side tests for the database change watch: a write to EnMicroMsg.db / -wal wakes the wait
// at once, noise in the same directory does not, and the timeout still ends a quiet wait.
#include "wx_watch.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {
int failures = 0;
char g_dir[512];

void Check(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

long long Now() {
    timespec t{};
    clock_gettime(CLOCK_MONOTONIC, &t);
    return static_cast<long long>(t.tv_sec) * 1000 + t.tv_nsec / 1000000;
}

void Touch(const char *name, const char *text) {
    char path[640];
    snprintf(path, sizeof(path), "%s/%s", g_dir, name);
    const int fd = open(path, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (fd < 0) return;
    if (write(fd, text, strlen(text)) < 0) { /* the test then fails on the wake */ }
    close(fd);
}

struct Writer { const char *name; int delay_ms; };
void *WriteLater(void *argument) {
    auto *writer = static_cast<Writer *>(argument);
    const timespec delay{0, writer->delay_ms * 1000000L};
    nanosleep(&delay, nullptr);
    Touch(writer->name, "x");
    return nullptr;
}

// Waits up to `timeout` for a write of `name` made after `delay` ms; returns the wake result and
// how long the wait really lasted.
bool WaitFor(int fd, const char *name, int delay_ms, int timeout_ms, long long *elapsed) {
    Writer writer{name, delay_ms};
    pthread_t thread;
    pthread_create(&thread, nullptr, WriteLater, &writer);
    const long long start = Now();
    const bool woke = satori::WatchWait(fd, timeout_ms);
    *elapsed = Now() - start;
    pthread_join(thread, nullptr);
    return woke;
}
} // namespace

int main() {
    snprintf(g_dir, sizeof(g_dir), "%s/satori-wx-watch-%d", getenv("SATORI_WATCH_TMP") ? getenv("SATORI_WATCH_TMP") : "/tmp", static_cast<int>(getpid()));
    mkdir(g_dir, 0700);
    char database[640];
    snprintf(database, sizeof(database), "%s/EnMicroMsg.db", g_dir);
    Touch("EnMicroMsg.db", "db");
    Touch("EnMicroMsg.db-wal", "wal");

    Check(satori::WatchOpen(nullptr) < 0, "null path is refused");
    Check(satori::WatchOpen("EnMicroMsg.db") < 0, "a bare file name has no directory to watch");
    char missing[700];
    snprintf(missing, sizeof(missing), "%s/nope/EnMicroMsg.db", g_dir);
    Check(satori::WatchOpen(missing) < 0, "a missing directory is refused");

    const int fd = satori::WatchOpen(database);
    Check(fd >= 0, "watch opens on the database directory");
    if (fd >= 0) {
        long long elapsed = 0;
        bool woke = WaitFor(fd, "EnMicroMsg.db-wal", 60, 2000, &elapsed);
        Check(woke, "a WAL write wakes the wait");
        Check(elapsed >= 50 && elapsed < 500, "the WAL wake is prompt, not a timeout");

        woke = WaitFor(fd, "EnMicroMsg.db", 60, 2000, &elapsed);
        Check(woke, "a write to the main database wakes the wait");
        Check(elapsed < 500, "the database wake is prompt");

        woke = WaitFor(fd, "SnsMicroMsg.db", 60, 400, &elapsed);
        Check(!woke, "a write to another file in the directory does not wake it");
        Check(elapsed >= 380, "an unrelated write does not cut the wait short");

        woke = WaitFor(fd, "EnMicroMsg.db-shm", 60, 300, &elapsed);
        Check(!woke, "the -shm file is not a change notice");

        // A burst of writes is one wake, and the queue is empty afterwards.
        Touch("EnMicroMsg.db-wal", "a"); Touch("EnMicroMsg.db-wal", "b"); Touch("EnMicroMsg.db-wal", "c");
        Check(satori::WatchWait(fd, 500), "a burst of writes wakes the wait");
        const long long start = Now();
        Check(!satori::WatchWait(fd, 150), "the burst left nothing queued");
        Check(Now() - start >= 140, "an empty queue waits out the timeout");

        // The -wal file may be deleted and recreated: a directory watch survives that.
        char wal[640];
        snprintf(wal, sizeof(wal), "%s/EnMicroMsg.db-wal", g_dir);
        unlink(wal);
        satori::WatchWait(fd, 20);
        woke = WaitFor(fd, "EnMicroMsg.db-wal", 60, 2000, &elapsed);
        Check(woke, "a recreated WAL file is still watched");
        close(fd);
    }

    long long elapsed = 0;
    const long long start = Now();
    Check(!satori::WatchWait(-1, 120), "without a watch the wait just sleeps");
    elapsed = Now() - start;
    Check(elapsed >= 110 && elapsed < 400, "a blind wait lasts about the timeout");
    Check(!satori::WatchWait(-1, -5), "a negative timeout is a zero wait");

    char command[600];
    snprintf(command, sizeof(command), "rm -rf '%s'", g_dir);
    if (system(command) != 0) { /* leftover temp dir is harmless */ }
    if (failures) { fprintf(stderr, "%d watch check(s) failed\n", failures); return 1; }
    printf("watch tests: PASS\n");
    return 0;
}
