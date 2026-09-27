#include "wx_live.h"
#include "wx_store.h"
#include "wx_account.h"
#include <dirent.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>

namespace satori {
namespace {
struct Spec {
    unsigned char key[64];
    int size;
    int page;
    int version;
};
struct Live {
    char app_data[1024];
    EventBus *bus;
    int login_sn;
    Store *store;
    long long watermark;
    long long emitted;
};

// Shared with the RPC backend; set once the store is opened.
Store *g_live_store = nullptr;
int g_live_login_sn = 0;

bool Emit(void *context, const char *event) {
    auto *live = static_cast<Live *>(context);
    ++live->emitted;
    return Publish(live->bus, event);
}

// File diagnostics: logcat is unreliable for an injected module tag, so status goes to
// <app>/files/satori-wx-store.log (same uid, overwritten each process start).
void Log(const char *app_data, const char *format, ...) {
    static bool started = false;
    char path[1200];
    snprintf(path, sizeof(path), "%s/files/satori-wx-store.log", app_data);
    FILE *file = fopen(path, started ? "a" : "w");
    started = true;
    if (!file) return;
    fprintf(file, "%ld ", static_cast<long>(time(nullptr)));
    va_list arguments;
    va_start(arguments, format);
    vfprintf(file, format, arguments);
    va_end(arguments);
    fputc('\n', file);
    fclose(file);
}

int HexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

bool ParseSpec(const char *line, Spec *spec) {
    int index = 0, size = 0, page = 0, version = 0;
    char hex[160] = {};
    if (sscanf(line, "spec=%d len=%d page=%d ver=%d hex=%159s", &index, &size, &page, &version, hex) != 5) return false;
    if (size <= 0 || size > static_cast<int>(sizeof(spec->key))) return false;
    if (static_cast<int>(strlen(hex)) != size * 2) return false;
    for (int i = 0; i < size; ++i) {
        const int high = HexDigit(hex[i * 2]), low = HexDigit(hex[i * 2 + 1]);
        if (high < 0 || low < 0) return false;
        spec->key[i] = static_cast<unsigned char>(high * 16 + low);
    }
    spec->size = size;
    spec->page = page;
    spec->version = version;
    return true;
}

bool FindDatabase(const char *app_data, char *out, size_t capacity) {
    char micro[1200];
    snprintf(micro, sizeof(micro), "%s/MicroMsg", app_data);
    DIR *dir = opendir(micro);
    if (!dir) return false;
    bool found = false;
    for (dirent *entry; (entry = readdir(dir)); ) {
        if (entry->d_name[0] == '.') continue;
        char path[1600];
        snprintf(path, sizeof(path), "%s/%s/EnMicroMsg.db", micro, entry->d_name);
        struct stat info{};
        if (!stat(path, &info) && S_ISREG(info.st_mode)) { snprintf(out, capacity, "%s", path); found = true; break; }
    }
    closedir(dir);
    return found;
}

// Returns the one spec that came from setCipherKey (it carries page_size + cipher version).
// Only that spec is ever used against the live database. The main module captures it itself
// (native/wx_key.cpp); there is no separate probe module any more.
bool LoadCipherSpec(const char *app_data, Spec *spec) {
    char path[1300];
    snprintf(path, sizeof(path), "%s/files/satori-wx/key.log", app_data);
    FILE *file = fopen(path, "r");
    if (!file) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), file)) {
        Spec candidate{};
        if (!ParseSpec(line, &candidate)) continue;
        if (candidate.version <= 0) continue;
        if (!found) { *spec = candidate; found = true; }
    }
    fclose(file);
    return found;
}

bool TryOpen(Live *live, Store **out) {
    Spec spec{};
    if (!LoadCipherSpec(live->app_data, &spec)) { Log(live->app_data, "open: no cipher spec yet"); return false; }
    char database[1600];
    if (!FindDatabase(live->app_data, database, sizeof(database))) { Log(live->app_data, "open: no database found"); return false; }
    Account account;
    const char *self_id = ReadAccount(live->app_data, &account) && account.exists ? account.wxid : nullptr;
    Store *store = CreateStore(database, spec.key, spec.size, spec.version, self_id);
    if (!store) { Log(live->app_data, "open: failed (%s) compat=%d len=%d", StoreError(nullptr), spec.version, spec.size); return false; }
    const long long watermark = StoreWatermark(store);
    if (watermark < 0) { Log(live->app_data, "open: watermark failed (%s)", StoreError(store)); DestroyStore(store); return false; }
    Log(live->app_data, "opened: compat=%d len=%d watermark=%lld", spec.version, spec.size, watermark);
    *out = store;
    return true;
}

void *Loop(void *argument) {
    auto *live = static_cast<Live *>(argument);
    pthread_setname_np(pthread_self(), "satori-wx-store");
    for (int i = 0; i < 90 && !live->store; ++i) {
        if (TryOpen(live, &live->store)) break;
        const timespec delay{2, 0};
        nanosleep(&delay, nullptr);
    }
    if (!live->store) { Log(live->app_data, "gave up: no store after retries"); return nullptr; }
    // Wait for the hub to hold an online login; otherwise Apply drops every event we publish.
    for (int i = 0; i < 150 && g_login_count <= 0; ++i) {
        const timespec second{1, 0};
        nanosleep(&second, nullptr);
    }
    Log(live->app_data, "login_count=%d", g_login_count);
    g_live_store = live->store;
    g_live_login_sn = live->login_sn;
    // Emit only new rows: history comes from message.list, which reads the database directly.
    live->watermark = StoreWatermark(live->store);
    Log(live->app_data, "polling from watermark=%lld", live->watermark);
    const timespec grace{5, 0};
    nanosleep(&grace, nullptr);
    long long logged = -1;
    for (;;) {
        live->watermark = StorePoll(live->store, live->watermark, live->login_sn, Emit, live);
        if (live->emitted != logged) {
            logged = live->emitted;
            Log(live->app_data, "emitted=%lld watermark=%lld", live->emitted, live->watermark);
        }
        const timespec delay{2, 0};
        nanosleep(&delay, nullptr);
    }
}
} // namespace

bool StartLiveStore(const char *app_data_dir, EventBus *bus, int login_sn) {
    if (!app_data_dir || !*app_data_dir || !bus) return false;
    auto *live = static_cast<Live *>(calloc(1, sizeof(Live)));
    if (!live) return false;
    snprintf(live->app_data, sizeof(live->app_data), "%s", app_data_dir);
    live->bus = bus;
    live->login_sn = login_sn;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, Loop, live)) { free(live); return false; }
    pthread_detach(thread);
    return true;
}

Store *LiveStore() { return g_live_store; }
int LiveLoginSn() { return g_live_login_sn; }
} // namespace satori
