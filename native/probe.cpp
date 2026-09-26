// Native-only JNI boundary research. No DEX, ArtMethod offsets or code patches.
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <android/log.h>

#include "zygisk.hpp"
#include "version.h"
#include "data_slot.h"
#include "wcdb.h"
#include <dirent.h>
#include <unistd.h>

namespace {
constexpr char kTarget[] = "com.tencent.mm";
constexpr int kCapacity = 1024;
constexpr long kCallLimit = 20000;
constexpr int64_t kWindowMs = 90000;
constexpr size_t kLogLimit = 16 * 1024 * 1024;
using RegisterFn = jint (*)(JNIEnv *, jclass, const JNINativeMethod *, jint);

struct Entry {
    jobject clazz;
    uintptr_t fn;
    int64_t time_ms;
    int tid;
    char name[96];
    char sig[224];
};
JavaVM *g_vm = nullptr;
wx::DataSlot<RegisterFn> g_patch;
RegisterFn *g_slot = nullptr;
char g_dir[1024];
char g_app_data[1024];
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;

// M3.1: capture WCDB cipher specs at the app's own RegisterNatives boundary.
// Only the fnPtrs of nativeSetKey/setCipherKey are swapped in a writable copy of the array
// the app passes; the app's array and code are never written. Wrappers forward unchanged.
constexpr size_t kKeyMax = 64;
constexpr int kSpecMax = 16;
constexpr jint kMethodCopyLimit = 256;
using SetKeyFn = void (*)(JNIEnv *, jobject, jlong, jbyteArray);
using SetCipherKeyFn = void (*)(JNIEnv *, jobject, jlong, jbyteArray, jint, jint);
struct CipherSpec {
    unsigned char key[kKeyMax];
    int key_size;
    int page_size;
    int version;
};
SetKeyFn g_original_set_key = nullptr;
SetCipherKeyFn g_original_cipher_key = nullptr;
pthread_mutex_t g_key_mu = PTHREAD_MUTEX_INITIALIZER;
CipherSpec g_specs[kSpecMax];
int g_spec_count = 0;
int g_verify_needed = 0;
Entry g_ring[kCapacity];
int g_head = 0, g_tail = 0, g_count = 0;
bool g_active = false;
long g_calls = 0, g_methods = 0, g_drops = 0;
int64_t g_started_ms = 0;

int64_t NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

// Caller owns g_mu. Queue capacity bounds JNI references as well as native storage.
void Record(JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint n) {
    g_methods += n;
    for (jint i = 0; i < n; ++i) {
        if (g_count == kCapacity) { g_drops += n - i; break; }
        jobject ref = env->NewGlobalRef(clazz);
        if (!ref || env->ExceptionCheck()) {
            // Entry had no pending exception; only clear an observer-created failure.
            env->ExceptionClear();
            if (ref) env->DeleteGlobalRef(ref);
            g_drops += n - i;
            break;
        }
        Entry &e = g_ring[g_head];
        e.clazz = ref;
        e.fn = reinterpret_cast<uintptr_t>(methods[i].fnPtr);
        e.time_ms = NowMs();
        e.tid = static_cast<int>(syscall(SYS_gettid));
        snprintf(e.name, sizeof(e.name), "%s", methods[i].name ? methods[i].name : "");
        snprintf(e.sig, sizeof(e.sig), "%s", methods[i].signature ? methods[i].signature : "");
        g_head = (g_head + 1) % kCapacity;
        ++g_count;
    }
}

int ReadKey(JNIEnv *env, jbyteArray array, unsigned char *out) {
    if (!array || env->ExceptionCheck()) return 0;
    const jsize size = env->GetArrayLength(array);
    if (size <= 0 || static_cast<size_t>(size) > kKeyMax) return 0;
    env->GetByteArrayRegion(array, 0, size, reinterpret_cast<jbyte *>(out));
    if (env->ExceptionCheck()) { env->ExceptionClear(); return 0; }
    return static_cast<int>(size);
}

void AddSpec(const unsigned char *key, int size, int page, int version) {
    if (!key || size <= 0) return;
    pthread_mutex_lock(&g_key_mu);
    bool duplicate = false;
    for (int i = 0; i < g_spec_count && !duplicate; ++i) {
        if (g_specs[i].key_size == size && !memcmp(g_specs[i].key, key, static_cast<size_t>(size))) {
            if (page > 0) g_specs[i].page_size = page;
            if (version > 0) g_specs[i].version = version;
            duplicate = true;
        }
    }
    if (!duplicate && g_spec_count < kSpecMax) {
        CipherSpec &spec = g_specs[g_spec_count++];
        memcpy(spec.key, key, static_cast<size_t>(size));
        spec.key_size = size;
        spec.page_size = page;
        spec.version = version;
    }
    pthread_mutex_unlock(&g_key_mu);
    __atomic_store_n(&g_verify_needed, 1, __ATOMIC_RELEASE);
}

void CaptureSetKey(JNIEnv *env, jobject thiz, jlong handle, jbyteArray key) {
    unsigned char buffer[kKeyMax];
    const int size = ReadKey(env, key, buffer);
    if (size > 0) AddSpec(buffer, size, 0, -1);
    if (g_original_set_key) g_original_set_key(env, thiz, handle, key);
}

void CaptureCipherKey(JNIEnv *env, jobject thiz, jlong handle, jbyteArray key, jint page_size, jint version) {
    unsigned char buffer[kKeyMax];
    const int size = ReadKey(env, key, buffer);
    if (size > 0) AddSpec(buffer, size, page_size, version);
    if (g_original_cipher_key) g_original_cipher_key(env, thiz, handle, key, page_size, version);
}

jint ObserveRegister(JNIEnv *env, jclass clazz, const JNINativeMethod *methods, jint n) {
    // Substitute before the host registers, using a local copy so the app's array stays read-only.
    JNINativeMethod copy[kMethodCopyLimit];
    const JNINativeMethod *argument = methods;
    if (methods && n > 0 && n <= kMethodCopyLimit) {
        memcpy(copy, methods, static_cast<size_t>(n) * sizeof(JNINativeMethod));
        for (jint i = 0; i < n; ++i) {
            if (!copy[i].name || !copy[i].signature) continue;
            if (!strcmp(copy[i].name, "nativeSetKey") && !strcmp(copy[i].signature, "(J[B)V")) {
                g_original_set_key = reinterpret_cast<SetKeyFn>(copy[i].fnPtr);
                copy[i].fnPtr = reinterpret_cast<void *>(CaptureSetKey);
            } else if (!strcmp(copy[i].name, "setCipherKey") && !strcmp(copy[i].signature, "(J[BII)V")) {
                g_original_cipher_key = reinterpret_cast<SetCipherKeyFn>(copy[i].fnPtr);
                copy[i].fnPtr = reinterpret_cast<void *>(CaptureCipherKey);
            }
        }
        argument = copy;
    }
    // The original result and pending exception belong to the host.
    const jint result = g_patch.original()(env, clazz, argument, n);
    if (result != JNI_OK || !clazz || !methods || n <= 0 || env->ExceptionCheck()) return result;
    pthread_mutex_lock(&g_mu);
    if (g_active) {
        ++g_calls;
        Record(env, clazz, methods, n);
        if (g_calls >= kCallLimit) g_active = false;
    }
    pthread_mutex_unlock(&g_mu);
    return result;
}

bool Pop(Entry *entry) {
    pthread_mutex_lock(&g_mu);
    const bool found = g_count != 0;
    if (found) {
        *entry = g_ring[g_tail];
        g_tail = (g_tail + 1) % kCapacity;
        --g_count;
    }
    pthread_mutex_unlock(&g_mu);
    return found;
}

const char *Stop() {
    pthread_mutex_lock(&g_mu);
    g_active = false; // No producer can add refs after this lock is released.
    const char *status = g_patch.restore();
    pthread_mutex_unlock(&g_mu);
    return status;
}

bool WriteAll(int fd, const char *data, size_t size) {
    while (size) {
        const ssize_t n = write(fd, data, size);
        if (n < 0 && errno == EINTR) continue;
        if (n <= 0) return false;
        data += n;
        size -= static_cast<size_t>(n);
    }
    return true;
}

// snprintf returns the *untruncated* size; callers must clamp before writing.
size_t Printed(int size, size_t capacity) {
    return size <= 0 ? 0 : (static_cast<size_t>(size) < capacity ? size : capacity - 1);
}

int OpenLog(int dir, const char *name) {
    return openat(dir, name, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK, 0600);
}

// Resolve current maps on the writer thread, not on RegisterNatives' call path.
// maps offset + address delta is a backing-file offset, not an ELF virtual address.
void Location(uintptr_t fn, char *out, size_t capacity) {
    snprintf(out, capacity, "unknown+0x%lx", fn);
    FILE *maps = fopen("/proc/self/maps", "re");
    if (!maps) return;
    char line[1536], perms[5];
    uintptr_t begin, end, offset;
    unsigned major, minor;
    unsigned long inode;
    int used;
    while (fgets(line, sizeof(line), maps)) {
        if (sscanf(line, "%lx-%lx %4s %lx %x:%x %lu%n", &begin, &end, perms,
                   &offset, &major, &minor, &inode, &used) != 7) continue;
        if (fn < begin || fn >= end) continue;
        char *path = line + used;
        while (*path == ' ') ++path;
        path[strcspn(path, "\r\n")] = 0;
        const char *base = strrchr(path, '/');
        snprintf(out, capacity, "%s+0x%lx", base ? base + 1 : (*path ? path : "anonymous"),
                 offset + fn - begin);
        break;
    }
    fclose(maps);
}

void Resolve(JNIEnv *env, jmethodID get_name, const Entry &entry, char *out, size_t capacity) {
    snprintf(out, capacity, "(unresolved)");
    if (get_name) {
        auto name = static_cast<jstring>(env->CallObjectMethod(entry.clazz, get_name));
        if (!env->ExceptionCheck() && name) {
            const char *chars = env->GetStringUTFChars(name, nullptr);
            if (chars) {
                snprintf(out, capacity, "%s", chars);
                env->ReleaseStringUTFChars(name, chars);
            }
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (name) env->DeleteLocalRef(name);
    }
    env->DeleteGlobalRef(entry.clazz);
}

char g_key_summary[512] = "key_captured=no";

bool CountRow(satori::Wcdb *db, void *stmt, void *context) {
    *static_cast<int *>(context) = static_cast<int>(satori::WcdbInt(db, stmt, 0));
    return false;
}

// Opens the account database read-only with each captured cipher spec and a bounded set of
// SQLCipher parameters, stopping at the first that decrypts. Keys are never logged.
void VerifyKey() {
    if (__atomic_exchange_n(&g_verify_needed, 0, __ATOMIC_ACQ_REL) == 0) return;
    CipherSpec specs[kSpecMax];
    int count = 0;
    pthread_mutex_lock(&g_key_mu);
    count = g_spec_count;
    for (int i = 0; i < count; ++i) specs[i] = g_specs[i];
    pthread_mutex_unlock(&g_key_mu);
    if (count == 0) return;
    char micro[1200];
    snprintf(micro, sizeof(micro), "%s/MicroMsg", g_app_data);
    char path[1600] = {};
    DIR *dir = opendir(micro);
    if (dir) {
        for (dirent *entry; (entry = readdir(dir)); ) {
            if (entry->d_name[0] == '.') continue;
            char candidate[1600];
            snprintf(candidate, sizeof(candidate), "%s/%s/EnMicroMsg.db", micro, entry->d_name);
            if (!access(candidate, R_OK)) { snprintf(path, sizeof(path), "%s", candidate); break; }
        }
        closedir(dir);
    }
    if (!path[0]) {
        snprintf(g_key_summary, sizeof(g_key_summary), "key_captured=yes specs=%d db_found=no", count);
    } else {
        const int pages[] = {0, 1024, 4096, 8192};
        const int versions[] = {0, 1, 2, 3, 4};
        constexpr int kAttemptLimit = 256;
        int attempts = 0;
        bool done = false;
        bool limited = false;
        for (int i = 0; i < count && !done; ++i) {
            for (int c = -1; c < 20 && !done; ++c) {
                const int page = c < 0 ? specs[i].page_size : pages[c % 4];
                const int version = c < 0 ? specs[i].version : versions[c / 4];
                if (c < 0 && page <= 0 && version <= 0) continue;
                for (int before = 0; before < 2 && !done; ++before) {
                    if (++attempts > kAttemptLimit) { done = true; limited = true; break; }
                    satori::Wcdb *db = satori::WcdbOpenEx("libWCDB.so", path, specs[i].key, specs[i].key_size, page, version, before, 1);
                    if (!db) continue;
                    int rows = 0;
                    const bool ok = satori::WcdbQuery(db, "SELECT count(*) FROM message", CountRow, &rows);
                    satori::WcdbClose(db);
                    if (ok) {
                        snprintf(g_key_summary, sizeof(g_key_summary),
                                 "key_captured=yes specs=%d db_readable=yes spec=%d len=%d page=%d ver=%d before=%d rows=%d",
                                 count, i, specs[i].key_size, page, version, before, rows);
                        done = true;
                    }
                }
            }
        }
        if (!done) snprintf(g_key_summary, sizeof(g_key_summary), "key_captured=yes specs=%d db_found=yes db_readable=no attempts=%d%s",
                            count, attempts, limited ? " limited" : "");
    }
    // Write the result (never the key) immediately so an on-boot check can read it.
    char keylog[1300];
    snprintf(keylog, sizeof(keylog), "%s/key.log", g_dir);
    FILE *file = fopen(keylog, "w");
    if (file) { fprintf(file, "%s\n", g_key_summary); fclose(file); }
}

void Snapshot(int dir) {
    const int out = OpenLog(dir, "maps.log");
    const int in = open("/proc/self/maps", O_RDONLY | O_CLOEXEC);
    if (out >= 0 && in >= 0) {
        char buf[4096];
        size_t total = 0;
        while (total < kLogLimit) {
            const ssize_t n = read(in, buf, sizeof(buf));
            if (n < 0 && errno == EINTR) continue;
            if (n <= 0 || !WriteAll(out, buf, static_cast<size_t>(n))) break;
            total += n;
        }
    }
    if (in >= 0) close(in);
    if (out >= 0) close(out);
}

void *Worker(void *) {
    pthread_setname_np(pthread_self(), "satori-wx");
    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThreadAsDaemon(&env, nullptr) != JNI_OK) {
        __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "worker attach failed; no patch installed");
        return nullptr;
    }
    // Bootstrap classes only; never wait for ActivityThread, load app classes or exempt hidden APIs.
    jclass cls = env->FindClass("java/lang/Class");
    jmethodID get_name = cls ? env->GetMethodID(cls, "getName", "()Ljava/lang/String;") : nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (cls) env->DeleteLocalRef(cls);
    if (mkdir(g_dir, 0700) != 0 && errno != EEXIST) {
        __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "cannot create probe directory: %s", strerror(errno));
        g_vm->DetachCurrentThread();
        return nullptr;
    }
    const int dir = open(g_dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
    const int boundary = dir >= 0 ? OpenLog(dir, "boundary.log") : -1;
    const int meta = dir >= 0 ? OpenLog(dir, "meta.log") : -1;
    if (boundary < 0 || meta < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "cannot open probe logs; no patch installed");
        if (boundary >= 0) close(boundary);
        if (meta >= 0) close(meta);
        if (dir >= 0) close(dir);
        g_vm->DetachCurrentThread();
        return nullptr;
    }
    g_started_ms = NowMs();
    pthread_mutex_lock(&g_mu);
    const bool installed = get_name && g_patch.install(g_slot, ObserveRegister);
    g_active = installed && g_patch.protection_ok();
    pthread_mutex_unlock(&g_mu);
    char line[1536];
    int len = snprintf(line, sizeof(line), "version=" SATORI_WX_VERSION "\npid=%d\nuid=%d\npatched=%s\n"
                       "start_monotonic_ms=%lld\nwindow_ms=%lld\ncall_limit=%ld\n",
                       getpid(), getuid(), installed ? "yes" : "no",
                       static_cast<long long>(g_started_ms), static_cast<long long>(kWindowMs), kCallLimit);
    bool io_ok = WriteAll(meta, line, Printed(len, sizeof(line)));
    bool stopping = false;
    const char *restore = "not-installed";
    const char *reason = "window";
    size_t written = 0;
    for (;;) {
        pthread_mutex_lock(&g_mu);
        const bool active = g_active;
        pthread_mutex_unlock(&g_mu);
        if (!stopping && (!active || !io_ok || written >= kLogLimit || NowMs() - g_started_ms >= kWindowMs)) {
            reason = !installed ? "passive" : !g_patch.protection_ok() ? "protection-failed" :
                     !io_ok ? "io-error" : written >= kLogLimit ? "log-limit" : !active ? "call-limit" : "window";
            restore = Stop();
            stopping = true;
        }
        if (__atomic_load_n(&g_verify_needed, __ATOMIC_ACQUIRE)) {
            VerifyKey();
            if (io_ok) {
                char verify[768];
                const int length = snprintf(verify, sizeof(verify), "%lld %d %s\n",
                                            static_cast<long long>(NowMs()), static_cast<int>(syscall(SYS_gettid)), g_key_summary);
                WriteAll(boundary, verify, Printed(length, sizeof(verify)));
            }
        }
        Entry entry{};
        if (!Pop(&entry)) {
            if (stopping) break;
            usleep(10000);
            continue;
        }
        char name[256];
        Resolve(env, get_name, entry, name, sizeof(name));
        if (!io_ok || written >= kLogLimit) continue; // Still release every queued reference.
        char location[384];
        Location(entry.fn, location, sizeof(location));
        len = snprintf(line, sizeof(line), "%lld %d %s %s %s %s\n",
                       static_cast<long long>(entry.time_ms), entry.tid, location, name, entry.name, entry.sig);
        const size_t size = Printed(len, sizeof(line));
        if (size > kLogLimit - written) { written = kLogLimit; continue; }
        io_ok = WriteAll(boundary, line, size);
        written += size;
    }
    Snapshot(dir);
    len = snprintf(line, sizeof(line), "stop=%s\nrestore=%s\nprotection_ok=%s\n"
                   "calls=%ld\nmethods=%ld\ndrops=%ld\nbytes=%zu\nio_ok=%s\n%s\nend_monotonic_ms=%lld\n",
                   reason, restore, g_patch.protection_ok() ? "yes" : "no", g_calls, g_methods,
                   g_drops, written, io_ok ? "yes" : "no", g_key_summary, static_cast<long long>(NowMs()));
    const bool meta_ok = WriteAll(meta, line, Printed(len, sizeof(line)));
    __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "probe stop=%s restore=%s methods=%ld drops=%ld meta_ok=%d",
                        reason, restore, g_methods, g_drops, meta_ok);
    close(boundary);
    close(meta);
    close(dir);
    g_vm->DetachCurrentThread();
    return nullptr;
}

class WxProbeModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override { api_ = api; env_ = env; }
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        if (args && args->nice_name && args->app_data_dir && !env_->ExceptionCheck()) {
            const char *name = env_->GetStringUTFChars(args->nice_name, nullptr);
            target_ = name && strcmp(name, kTarget) == 0;
            if (name) env_->ReleaseStringUTFChars(args->nice_name, name);
            if (env_->ExceptionCheck()) env_->ExceptionClear();
            if (target_) {
                const char *data = env_->GetStringUTFChars(args->app_data_dir, nullptr);
                const int n = data ? snprintf(g_dir, sizeof(g_dir), "%s/files/satori-wx-probe", data) : -1;
                if (data) snprintf(g_app_data, sizeof(g_app_data), "%s", data);
                target_ = n > 0 && static_cast<size_t>(n) < sizeof(g_dir);
                if (data) env_->ReleaseStringUTFChars(args->app_data_dir, data);
                if (env_->ExceptionCheck()) env_->ExceptionClear();
            }
        }
        if (!target_) api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }
    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!target_ || env_->GetJavaVM(&g_vm) != JNI_OK) return;
        // Worker attaches before patching. Some very early registrations can be missed.
        g_slot = const_cast<RegisterFn *>(&env_->functions->RegisterNatives);
        pthread_t thread;
        const int error = pthread_create(&thread, nullptr, Worker, nullptr);
        if (!error) pthread_detach(thread);
        else __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "pthread_create: %s", strerror(error));
    }
    void preServerSpecialize(zygisk::ServerSpecializeArgs *) override {
        api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }
private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool target_ = false;
};
} // namespace

REGISTER_ZYGISK_MODULE(WxProbeModule)
