// Reflection-only sender: no hooks, no code patches, no dex, no ArtMethod changes.
//
// Verified against WeChat 8.0.78 (base.apk, versionCode 671108664):
//   com.tencent.mm.network.a3.c()            static -> j1 dispatcher (com.tencent.mm.network.s)
//   v51.r0.<init>(String,String,int,int,long,String)  -> inserts the message into the app db
//   v51.r0.f  (long)                         -> local message id returned by that insert
//   v51.r0.doScene(com.tencent.mm.network.s, com.tencent.mm.modelbase.u0) -> int
//   com.tencent.mm.network.y2.<init>()       -> the no-op IOnSceneEnd the app itself uses
// a3.b(j1, m1) is the same call the app makes, but it hides doScene's return value.
//
// Recall (撤回) uses the app's own revoke scene:
//   ex0.k0.F0 (ex0.j0)  .k(talker, localId) -> com.tencent.mm.storage.e9 (MsgInfo)
//   com.tencent.mm.modelsimple.d1.<init>(e9, hint, "") -> cgi /cgi-bin/micromsg-bin/revokemsg
//   d1.doScene(dispatcher, com.tencent.mm.network.y2)   -> int netId (>= 0 accepted).
#include "wx_send.h"
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <android/log.h>

namespace satori {
namespace {
JavaVM *g_vm = nullptr;
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
long long g_sent = 0;
long long g_failed = 0;
long long g_rejected = 0;
long long g_attempt_ms = 0;   // last SendText() attempt (monotonic), 0 = none yet
bool g_last_ok = false;
int g_last_net = -1;
long long g_last_local = -1;
char g_last_target[96] = {};
char g_last_error[160] = {};

// Resolved once on the host class loader and then only read.
bool g_resolved = false;
bool g_dispatcher_ok = false;
jobject g_loader = nullptr;      // java.lang.ClassLoader (app)
jmethodID g_loader_load = nullptr; // ClassLoader.loadClass(String), cached by Resolve
jclass g_r0 = nullptr;           // v51.r0
jmethodID g_r0_ctor = nullptr;   // (String,String,int,int,long,String)V
jmethodID g_r0_ctor_map = nullptr; // (String,String,int,int,Object,String)V: the overload that takes a msgsource map
jclass g_hashmap = nullptr;       // java.util.HashMap, for that map
jmethodID g_hashmap_ctor = nullptr;
jmethodID g_hashmap_put = nullptr;
jfieldID g_r0_local = nullptr;   // f:J
jmethodID g_r0_do_scene = nullptr; // (com.tencent.mm.network.s, com.tencent.mm.modelbase.u0)I
jclass g_y2 = nullptr;           // com.tencent.mm.network.y2
jmethodID g_y2_ctor = nullptr;   // ()V
jclass g_a3 = nullptr;           // com.tencent.mm.network.a3 (MMPushCore, :push only)
jmethodID g_a3_dispatcher = nullptr; // ()Lcom/tencent/mm/network/j1;
jclass g_r1 = nullptr;           // com.tencent.mm.modelbase.r1 (MMKernel network holder)
jfieldID g_r1_singleton = nullptr;   // y:Lcom/tencent/mm/modelbase/r1;
jmethodID g_r1_dispatcher = nullptr; // k()Lcom/tencent/mm/network/s;
// Recall: the message store and the revoke scene. Resolved leniently; when absent the
// recall call reports it instead of failing the whole sender. (e9 = com.tencent.mm.storage.e9)
jclass g_d1 = nullptr;             // com.tencent.mm.modelsimple.d1 (NetSceneRevokeMsg)
jmethodID g_d1_ctor = nullptr;     // (e9,String,String)V
jmethodID g_d1_do_scene = nullptr; // (com.tencent.mm.network.s, com.tencent.mm.modelbase.u0)I
jclass g_k0 = nullptr;             // ex0.k0 (message store holder)
jfieldID g_k0_store = nullptr;     // F0:Lex0/j0;
jclass g_j0 = nullptr;             // ex0.j0 (per-account message store)
jmethodID g_j0_get = nullptr;      // k(String,J)Le9;
jmethodID g_e9_is_send = nullptr;  // z0()I, 1 when the message was sent by this account
long long g_recalled = 0;
long long g_media = 0;

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

// A failed lookup leaves a pending NoSuchMethod/NoSuchFieldError; ART aborts the process if
// the next lookup throws on top of one. These helpers clear it right away and report failure
// as a null id, which the callers already treat as "capability unavailable".
jmethodID Method(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    if (!cls) return nullptr;
    jmethodID id = env->GetMethodID(cls, name, signature);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return id;
}

jmethodID StaticMethod(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    if (!cls) return nullptr;
    jmethodID id = env->GetStaticMethodID(cls, name, signature);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return id;
}

jfieldID Field(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    if (!cls) return nullptr;
    jfieldID id = env->GetFieldID(cls, name, signature);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return id;
}

jfieldID StaticField(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    if (!cls) return nullptr;
    jfieldID id = env->GetStaticFieldID(cls, name, signature);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return id;
}

// The send pipeline creates arg-less Handlers on the calling thread once the Looper is
// present; without one those paths throw. Prepare one so dispatch degrades cleanly.
void PrepareLooper(JNIEnv *env) {
    jclass looper = env->FindClass("android/os/Looper");
    if (!looper) { env->ExceptionClear(); return; }
    jmethodID mine = StaticMethod(env, looper, "myLooper", "()Landroid/os/Looper;");
    jmethodID prepare = StaticMethod(env, looper, "prepare", "()V");
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
    jmethodID current = StaticMethod(env, activity_thread, "currentApplication", "()Landroid/app/Application;");
    jobject application = current ? env->CallStaticObjectMethod(activity_thread, current) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); application = nullptr; }
    if (!application) { env->DeleteLocalRef(activity_thread); Detail(detail, size, "application not ready"); return false; }
    jclass application_class = env->FindClass("android/app/Application");
    jmethodID get_loader = Method(env, application_class, "getClassLoader", "()Ljava/lang/ClassLoader;");
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
    jmethodID load = Method(env, loader_class, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
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
    jclass r1 = LoadClass(env, loader, load, "com.tencent.mm.modelbase.r1");
    if (!r0 || !y2 || !a3) {
        ok = false;
        Detail(detail, size, "send classes not found (version mismatch?)");
    }
    if (ok) {
        g_r0_ctor = Method(env, r0, "<init>", "(Ljava/lang/String;Ljava/lang/String;IIJLjava/lang/String;)V");
        // Optional: the overload the chat UI uses to attach an <atuserlist>; a build without it
        // still sends text, just without real mentions.
        g_r0_ctor_map = Method(env, r0, "<init>", "(Ljava/lang/String;Ljava/lang/String;IILjava/lang/Object;Ljava/lang/String;)V");
        if (!g_r0_ctor_map) env->ExceptionClear();
        jclass hashmap = env->FindClass("java/util/HashMap");
        if (hashmap) {
            g_hashmap_ctor = Method(env, hashmap, "<init>", "()V");
            g_hashmap_put = Method(env, hashmap, "put", "(Ljava/lang/Object;Ljava/lang/Object;)Ljava/lang/Object;");
            if (g_hashmap_ctor && g_hashmap_put) g_hashmap = static_cast<jclass>(env->NewGlobalRef(hashmap));
            env->DeleteLocalRef(hashmap);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
        g_r0_local = Field(env, r0, "f", "J");
        g_r0_do_scene = Method(env, r0, "doScene", "(Lcom/tencent/mm/network/s;Lcom/tencent/mm/modelbase/u0;)I");
        g_y2_ctor = Method(env, y2, "<init>", "()V");
        g_a3_dispatcher = StaticMethod(env, a3, "c", "()Lcom/tencent/mm/network/j1;");
        if (!g_r0_ctor || !g_r0_local || !g_r0_do_scene || !g_y2_ctor || !g_a3_dispatcher) {
            env->ExceptionClear();
            ok = false;
            Detail(detail, size, "send methods not found (version mismatch?)");
        }
    }
    // The dispatcher in the main process is the remote one held by the MMKernel network
    // holder; a3's j1 only exists in :push. Not fatal if absent, so it is resolved leniently.
    if (ok && r1) {
        g_r1_singleton = StaticField(env, r1, "y", "Lcom/tencent/mm/modelbase/r1;");
        g_r1_dispatcher = Method(env, r1, "k", "()Lcom/tencent/mm/network/s;");
        if (!g_r1_singleton || !g_r1_dispatcher) env->ExceptionClear();
    }
    // Recall is a separate capability: resolve it without failing text sends when a class
    // or member is missing (another WeChat build, or the scene renamed).
    jclass d1 = LoadClass(env, loader, load, "com.tencent.mm.modelsimple.d1");
    jclass k0 = LoadClass(env, loader, load, "ex0.k0");
    jclass j0 = LoadClass(env, loader, load, "ex0.j0");
    jclass e9 = LoadClass(env, loader, load, "com.tencent.mm.storage.e9");
    if (ok && d1 && k0 && j0 && e9) {
        g_d1_ctor = Method(env, d1, "<init>", "(Lcom/tencent/mm/storage/e9;Ljava/lang/String;Ljava/lang/String;)V");
        g_d1_do_scene = Method(env, d1, "doScene", "(Lcom/tencent/mm/network/s;Lcom/tencent/mm/modelbase/u0;)I");
        g_k0_store = StaticField(env, k0, "F0", "Lex0/j0;");
        g_j0_get = Method(env, j0, "k", "(Ljava/lang/String;J)Lcom/tencent/mm/storage/e9;");
        g_e9_is_send = Method(env, e9, "z0", "()I");
        if (!g_d1_ctor || !g_d1_do_scene || !g_k0_store || !g_j0_get || !g_e9_is_send) {
            env->ExceptionClear();
            g_d1_ctor = nullptr;
            g_d1_do_scene = nullptr;
            g_k0_store = nullptr;
            g_j0_get = nullptr;
            g_e9_is_send = nullptr;
        }
    }
    if (ok) {
        g_loader = env->NewGlobalRef(loader);
        g_loader_load = load;
        g_r0 = static_cast<jclass>(env->NewGlobalRef(r0));
        g_y2 = static_cast<jclass>(env->NewGlobalRef(y2));
        g_a3 = static_cast<jclass>(env->NewGlobalRef(a3));
        g_r1 = r1 ? static_cast<jclass>(env->NewGlobalRef(r1)) : nullptr;
        g_d1 = d1 ? static_cast<jclass>(env->NewGlobalRef(d1)) : nullptr;
        g_k0 = k0 ? static_cast<jclass>(env->NewGlobalRef(k0)) : nullptr;
        g_j0 = j0 ? static_cast<jclass>(env->NewGlobalRef(j0)) : nullptr;
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
    if (r1) env->DeleteLocalRef(r1);
    if (d1) env->DeleteLocalRef(d1);
    if (k0) env->DeleteLocalRef(k0);
    if (j0) env->DeleteLocalRef(j0);
    if (e9) env->DeleteLocalRef(e9);
    env->DeleteLocalRef(loader);
    env->DeleteLocalRef(loader_class);
    env->DeleteLocalRef(application);
    env->DeleteLocalRef(application_class);
    env->DeleteLocalRef(activity_thread);
    return ok;
}

// Returns the scene dispatcher for this process, or null.
// The main process holds the MMKernel network wrapper (the remote dispatcher that forwards
// to :push); MMFushCore's j1, which owns the mars transport, only exists in :push.
jobject Dispatcher(JNIEnv *env, int *mask) {
    int found = 0;
    if (g_r1 && g_r1_singleton && g_r1_dispatcher) {
        jobject holder = env->GetStaticObjectField(g_r1, g_r1_singleton);
        if (env->ExceptionCheck()) { env->ExceptionClear(); holder = nullptr; }
        if (holder) {
            found |= 1;
            jobject local = env->CallObjectMethod(holder, g_r1_dispatcher);
            env->DeleteLocalRef(holder);
            if (env->ExceptionCheck()) env->ExceptionClear();
            else if (local) { if (mask) *mask = found | 2; return local; }
        }
    }
    jobject dispatcher = env->CallStaticObjectMethod(g_a3, g_a3_dispatcher);
    if (env->ExceptionCheck()) { env->ExceptionClear(); dispatcher = nullptr; }
    if (dispatcher) found |= 4;
    if (mask) *mask = found;
    return dispatcher;
}

} // namespace

void SendInit(void *vm) { g_vm = static_cast<JavaVM *>(vm); }

void *ReflectEnv() { return Env(); }

bool ReflectResolve(char *detail, size_t size) {
    JNIEnv *env = Env();
    if (!env) { if (detail && size) Detail(detail, size, "JavaVM unavailable"); return false; }
    pthread_mutex_lock(&g_mu);
    const bool ok = g_resolved || Resolve(env, detail, size);
    pthread_mutex_unlock(&g_mu);
    return ok;
}

void *ReflectLoad(const char *name) {
    JNIEnv *env = Env();
    if (!env || !g_loader || !g_loader_load) return nullptr;
    return LoadClass(env, g_loader, g_loader_load, name);
}

// Creates a no-op onSceneEnd callback (com.tencent.mm.network.y2) as a local reference.
void *ReflectCallback() {
    JNIEnv *env = Env();
    if (!env || !g_y2 || !g_y2_ctor) return nullptr;
    jobject callback = env->NewObject(g_y2, g_y2_ctor);
    if (env->ExceptionCheck()) { env->ExceptionClear(); return nullptr; }
    return callback;
}

// Calls scene.doScene(dispatcher, callback); returns the netId, or -1 with a reason.
int ReflectDispatchScene(void *scene, void *do_scene, char *detail, size_t size) {
    JNIEnv *env = Env();
    if (!env || !scene || !do_scene) { Detail(detail, size, "scene unavailable"); return -1; }
    int probe = 0;
    jobject dispatcher = Dispatcher(env, &probe);
    if (!dispatcher) {
        Detail(detail, size, "network dispatcher unavailable (probe=0x%x)", probe);
        return -1;
    }
    jobject callback = env->NewObject(g_y2, g_y2_ctor);
    if (env->ExceptionCheck()) { env->ExceptionClear(); callback = nullptr; }
    if (!callback) {
        env->DeleteLocalRef(dispatcher);
        Detail(detail, size, "callback allocation failed");
        return -1;
    }
    const jint net = env->CallIntMethod(static_cast<jobject>(scene), reinterpret_cast<jmethodID>(do_scene), dispatcher, callback);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        Detail(detail, size, "dispatch threw");
    } else if (net < 0) {
        Detail(detail, size, "dispatch rejected (netId=%d)", static_cast<int>(net));
    }
    env->DeleteLocalRef(callback);
    env->DeleteLocalRef(dispatcher);
    return net;
}

bool SendDispatcherReady() {
    pthread_mutex_lock(&g_mu);
    const bool ready = g_dispatcher_ok;
    pthread_mutex_unlock(&g_mu);
    return ready;
}

void SendStatusGet(SendStatus *status) {
    if (!status) return;
    pthread_mutex_lock(&g_mu);
    status->ready = g_vm != nullptr;
    status->resolved = g_resolved;
    status->dispatcher = g_dispatcher_ok;
    status->sent = g_sent;
    status->failed = g_failed;
    status->rejected = g_rejected;
    status->recalled = g_recalled;
    status->media = g_media;
    status->last_age_ms = g_attempt_ms ? NowMs() - g_attempt_ms : -1;
    status->last_ok = g_last_ok;
    status->last_net_id = g_last_net;
    status->last_local_id = g_last_local;
    snprintf(status->last_target, sizeof(status->last_target), "%s", g_last_target);
    snprintf(status->last_error, sizeof(status->last_error), "%s", g_last_error);
    pthread_mutex_unlock(&g_mu);
}

// Resolves the send classes and probes the dispatcher without sending anything. Called from
// the module's warm-up thread after login so the status block reports real capability.
bool SendWarmUp() {
    JNIEnv *env = Env();
    if (!env) return false;
    char detail[160] = {};
    pthread_mutex_lock(&g_mu);
    const bool resolved = g_resolved || Resolve(env, detail, sizeof(detail));
    pthread_mutex_unlock(&g_mu);
    if (!resolved) return false;
    int probe = 0;
    jobject dispatcher = Dispatcher(env, &probe);
    pthread_mutex_lock(&g_mu);
    g_dispatcher_ok = dispatcher != nullptr;
    pthread_mutex_unlock(&g_mu);
    __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "send warm-up: resolved=%d dispatcher=%d probe=0x%x",
                        resolved ? 1 : 0, dispatcher ? 1 : 0, probe);
    if (dispatcher) env->DeleteLocalRef(dispatcher);
    return true;
}

// Resolves the send classes and obtains the process dispatcher. Assumes the caller already
// checked the switch and the JavaVM. Never touches WeChat's database, so a failure here
// cannot leave an orphan SENDING row behind.
static jobject AcquireDispatcher(JNIEnv *env, SendResult &result) {
    pthread_mutex_lock(&g_mu);
    const bool resolved = g_resolved || Resolve(env, result.detail, sizeof(result.detail));
    pthread_mutex_unlock(&g_mu);
    if (!resolved) return nullptr;
    int probe = 0;
    jobject dispatcher = Dispatcher(env, &probe);
    pthread_mutex_lock(&g_mu);
    g_dispatcher_ok = dispatcher != nullptr;
    pthread_mutex_unlock(&g_mu);
    if (!dispatcher) {
        Detail(result.detail, sizeof(result.detail), "network dispatcher unavailable (probe=0x%x)", probe);
    }
    return dispatcher;
}

static SendResult SendTextInner(const char *talker, const char *content, const char *mention_ids) {
    SendResult result{};
    result.local_id = -1;
    result.net_id = -1;
    if (!talker || !*talker || !content || !*content) {
        Detail(result.detail, sizeof(result.detail), "empty target or content");
        return result;
    }
    JNIEnv *env = Env();
    if (!env) {
        Detail(result.detail, sizeof(result.detail), "JavaVM unavailable");
        return result;
    }
    // Resolve the dispatcher before touching WeChat's database: the scene constructor below
    // inserts a SENDING row, so a send that cannot be dispatched must not get that far.
    jobject dispatcher = AcquireDispatcher(env, result);
    if (!dispatcher) return result;
    jstring jtalker = env->NewStringUTF(talker);
    jstring jcontent = env->NewStringUTF(content);
    jstring jempty = env->NewStringUTF("");
    if (!jtalker || !jcontent || !jempty) {
        if (jtalker) env->DeleteLocalRef(jtalker);
        if (jcontent) env->DeleteLocalRef(jcontent);
        if (jempty) env->DeleteLocalRef(jempty);
        env->DeleteLocalRef(dispatcher);
        Detail(result.detail, sizeof(result.detail), "string allocation failed");
        return result;
    }
    // The constructor inserts the row into WeChat's own message table (status SENDING);
    // doScene() then picks pending rows up and hands them to mars.
    jobject scene = nullptr;
    jobject map = nullptr;
    if (mention_ids && *mention_ids && g_r0_ctor_map && g_hashmap) {
        // Same as the chat UI (AtSomeOneHelper): flag 1 tells the constructor to merge this map's
        // entries into <msgsource>, and "atuserlist" holds the CDATA-wrapped id list.
        map = env->NewObject(g_hashmap, g_hashmap_ctor);
        char value[2200];
        snprintf(value, sizeof(value), "<![CDATA[%s]]>", mention_ids);
        jstring jkey = env->NewStringUTF("atuserlist");
        jstring jvalue = env->NewStringUTF(value);
        if (map && jkey && jvalue) {
            jobject previous = env->CallObjectMethod(map, g_hashmap_put, jkey, jvalue);
            if (previous) env->DeleteLocalRef(previous);
        } else {
            if (map) { env->DeleteLocalRef(map); map = nullptr; }
        }
        if (jkey) env->DeleteLocalRef(jkey);
        if (jvalue) env->DeleteLocalRef(jvalue);
        if (env->ExceptionCheck()) { env->ExceptionClear(); if (map) { env->DeleteLocalRef(map); map = nullptr; } }
    }
    if (map) {
        scene = env->NewObject(g_r0, g_r0_ctor_map, jtalker, jcontent, static_cast<jint>(1), static_cast<jint>(1), map, jempty);
        env->DeleteLocalRef(map);
    } else {
        scene = env->NewObject(g_r0, g_r0_ctor, jtalker, jcontent, static_cast<jint>(1), static_cast<jint>(0),
                               static_cast<jlong>(0), jempty);
    }
    env->DeleteLocalRef(jtalker);
    env->DeleteLocalRef(jcontent);
    env->DeleteLocalRef(jempty);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(dispatcher);
        Detail(result.detail, sizeof(result.detail), "scene construction failed");
        return result;
    }
    if (!scene) {
        env->DeleteLocalRef(dispatcher);
        Detail(result.detail, sizeof(result.detail), "scene construction returned null");
        return result;
    }
    result.local_id = env->GetLongField(scene, g_r0_local);
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
    return result;
}

SendResult SendText(const char *talker, const char *content, const char *mention_ids) {
    SendResult result = SendTextInner(talker, content, mention_ids);
    pthread_mutex_lock(&g_mu);
    g_attempt_ms = NowMs();
    g_last_ok = result.ok;
    g_last_net = result.net_id;
    g_last_local = result.local_id;
    snprintf(g_last_target, sizeof(g_last_target), "%s", talker ? talker : "");
    snprintf(g_last_error, sizeof(g_last_error), "%s", result.ok ? "" : result.detail);
    if (result.ok) ++g_sent;
    else if (result.rejected) ++g_rejected;
    else ++g_failed;
    pthread_mutex_unlock(&g_mu);
    if (result.ok) __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "sent to %s (local id %lld, netId %d)",
                                       talker, result.local_id, result.net_id);
    else __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "send to %s failed%s: %s",
                             talker, result.rejected ? " (rejected)" : "", result.detail);
    return result;
}

SendResult SendImage(const char *talker, const char *self_id, const char *path) {
    SendResult result{};
    result.local_id = -1;
    result.net_id = -1;
    if (!talker || !*talker || !self_id || !*self_id || !path || !*path) {
        Detail(result.detail, sizeof(result.detail), "empty target, sender or path");
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

    // Everything below is looked up per call (a picture is rare enough) so a build without the
    // image classes degrades to one failed request, not a broken sender.
    jclass n0 = LoadClass(env, g_loader, g_loader_load, "ph5.n0");
    jclass service_interface = LoadClass(env, g_loader, g_loader_load, "kt.d1");
    jclass params_class = LoadClass(env, g_loader, g_loader_load, "da0.g");
    jclass context_class = LoadClass(env, g_loader, g_loader_load, "w90.i0");
    jclass callback_class = LoadClass(env, g_loader, g_loader_load, "b41.k7");
    jclass prepare_class = LoadClass(env, g_loader, g_loader_load, "com.tencent.mm.pluginsdk.ui.tools.p0");
    jobject service = nullptr, callback = nullptr, context = nullptr, params = nullptr, flow = nullptr;
    jstring jpath = nullptr, jself = nullptr, jtalker = nullptr, jsource = nullptr;
    jmethodID lookup = StaticMethod(env, n0, "c", "(Ljava/lang/Class;)Lph5/m;");
    jmethodID params_ctor = Method(env, params_class, "<init>", "(Ljava/lang/String;ILjava/lang/String;Ljava/lang/String;Lw90/i0;)V");
    jfieldID params_source = Field(env, params_class, "j", "Ljava/lang/String;");
    jmethodID context_ctor = Method(env, context_class, "<init>", "()V");
    jfieldID context_kind = Field(env, context_class, "a", "I");
    jfieldID context_callback = Field(env, context_class, "o", "Lb41/k7;");
    jmethodID callback_ctor = Method(env, callback_class, "<init>", "()V");
    jmethodID prepare = StaticMethod(env, prepare_class, "a", "()V");
    if (!n0 || !service_interface || !params_class || !context_class || !callback_class || !lookup || !params_ctor || !params_source ||
        !context_ctor || !context_kind || !context_callback || !callback_ctor) {
        Detail(result.detail, sizeof(result.detail), "image classes not found (version mismatch?)");
    } else {
        service = env->CallStaticObjectMethod(n0, lookup, service_interface);
        if (env->ExceptionCheck()) { env->ExceptionClear(); service = nullptr; }
        jclass service_class = service ? env->GetObjectClass(service) : nullptr;
        jmethodID send = service_class ? Method(env, service_class, "rj", "(Lda0/g;)Lkotlinx/coroutines/flow/j;") : nullptr;
        if (service_class) env->DeleteLocalRef(service_class);
        if (!service || !send) {
            Detail(result.detail, sizeof(result.detail), service ? "image service has no rj() (version mismatch?)" : "image service unavailable");
        } else {
            // The chat UI's own call, minus the UI: source 4 = "sent from a chat", a fresh
            // callback object for the pipeline to fill in, "msg_mgr_send_img" as the feature tag.
            if (prepare) { env->CallStaticVoidMethod(prepare_class, prepare); if (env->ExceptionCheck()) env->ExceptionClear(); }
            callback = env->NewObject(callback_class, callback_ctor);
            context = env->NewObject(context_class, context_ctor);
            jpath = env->NewStringUTF(path);
            jself = env->NewStringUTF(self_id);
            jtalker = env->NewStringUTF(talker);
            jsource = env->NewStringUTF("msg_mgr_send_img");
            if (env->ExceptionCheck()) { env->ExceptionClear(); callback = context = nullptr; }
            if (!callback || !context || !jpath || !jself || !jtalker || !jsource) {
                Detail(result.detail, sizeof(result.detail), "image argument allocation failed");
            } else {
                env->SetIntField(context, context_kind, 4);
                env->SetObjectField(context, context_callback, callback);
                params = env->NewObject(params_class, params_ctor, jpath, static_cast<jint>(0), jself, jtalker, context);
                if (env->ExceptionCheck()) { env->ExceptionClear(); params = nullptr; }
                if (!params) {
                    Detail(result.detail, sizeof(result.detail), "image parameters construction failed");
                } else {
                    env->SetObjectField(params, params_source, jsource);
                    flow = env->CallObjectMethod(service, send, params);
                    if (env->ExceptionCheck()) {
                        env->ExceptionClear();
                        Detail(result.detail, sizeof(result.detail), "image pipeline threw");
                    } else {
                        result.ok = true;  // launched; the progress flow is not needed
                    }
                }
            }
        }
    }
    jobject locals[] = {n0, service_interface, params_class, context_class, callback_class, prepare_class, service, callback, context, params, flow,
                        jpath, jself, jtalker, jsource};
    for (jobject local : locals) if (local) env->DeleteLocalRef(local);
    pthread_mutex_lock(&g_mu);
    g_attempt_ms = NowMs();
    g_last_ok = result.ok;
    g_last_net = -1;
    snprintf(g_last_target, sizeof(g_last_target), "%s", talker);
    snprintf(g_last_error, sizeof(g_last_error), "%s", result.ok ? "" : result.detail);
    if (result.ok) ++g_media; else ++g_failed;
    pthread_mutex_unlock(&g_mu);
    if (result.ok) __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "image handed to WeChat for %s", talker);
    else __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "image send to %s failed: %s", talker, result.detail);
    return result;
}

SendResult SendRecall(const char *talker, const char *message_id) {
    SendResult result{};
    result.local_id = -1;
    result.net_id = -1;
    if (!talker || !*talker || !message_id || !*message_id) {
        Detail(result.detail, sizeof(result.detail), "empty target or message id");
        return result;
    }
    char *tail = nullptr;
    const long long local_id = strtoll(message_id, &tail, 10);
    if (tail == message_id || (tail && *tail) || local_id <= 0) {
        result.rejected = true;
        Detail(result.detail, sizeof(result.detail), "message_id must be a positive decimal local id");
        return result;
    }
    result.local_id = local_id;
    JNIEnv *env = Env();
    if (!env) {
        Detail(result.detail, sizeof(result.detail), "JavaVM unavailable");
        return result;
    }
    pthread_mutex_lock(&g_mu);
    const bool resolved = g_resolved || Resolve(env, result.detail, sizeof(result.detail));
    pthread_mutex_unlock(&g_mu);
    if (!resolved) return result;
    if (!g_d1 || !g_d1_ctor || !g_d1_do_scene || !g_k0_store || !g_j0_get || !g_e9_is_send) {
        Detail(result.detail, sizeof(result.detail), "recall classes unavailable (version mismatch?)");
        return result;
    }
    int probe = 0;
    jobject dispatcher = Dispatcher(env, &probe);
    if (!dispatcher) {
        Detail(result.detail, sizeof(result.detail), "network dispatcher unavailable (probe=0x%x)", probe);
        return result;
    }
    // The account's message store gives us WeChat's own MsgInfo, which the revoke scene needs
    // to build /cgi-bin/micromsg-bin/revokemsg. Reading it is the only way the client-side
    // checks (does it exist, did we send it) are the same ones the app itself applies.
    jobject store = env->GetStaticObjectField(g_k0, g_k0_store);
    if (env->ExceptionCheck()) { env->ExceptionClear(); store = nullptr; }
    if (!store) {
        env->DeleteLocalRef(dispatcher);
        Detail(result.detail, sizeof(result.detail), "message store unavailable");
        return result;
    }
    jstring jtalker = env->NewStringUTF(talker);
    jobject info = jtalker ? env->CallObjectMethod(store, g_j0_get, jtalker, static_cast<jlong>(local_id)) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); info = nullptr; }
    env->DeleteLocalRef(store);
    if (jtalker) env->DeleteLocalRef(jtalker);
    if (!info) {
        env->DeleteLocalRef(dispatcher);
        Detail(result.detail, sizeof(result.detail), "message %lld not found in %s", local_id, talker);
        return result;
    }
    const jint is_send = env->CallIntMethod(info, g_e9_is_send);
    if (env->ExceptionCheck() || is_send != 1) {
        env->ExceptionClear();
        env->DeleteLocalRef(info);
        env->DeleteLocalRef(dispatcher);
        result.rejected = true;
        Detail(result.detail, sizeof(result.detail), "only messages sent by this account can be recalled");
        return result;
    }
    // The hint becomes the local "you recalled a message" system line; WeChat's own string for
    // it is R.string.b5s. Hardcoded so recall does not depend on resource ids.
    jstring jhint = env->NewStringUTF("你撤回了一条消息");
    jstring jempty = env->NewStringUTF("");
    jobject scene = (jhint && jempty) ? env->NewObject(g_d1, g_d1_ctor, info, jhint, jempty) : nullptr;
    if (env->ExceptionCheck()) { env->ExceptionClear(); scene = nullptr; }
    if (jhint) env->DeleteLocalRef(jhint);
    if (jempty) env->DeleteLocalRef(jempty);
    env->DeleteLocalRef(info);
    if (!scene) {
        env->DeleteLocalRef(dispatcher);
        Detail(result.detail, sizeof(result.detail), "revoke scene construction failed");
        return result;
    }
    jobject callback = env->NewObject(g_y2, g_y2_ctor);
    if (env->ExceptionCheck()) { env->ExceptionClear(); callback = nullptr; }
    if (!callback) {
        env->DeleteLocalRef(scene);
        env->DeleteLocalRef(dispatcher);
        Detail(result.detail, sizeof(result.detail), "callback allocation failed");
        return result;
    }
    jint net = -1;
    net = env->CallIntMethod(scene, g_d1_do_scene, dispatcher, callback);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        Detail(result.detail, sizeof(result.detail), "recall dispatch threw");
    } else if (net < 0) {
        Detail(result.detail, sizeof(result.detail), "recall rejected (netId=%d)", static_cast<int>(net));
    } else {
        result.ok = true;
    }
    result.net_id = static_cast<int>(net);
    env->DeleteLocalRef(callback);
    env->DeleteLocalRef(dispatcher);
    env->DeleteLocalRef(scene);
    pthread_mutex_lock(&g_mu);
    if (result.ok) ++g_recalled;
    else if (result.rejected) ++g_rejected;
    else ++g_failed;
    pthread_mutex_unlock(&g_mu);
    if (result.ok) __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "recalled %s in %s (netId %d)",
                                       message_id, talker, result.net_id);
    else __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "recall %s in %s failed%s: %s",
                             message_id, talker, result.rejected ? " (rejected)" : "", result.detail);
    return result;
}
} // namespace satori
