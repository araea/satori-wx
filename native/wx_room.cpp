// Group-management write actions for the Satori backend.
//
// Everything here goes through WeChat's own request builders resolved on the host class
// loader (see wx_send.h's Reflect* helpers) and dispatched by WeChat's own network queue. In the 8.0.78
// names (the names themselves are in wx_names.h and differ per build):
//   kick / leave : qn.p  (cgi /cgi-bin/micromsg-bin/delchatroommember), doScene(dispatcher, cb)
//   add admin    : qn.b  (cgi /cgi-bin/micromsg-bin/addchatroomadmin), Cgi runner via z2.d
//   del admin    : qn.e  (cgi /cgi-bin/micromsg-bin/delchatroomadmin), Cgi runner via z2.d
// qn.b/qn.e extend com.tencent.mm.modelbase.i (the newer "Cgi" base) instead of m1, so their
// request object is handed to com.tencent.mm.modelbase.z2.d(o, null, false) which dispatches
// it exactly like the app does. No hook, no patch and no dex.
#include "wx_room.h"
#include "wx_send.h"
#include "wx_names.h"
#include <android/log.h>
#include <jni.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

namespace satori {
namespace {
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
bool g_ready = false;
jclass g_p = nullptr, g_b = nullptr, g_e = nullptr, g_z2 = nullptr, g_linked = nullptr;
jmethodID g_p_ctor = nullptr, g_p_do = nullptr;
jmethodID g_b_ctor = nullptr, g_e_ctor = nullptr;
jmethodID g_z2_d = nullptr;
jmethodID g_linked_ctor = nullptr, g_list_add = nullptr;
jfieldID g_i_f = nullptr; // com.tencent.mm.modelbase.i.f : the request object

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

// Resolves the room scenes once, against the first set of WeChat names (wx_names.h) under which every
// class and member exists. Every class reference is promoted to a global so it stays valid across the JNI
// calls that follow; a failure clears the half-built cache.
bool ResolveWith(JNIEnv *env, const WxNames &n) {
    auto load = [](const char *name) { return static_cast<jclass>(ReflectLoad(name)); };
    jclass p = load(n.value[kRoomKickCls]), b = load(n.value[kRoomAdminAddCls]), e = load(n.value[kRoomAdminDelCls]);
    jclass z2 = load(n.value[kRoomRunnerCls]);
    jclass i = load(n.value[kRoomCgiBaseCls]);
    jclass linked = load("java.util.LinkedList");
    bool ok = p && b && e && z2 && i && linked;
    jmethodID p_ctor = nullptr, p_do = nullptr, b_ctor = nullptr, e_ctor = nullptr, z2_d = nullptr,
              linked_ctor = nullptr, list_add = nullptr;
    jfieldID i_f = nullptr;
    if (ok) {
        p_ctor = Method(env, p, "<init>", "(Ljava/lang/String;Ljava/util/List;I)V");
        p_do = Method(env, p, "doScene", n.value[kDoSceneDesc]);
        b_ctor = Method(env, b, "<init>", "(Ljava/lang/String;Ljava/util/LinkedList;)V");
        e_ctor = Method(env, e, "<init>", "(Ljava/lang/String;Ljava/util/LinkedList;)V");
        z2_d = StaticMethod(env, z2, n.value[kRoomRunnerName], n.value[kRoomRunnerDesc]);
        i_f = Field(env, i, n.value[kRoomCgiReqName], n.value[kRoomCgiReqDesc]);
        linked_ctor = Method(env, linked, "<init>", "()V");
        list_add = Method(env, linked, "add", "(Ljava/lang/Object;)Z");
        ok = p_ctor && p_do && b_ctor && e_ctor && z2_d && i_f && linked_ctor && list_add;
    }
    if (ok) {
        g_p = static_cast<jclass>(env->NewGlobalRef(p));
        g_b = static_cast<jclass>(env->NewGlobalRef(b));
        g_e = static_cast<jclass>(env->NewGlobalRef(e));
        g_z2 = static_cast<jclass>(env->NewGlobalRef(z2));
        g_linked = static_cast<jclass>(env->NewGlobalRef(linked));
        if (g_p && g_b && g_e && g_z2 && g_linked) {
            g_p_ctor = p_ctor;
            g_p_do = p_do;
            g_b_ctor = b_ctor;
            g_e_ctor = e_ctor;
            g_z2_d = z2_d;
            g_i_f = i_f;
            g_linked_ctor = linked_ctor;
            g_list_add = list_add;
        } else {
            ok = false;
        }
    }
    if (p) env->DeleteLocalRef(p);
    if (b) env->DeleteLocalRef(b);
    if (e) env->DeleteLocalRef(e);
    if (z2) env->DeleteLocalRef(z2);
    if (i) env->DeleteLocalRef(i);
    if (linked) env->DeleteLocalRef(linked);
    return ok;
}

bool Resolve(JNIEnv *env, char *detail, size_t size) {
    if (g_ready) return true;
    char reason[160] = {};
    if (!ReflectResolve(reason, sizeof(reason))) {
        Detail(detail, size, "%s", reason);
        return false;
    }
    for (int index = 0; index < WxNamesCount(); ++index) {
        if (ResolveWith(env, WxNamesAt(index))) {
            g_ready = true;
            return true;
        }
        // Drop whatever the attempt took before it failed, so the next set starts clean.
        jclass *held[] = {&g_p, &g_b, &g_e, &g_z2, &g_linked};
        for (jclass *slot : held) {
            if (*slot) env->DeleteGlobalRef(*slot);
            *slot = nullptr;
        }
    }
    Detail(detail, size, "room classes not found (version mismatch?)");
    return false;
}

jobject NewStringList(JNIEnv *env, const char *user) {
    jobject list = env->NewObject(g_linked, g_linked_ctor);
    if (!list) return nullptr;
    jstring text = env->NewStringUTF(user);
    if (!text) {
        env->DeleteLocalRef(list);
        return nullptr;
    }
    env->CallBooleanMethod(list, g_list_add, text);
    env->DeleteLocalRef(text);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(list);
        return nullptr;
    }
    return list;
}

// qn.b/qn.e request objects are dispatched by the Cgi runner; the request lives in the
// inherited `f` field after the constructor ran.
bool DispatchCgi(JNIEnv *env, jobject scene, ActionResult *result) {
    jobject request = env->GetObjectField(scene, g_i_f);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        request = nullptr;
    }
    if (!request) {
        Detail(result->detail, sizeof(result->detail), "cgi request unavailable");
        return false;
    }
    jobject dispatched = env->CallStaticObjectMethod(g_z2, g_z2_d, request, nullptr, JNI_FALSE);
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        env->DeleteLocalRef(request);
        Detail(result->detail, sizeof(result->detail), "cgi dispatch threw");
        return false;
    }
    env->DeleteLocalRef(request);
    if (!dispatched) {
        Detail(result->detail, sizeof(result->detail), "cgi dispatch rejected");
        return false;
    }
    env->DeleteLocalRef(dispatched);
    result->ok = true;
    return true;
}

bool Prepare(ActionResult *result, JNIEnv **env) {
    *env = static_cast<JNIEnv *>(ReflectEnv());
    if (!*env) {
        Detail(result->detail, sizeof(result->detail), "JavaVM unavailable");
        return false;
    }
    pthread_mutex_lock(&g_mu);
    const bool ok = g_ready || Resolve(*env, result->detail, sizeof(result->detail));
    pthread_mutex_unlock(&g_mu);
    return ok;
}

void Log(const char *action, const char *target, const ActionResult &result) {
    if (result.ok)
        __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "%s %s dispatched", action, target);
    else
        __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "%s %s failed: %s", action, target, result.detail);
}
} // namespace

ActionResult RoomRemoveMember(const char *chatroom, const char *user) {
    ActionResult result{};
    if (!chatroom || !*chatroom || !user || !*user) {
        Detail(result.detail, sizeof(result.detail), "empty chatroom or user");
        return result;
    }
    JNIEnv *env = nullptr;
    if (!Prepare(&result, &env)) return result;
    jobject list = NewStringList(env, user);
    jstring room = env->NewStringUTF(chatroom);
    jobject scene = (list && room) ? env->NewObject(g_p, g_p_ctor, room, list, static_cast<jint>(0)) : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        scene = nullptr;
    }
    if (room) env->DeleteLocalRef(room);
    if (list) env->DeleteLocalRef(list);
    if (!scene) {
        Detail(result.detail, sizeof(result.detail), "delchatroommember scene construction failed");
        return result;
    }
    const int net = ReflectDispatchScene(scene, reinterpret_cast<void *>(g_p_do), result.detail, sizeof(result.detail));
    env->DeleteLocalRef(scene);
    result.ok = net >= 0;
    Log("room.remove", chatroom, result);
    return result;
}

ActionResult RoomSetAdmin(const char *chatroom, const char *user, bool enable) {
    ActionResult result{};
    if (!chatroom || !*chatroom || !user || !*user) {
        Detail(result.detail, sizeof(result.detail), "empty chatroom or user");
        return result;
    }
    JNIEnv *env = nullptr;
    if (!Prepare(&result, &env)) return result;
    jobject list = NewStringList(env, user);
    jstring room = env->NewStringUTF(chatroom);
    jobject scene =
        (list && room) ? env->NewObject(enable ? g_b : g_e, enable ? g_b_ctor : g_e_ctor, room, list) : nullptr;
    if (env->ExceptionCheck()) {
        env->ExceptionClear();
        scene = nullptr;
    }
    if (room) env->DeleteLocalRef(room);
    if (list) env->DeleteLocalRef(list);
    if (!scene) {
        Detail(result.detail, sizeof(result.detail), "chatroom admin scene construction failed");
        return result;
    }
    DispatchCgi(env, scene, &result);
    env->DeleteLocalRef(scene);
    Log(enable ? "room.admin.add" : "room.admin.del", chatroom, result);
    return result;
}
} // namespace satori
