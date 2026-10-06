// Key capture, folded in from the old satori-wx-probe module. Still no hook engine, no DEX,
// no ArtMethod rewriting: only the fnPtrs of two JNI natives are replaced inside a writable
// copy of the array the app passes to RegisterNatives, and the wrappers forward unchanged.
#include "wx_key.h"
#include "data_slot.h"
#include <errno.h>
#include <jni.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <android/log.h>

namespace satori {
namespace {
constexpr size_t kKeyMax = 64;
constexpr int kSpecMax = 16;
constexpr jint kMethodCopyLimit = 256;

using RegisterFn = jint (*)(JNIEnv *, jclass, const JNINativeMethod *, jint);
using SetKeyFn = void (*)(JNIEnv *, jobject, jlong, jbyteArray);
using SetCipherKeyFn = void (*)(JNIEnv *, jobject, jlong, jbyteArray, jint, jint);

struct CipherSpec {
    unsigned char key[kKeyMax];
    int key_size;
    int page_size;
    int version;
};

JavaVM *g_vm = nullptr;
char g_dir[1200] = {};
wx::DataSlot<RegisterFn> g_patch;
RegisterFn *g_slot = nullptr;
SetKeyFn g_original_set_key = nullptr;
SetCipherKeyFn g_original_cipher_key = nullptr;
CipherSpec g_specs[kSpecMax];
int g_spec_count = 0;
volatile int g_verify_needed = 0;
pthread_mutex_t g_key_mu = PTHREAD_MUTEX_INITIALIZER;

int ReadKey(JNIEnv *env, jbyteArray array, unsigned char *out) {
    if (!array || env->ExceptionCheck()) return 0;
    const jsize size = env->GetArrayLength(array);
    if (size <= 0 || static_cast<size_t>(size) > kKeyMax) return 0;
    env->GetByteArrayRegion(array, 0, size, reinterpret_cast<jbyte *>(out));
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        return 0;
    }
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
            // WeChat re-registers these natives several times, so a one-shot guard would let a
            // later registration restore the original pointer.
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
    return g_patch.original()(env, clazz, argument, n);
}

// Dumps the captured cipher specs to a private 0600 file and nothing else. The live WeChat
// database is deliberately never opened here: opening it with candidate keys while WeChat
// uses it can corrupt a WAL mmap and crash the host (observed SIGBUS).
void DumpSpecs() {
    if (__atomic_exchange_n(&g_verify_needed, 0, __ATOMIC_ACQ_REL) == 0) return;
    CipherSpec specs[kSpecMax];
    int count = 0;
    pthread_mutex_lock(&g_key_mu);
    count = g_spec_count;
    for (int i = 0; i < count; ++i) specs[i] = g_specs[i];
    pthread_mutex_unlock(&g_key_mu);
    char path[1300];
    snprintf(path, sizeof(path), "%s/key.log", g_dir);
    FILE *file = fopen(path, "w");
    if (!file) return;
    for (int i = 0; i < count; ++i) {
        fprintf(file, "spec=%d len=%d page=%d ver=%d hex=", i, specs[i].key_size, specs[i].page_size, specs[i].version);
        for (int j = 0; j < specs[i].key_size; ++j) fprintf(file, "%02x", specs[i].key[j]);
        fprintf(file, "\n");
    }
    fclose(file);
    chmod(path, 0600);
}

void *Worker(void *) {
    pthread_setname_np(pthread_self(), "satori-wx-key");
    JNIEnv *env = nullptr;
    if (g_vm->AttachCurrentThreadAsDaemon(&env, nullptr) != JNI_OK) {
        __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "key capture: attach failed");
        return nullptr;
    }
    if (mkdir(g_dir, 0700) != 0 && errno != EEXIST) {
        __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "key capture: cannot create %s", g_dir);
        g_vm->DetachCurrentThread();
        return nullptr;
    }
    g_slot = const_cast<RegisterFn *>(&env->functions->RegisterNatives);
    const bool installed = g_patch.install(g_slot, ObserveRegister);
    __android_log_print(installed ? ANDROID_LOG_INFO : ANDROID_LOG_ERROR, "SatoriWx", "key capture: patched=%d",
                        installed ? 1 : 0);
    // Stay resident: WeChat re-registers natives and can switch accounts, so later specs must
    // still be captured. Each new spec rewrites key.log for the store to pick up.
    for (;;) {
        if (__atomic_load_n(&g_verify_needed, __ATOMIC_ACQUIRE)) DumpSpecs();
        const timespec delay{1, 0};
        nanosleep(&delay, nullptr);
    }
    g_vm->DetachCurrentThread();
    return nullptr;
}
} // namespace

void KeyCaptureStart(void *vm, const char *app_data_dir) {
    if (!vm || !app_data_dir || !*app_data_dir) return;
    g_vm = static_cast<JavaVM *>(vm);
    snprintf(g_dir, sizeof(g_dir), "%s/files/satori-wx", app_data_dir);
    pthread_t thread;
    const int error = pthread_create(&thread, nullptr, Worker, nullptr);
    if (!error)
        pthread_detach(thread);
    else
        __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "key capture thread: %s", strerror(error));
}
} // namespace satori
