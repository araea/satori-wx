// WeChat "拍一戳" (pat) for the Satori backend.
//
// The app's own flow (double-tap on an avatar) is two steps, both reachable by reflection:
//   1. nv3.l.nj(talker, fromUser, pattedUser, template, createTimeSec, svrId) inserts the local
//      interaction row (type 922746929, an appmsg-62 wrapper around <patMsg>) and returns an
//      android.util.Pair(msgId, createTime) pointing at the record; it returns (0, 0) when the
//      talker or the target is not pattable.
//   2. qv3.b (NetSceneSendPat) builds cgi /cgi-bin/micromsg-bin/sendpat from that pair plus the
//      two usernames; its constructor resolves the pointer string ("<uin>_<msgId>_<createTime>")
//      itself. The scene is dispatched exactly like the sender's: doScene(dispatcher, no-op).
// The app's own type-849 listener (registered on the central scene runner) never sees the scene
// when it is dispatched directly, so no local row updates and no failure toast happens; the
// server's svrId for the record stays 0, which is cosmetic. Incoming pats are decoded from the
// database by wx_message.cpp (appmsg subtype 62).
#include "wx_pat.h"
#include "wx_live.h"
#include "wx_send.h"
#include "wx_store.h"
#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

namespace satori {
namespace {
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
bool g_ready = false;
jclass g_mgr = nullptr, g_scene = nullptr, g_pair = nullptr;
jmethodID g_nj = nullptr, g_scene_ctor = nullptr, g_scene_do = nullptr, g_long_value = nullptr;
jfieldID g_pair_first = nullptr;

void Detail(char *out, size_t size, const char *format, ...) {
    if (!out || !size) return;
    va_list args;
    va_start(args, format);
    vsnprintf(out, size, format, args);
    va_end(args);
}

jmethodID Method(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    if (!cls) return nullptr;
    jmethodID id = env->GetMethodID(cls, name, signature);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return id;
}
jfieldID Field(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    if (!cls) return nullptr;
    jfieldID id = env->GetFieldID(cls, name, signature);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return id;
}

// Resolves the pat classes once; every reference is promoted to a global. A failure clears the
// half-built cache so a later call can retry from scratch.
bool Resolve(JNIEnv *env, char *detail, size_t size) {
    if (g_ready) return true;
    char reason[160] = {};
    if (!ReflectResolve(reason, sizeof(reason))) {
        Detail(detail, size, "%s", reason);
        return false;
    }
    auto load = [](const char *name) { return static_cast<jclass>(ReflectLoad(name)); };
    jclass mgr = load("nv3.l");
    jclass scene = load("qv3.b");
    jclass pair = load("android.util.Pair");
    jclass boxed = load("java.lang.Long");
    bool ok = mgr && scene && pair && boxed;
    if (ok) {
        // (talker, fromUser, pattedUser, template, createTimeSec, svrId) -> Pair(msgId, createTime)
        g_nj =
            Method(env, mgr, "nj",
                   "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;IJ)Landroid/util/Pair;");
        g_scene_ctor = Method(env, scene, "<init>", "(Landroid/util/Pair;Ljava/lang/String;Ljava/lang/String;I)V");
        g_scene_do = Method(env, scene, "doScene", "(Lcom/tencent/mm/network/s;Lcom/tencent/mm/modelbase/u0;)I");
        g_long_value = Method(env, boxed, "longValue", "()J");
        g_pair_first = Field(env, pair, "first", "Ljava/lang/Object;");
        ok = g_nj && g_scene_ctor && g_scene_do && g_long_value && g_pair_first;
    }
    if (!ok) {
        Detail(detail, size, "pat classes not found (version mismatch?)");
        if (mgr) env->DeleteLocalRef(mgr);
        if (scene) env->DeleteLocalRef(scene);
        if (pair) env->DeleteLocalRef(pair);
        if (boxed) env->DeleteLocalRef(boxed);
        return false;
    }
    g_mgr = static_cast<jclass>(env->NewGlobalRef(mgr));
    g_scene = static_cast<jclass>(env->NewGlobalRef(scene));
    g_pair = static_cast<jclass>(env->NewGlobalRef(pair));
    env->DeleteLocalRef(mgr);
    env->DeleteLocalRef(scene);
    env->DeleteLocalRef(pair);
    env->DeleteLocalRef(boxed);
    if (!g_mgr || !g_scene || !g_pair) {
        Detail(detail, size, "global reference allocation failed");
        return false;
    }
    g_ready = true;
    return true;
}

// The pat manager instance: the app's service locator keyed by the marker interface ov3.j.
jobject Manager(JNIEnv *env) {
    jclass marker = static_cast<jclass>(ReflectLoad("ov3.j"));
    if (!marker) return nullptr;
    jclass locator = static_cast<jclass>(ReflectLoad("ph5.n0"));
    if (!locator) {
        env->DeleteLocalRef(marker);
        return nullptr;
    }
    jmethodID get = env->GetStaticMethodID(locator, "c", "(Ljava/lang/Class;)Lph5/m;");
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        get = nullptr;
    }
    jobject manager = get ? env->CallStaticObjectMethod(locator, get, marker) : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        manager = nullptr;
    }
    env->DeleteLocalRef(locator);
    env->DeleteLocalRef(marker);
    return manager;
}
} // namespace

ActionResult PatSend(const char *channel, const char *user) {
    ActionResult result{};
    if (!channel || !*channel || !user || !*user) {
        Detail(result.detail, sizeof(result.detail), "empty channel or user");
        return result;
    }
    JNIEnv *env = static_cast<JNIEnv *>(ReflectEnv());
    if (!env) {
        Detail(result.detail, sizeof(result.detail), "JavaVM unavailable");
        return result;
    }
    pthread_mutex_lock(&g_mu);
    const bool ready = Resolve(env, result.detail, sizeof(result.detail));
    pthread_mutex_unlock(&g_mu);
    if (!ready) return result;

    Store *store = LiveStore();
    const char *self = store ? StoreSelfId(store) : "";
    if (!self || !*self) {
        Detail(result.detail, sizeof(result.detail), "account identity unavailable");
        return result;
    }

    jobject manager = Manager(env);
    if (!manager) {
        Detail(result.detail, sizeof(result.detail), "pat manager unavailable");
        return result;
    }

    // The visible tip text of the local row; the server's own record template uses the same
    // "${wxid}" placeholders, so the row reads like one the app itself would have inserted.
    char template_text[300];
    snprintf(template_text, sizeof(template_text), "\"${%s}\" \xE6\x8B\x8D\xE4\xBA\x86\xE6\x8B\x8D \"${%s}\"", self,
             user);
    jstring talker = env->NewStringUTF(channel);
    jstring from = env->NewStringUTF(self);
    jstring patted = env->NewStringUTF(user);
    jstring tip = env->NewStringUTF(template_text);
    jobject pair = nullptr;
    if (talker && from && patted && tip) {
        pair = env->CallObjectMethod(manager, g_nj, talker, from, patted, tip, static_cast<jint>(time(nullptr)),
                                     static_cast<jlong>(0));
        if (env->ExceptionCheck()) {
            env->ExceptionClear();
            pair = nullptr;
        }
    } else {
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (talker) env->DeleteLocalRef(talker);
    if (from) env->DeleteLocalRef(from);
    if (patted) env->DeleteLocalRef(patted);
    if (tip) env->DeleteLocalRef(tip);
    if (!pair) {
        env->DeleteLocalRef(manager);
        Detail(result.detail, sizeof(result.detail), "pat row insert failed");
        return result;
    }

    // nj returns Pair(0, 0) when the talker or the patted user cannot be patted; refuse before
    // dispatching anything instead of trusting the server to say no.
    jobject boxed = pair ? env->GetObjectField(pair, g_pair_first) : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        boxed = nullptr;
    }
    jlong msg_id = boxed ? env->CallLongMethod(boxed, g_long_value) : 0;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        msg_id = 0;
    }
    if (boxed) env->DeleteLocalRef(boxed);
    if (msg_id <= 0) {
        env->DeleteLocalRef(pair);
        env->DeleteLocalRef(manager);
        result.rejected = true;
        Detail(result.detail, sizeof(result.detail), "cannot pat this talker or user");
        return result;
    }

    jstring chat = env->NewStringUTF(channel);
    jstring target = env->NewStringUTF(user);
    // scene int 0 = the plain pat (1 is the "edit pat suffix" flow in the app).
    jobject scene =
        (chat && target) ? env->NewObject(g_scene, g_scene_ctor, pair, chat, target, static_cast<jint>(0)) : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        scene = nullptr;
    }
    if (chat) env->DeleteLocalRef(chat);
    if (target) env->DeleteLocalRef(target);
    env->DeleteLocalRef(pair);
    env->DeleteLocalRef(manager);
    if (!scene) {
        Detail(result.detail, sizeof(result.detail), "sendpat scene construction failed");
        return result;
    }
    const int net =
        ReflectDispatchScene(scene, reinterpret_cast<void *>(g_scene_do), result.detail, sizeof(result.detail));
    env->DeleteLocalRef(scene);
    result.ok = net >= 0;
    if (result.ok)
        __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "pat %s -> %s dispatched", channel, user);
    else
        __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "pat %s -> %s failed: %s", channel, user, result.detail);
    return result;
}

bool PatDispatch(const char *channel, const char *user, bool *rejected, char *detail, size_t size) {
    ActionResult result = PatSend(channel, user);
    if (rejected) *rejected = result.rejected;
    if (detail && size) snprintf(detail, size, "%s", result.detail);
    return result.ok;
}
} // namespace satori
