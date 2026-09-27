// Reflection-only sender: no hooks, no code patches, no dex, no ArtMethod changes.
//
// Verified against WeChat 8.0.78 (base.apk, versionCode 671108664):
//   com.tencent.mm.network.a3.c()            static -> j1 dispatcher (com.tencent.mm.network.s)
//   v51.r0.<init>(String,String,int,int,long,String)  -> inserts the message into the app db
//   v51.r0.f  (long)                         -> local message id returned by that insert
//   v51.r0.doScene(com.tencent.mm.network.s, com.tencent.mm.modelbase.u0) -> int
//   com.tencent.mm.network.y2.<init>()       -> the no-op IOnSceneEnd the app itself uses
// a3.b(j1, m1) is the same call the app makes, but it hides doScene's return value.
#include "wx_send.h"
#include "wx_capabilities.h"
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <android/log.h>

namespace satori {
namespace {
constexpr long long kMinIntervalMs = 1500;
constexpr int kMaxPerMinute = 10;
constexpr size_t kAllowMax = 512;

JavaVM *g_vm = nullptr;
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
char g_allow[kAllowMax] = {};
long long g_last_ms = 0;
long long g_minute_start_ms = 0;
int g_minute_count = 0;
long long g_sent = 0;
long long g_failed = 0;

// Resolved once on the host class loader and then only read.
bool g_resolved = false;
jobject g_loader = nullptr;      // java.lang.ClassLoader (app)
jclass g_r0 = nullptr;           // v51.r0
jmethodID g_r0_ctor = nullptr;   // (String,String,int,int,long,String)V
jfieldID g_r0_local = nullptr;   // f:J
jmethodID g_r0_do_scene = nullptr; // (com.tencent.mm.network.s, com.tencent.mm.modelbase.u0)I
jclass g_y2 = nullptr;           // com.tencent.mm.network.y2
jmethodID g_y2_ctor = nullptr;   // ()V
jclass g_a3 = nullptr;           // com.tencent.mm.network.a3
jmethodID g_a3_dispatcher = nullptr; // ()Lcom/tencent/mm/network/j1;

long long NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void Detail(char *out, size_t size, const char *format, ...) {
    if (!out || !size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(out, size, format, args);
    va_end(args);
}

// The send pipeline creates arg-less Handlers on the calling thread once the Looper is
// present; without one those paths throw. Prepare one so dispatch degrades cleanly.
void PrepareLooper(JNIEnv *env) {
    jclass looper = env->FindClass("android/os/Looper");
    if (!looper) { env->ExceptionClear(); return; }
    jmethodID mine = env->GetStaticMethodID(looper, "myLooper", "()Landroid/os/Looper;");
    jmethodID prepare = env->GetStaticMethodID(looper, "prepare", "()V");
    if (mine && prepare && !env->CallStaticObjectMethod(looper, mine)) {
        env->ExceptionClear();
        env->CallStaticVoidMethod(looper, prepare);
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    env->DeleteLocalRef(looper);
}

JNIEnv *Env() {
    if (!g_vm) return nullptr;
    JNIEnv *env = nullptr;
    if (g_vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (g_vm->AttachCurrentThreadAsDaemon(&env, nullptr) != JNI_OK) return nullptr;
    PrepareLooper(env);
    return env;
}

jclass LoadClass(JNIEnv *env, jobject loader, jmethodID load, const char *name) {
    jstring text = env->NewStringUTF(name);
    if (!text) return nullptr;
    auto cls = static_cast<jclass>(env->CallObjectMethod(loader, load, text));
    env->DeleteLocalRef(text);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return cls;
}

// Builds the cache from the host class loader. The app's classes are invisible to native
// FindClass, so everything goes through ActivityThread.currentApplication()'s loader.
bool Resolve(JNIEnv *env, char *detail, size_t size) {
    jclass activity_thread = env->FindClass("android/app/ActivityThread");
    if (!activity_thread) { env->ExceptionClear(); Detail(detail, size, "ActivityThread missing"); return false; }
    jmethodID current = env->GetStaticMethodID(activity_thread, "currentApplication", "()Landroid/app/Application;");
    jobject application = current ? env->CallStaticObjectMethod(activity_thread, current) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); application = nullptr; }
    if (!application) { env->DeleteLocalRef(activity_thread); Detail(detail, size, "application not ready"); return false; }
    jclass application_class = env->FindClass("android/app/Application");
    jmethodID get_loader = application_class ? env->GetMethodID(application_class, "getClassLoader", "()Ljava/lang/ClassLoader;") : nullptr;
    jobject loader = get_loader ? env->CallObjectMethod(application, get_loader) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); loader = nullptr; }
    if (!loader) {
        env->DeleteLocalRef(application);
        if (application_class) env->DeleteLocalRef(application_class);
        env->DeleteLocalRef(activity_thread);
        Detail(detail, size, "application class loader unavailable");
        return false;
    }
    jclass loader_class = env->FindClass("java/lang/ClassLoader");
    jmethodID load = loader_class ? env->GetMethodID(loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;") : nullptr;
    if (!load) {
        env->ExceptionClear();
        env->DeleteLocalRef(loader);
        if (loader_class) env->DeleteLocalRef(loader_class);
        env->DeleteLocalRef(application);
        if (application_class) env->DeleteLocalRef(application_class);
        env->DeleteLocalRef(activity_thread);
        Detail(detail, size, "ClassLoader.loadClass unavailable");
        return false;
    }

    bool ok = true;
    jclass r0 = LoadClass(env, loader, load, "v51.r0");
    jclass y2 = LoadClass(env, loader, load, "com.tencent.mm.network.y2");
    jclass a3 = LoadClass(env, loader, load, "com.tencent.mm.network.a3");
    if (!r0 || !y2 || !a3) {
        ok = false;
        Detail(detail, size, "send classes not found (version mismatch?)");
    }
    if (ok) {
        g_r0_ctor = env->GetMethodID(r0, "<init>", "(Ljava/lang/String;Ljava/lang/String;IIJLjava/lang/String;)V");
        g_r0_local = env->GetFieldID(r0, "f", "J");
        g_r0_do_scene = env->GetMethodID(r0, "doScene", "(Lcom/tencent/mm/network/s;Lcom/tencent/mm/modelbase/u0;)I");
        g_y2_ctor = env->GetMethodID(y2, "<init>", "()V");
        g_a3_dispatcher = env->GetStaticMethodID(a3, "c", "()Lcom/tencent/mm/network/j1;");
        if (!g_r0_ctor || !g_r0_local || !g_r0_do_scene || !g_y2_ctor || !g_a3_dispatcher) {
            env->ExceptionClear();
            ok = false;
            Detail(detail, size, "send methods not found (version mismatch?)");
        }
    }
    if (ok) {
        g_loader = env->NewGlobalRef(loader);
        g_r0 = static_cast<jclass>(env->NewGlobalRef(r0));
        g_y2 = static_cast<jclass>(env->NewGlobalRef(y2));
        g_a3 = static_cast<jclass>(env->NewGlobalRef(a3));
        if (!g_loader || !g_r0 || !g_y2 || !g_a3) {
            ok = false;
            Detail(detail, size, "global reference allocation failed");
        } else {
            g_resolved = true;
        }
    }
    if (r0) env->DeleteLocalRef(r0);
    if (y2) env->DeleteLocalRef(y2);
    if (a3) env->DeleteLocalRef(a3);
    env->DeleteLocalRef(loader);
    env->DeleteLocalRef(loader_class);
    env->DeleteLocalRef(application);
    env->DeleteLocalRef(application_class);
    env->DeleteLocalRef(activity_thread);
    return ok;
}

bool Allowed(const char *talker) {
    if (!g_allow[0]) return false;
    const size_t length = strlen(talker);
    const char *cursor = g_allow;
    while (*cursor) {
        const char *end = strchr(cursor, ';');
        const size_t span = end ? static_cast<size_t>(end - cursor) : strlen(cursor);
        if (span == length && !strncmp(cursor, talker, length)) return true;
        if (!end) break;
        cursor = end + 1;
    }
    return false;
}

// Consumes one slot from the pacing window. Returns false when the caller must wait.
bool Pacing() {
    const long long now = NowMs();
    pthread_mutex_lock(&g_mu);
    bool allowed = false;
    if (g_last_ms == 0 || now - g_last_ms >= kMinIntervalMs) {
        if (g_minute_start_ms == 0 || now - g_minute_start_ms >= 60000) {
            g_minute_start_ms = now;
            g_minute_count = 0;
        }
        if (g_minute_count < kMaxPerMinute) {
            ++g_minute_count;
            g_last_ms = now;
            allowed = true;
        }
    }
    pthread_mutex_unlock(&g_mu);
    return allowed;
}
} // namespace

void SendInit(void *vm) { g_vm = static_cast<JavaVM *>(vm); }

void SendConfigure(const char *allow_semicolon_list) {
    pthread_mutex_lock(&g_mu);
    if (allow_semicolon_list) snprintf(g_allow, sizeof(g_allow), "%s", allow_semicolon_list);
    else g_allow[0] = 0;
    g_last_ms = 0;
    g_minute_start_ms = 0;
    g_minute_count = 0;
    pthread_mutex_unlock(&g_mu);
}

bool SendReady() { return g_resolved; }

void SendStats(long long *sent, long long *failed) {
    pthread_mutex_lock(&g_mu);
    if (sent) *sent = g_sent;
    if (failed) *failed = g_failed;
    pthread_mutex_unlock(&g_mu);
}

SendResult SendText(const char *talker, const char *content) {
    SendResult result{};
    result.local_id = -1;
    result.net_id = -1;
    if (!talker || !*talker || !content || !*content) {
        Detail(result.detail, sizeof(result.detail), "empty target or content");
        return result;
    }
    if (!SendEnabled()) {
        Detail(result.detail, sizeof(result.detail), "send is disabled by configuration");
        return result;
    }
    if (!Allowed(talker)) {
        Detail(result.detail, sizeof(result.detail), "target not in send_allow");
        return result;
    }
    if (!Pacing()) {
        Detail(result.detail, sizeof(result.detail), "rate limited");
        return result;
    }
    JNIEnv *env = Env();
    if (!env) {
        Detail(result.detail, sizeof(result.detail), "JavaVM unavailable");
        return result;
    }
    pthread_mutex_lock(&g_mu);
    const bool resolved = g_resolved || Resolve(env, result.detail, sizeof(result.detail));
    pthread_mutex_unlock(&g_mu);
    if (!resolved) return result;

    jstring jtalker = env->NewStringUTF(talker);
    jstring jcontent = env->NewStringUTF(content);
    jstring jempty = env->NewStringUTF("");
    if (!jtalker || !jcontent || !jempty) {
        if (jtalker) env->DeleteLocalRef(jtalker);
        if (jcontent) env->DeleteLocalRef(jcontent);
        if (jempty) env->DeleteLocalRef(jempty);
        Detail(result.detail, sizeof(result.detail), "string allocation failed");
        return result;
    }
    // The constructor inserts the row into WeChat's own message table (status SENDING);
    // doScene() then picks pending rows up and hands them to mars.
    jobject scene = env->NewObject(g_r0, g_r0_ctor, jtalker, jcontent, static_cast<jint>(1), static_cast<jint>(0),
                                   static_cast<jlong>(0), jempty);
    env->DeleteLocalRef(jtalker);
    env->DeleteLocalRef(jcontent);
    env->DeleteLocalRef(jempty);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        Detail(result.detail, sizeof(result.detail), "scene construction failed");
        return result;
    }
    if (!scene) {
        Detail(result.detail, sizeof(result.detail), "scene construction returned null");
        return result;
    }
    result.local_id = env->GetLongField(scene, g_r0_local);
    jobject dispatcher = env->CallStaticObjectMethod(g_a3, g_a3_dispatcher);
    if (env->ExceptionCheck()) { env->ExceptionClear(); dispatcher = nullptr; }
    if (!dispatcher) {
        env->DeleteLocalRef(scene);
        Detail(result.detail, sizeof(result.detail), "network dispatcher unavailable (logged in?)");
        return result;
    }
    jobject callback = env->NewObject(g_y2, g_y2_ctor);
    if (env->ExceptionCheck()) { env->ExceptionClear(); callback = nullptr; }
    if (!callback) {
        env->DeleteLocalRef(dispatcher);
        env->DeleteLocalRef(scene);
        Detail(result.detail, sizeof(result.detail), "callback allocation failed");
        return result;
    }
    jint net = -1;
    net = env->CallIntMethod(scene, g_r0_do_scene, dispatcher, callback);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        Detail(result.detail, sizeof(result.detail), "dispatch threw");
    } else if (net < 0) {
        Detail(result.detail, sizeof(result.detail), "dispatch rejected (netId=%d)", static_cast<int>(net));
    } else {
        result.ok = true;
    }
    result.net_id = static_cast<int>(net);
    env->DeleteLocalRef(callback);
    env->DeleteLocalRef(dispatcher);
    env->DeleteLocalRef(scene);
    pthread_mutex_lock(&g_mu);
    if (result.ok) ++g_sent; else ++g_failed;
    pthread_mutex_unlock(&g_mu);
    if (result.ok) __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "sent to %s (local id %lld, netId %d)",
                                       talker, result.local_id, result.net_id);
    else __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "send to %s failed: %s", talker, result.detail);
    return result;
}
} // namespace satori
