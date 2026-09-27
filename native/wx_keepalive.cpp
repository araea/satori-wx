// In-process residency: resident status notification, wake lock and a periodic restart of
// WeChat's own core service. See wx_keepalive.h for the contract.
//
// Everything is plain JNI on public framework classes. No dex is embedded, no class is
// defined at runtime and no ArtMethod is touched, so nothing here loads code into WeChat.
#include "wx_keepalive.h"
#include "server.h"
#include "protocol.h"
#include "jni_helpers.h"
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <unistd.h>
#include <android/log.h>

namespace satori {
namespace {
// Framework constants. These are stable public API values; kept as literals so the hot path
// does not depend on extra reflection.
constexpr int kPartialWakeLock = 0x00000001;   // PowerManager.PARTIAL_WAKE_LOCK
constexpr int kWifiFullHighPerf = 3;           // WifiManager.WIFI_MODE_FULL_HIGH_PERF
constexpr int kImportanceLow = 2;              // NotificationManager.IMPORTANCE_LOW
constexpr int kFlagUpdateCurrent = 0x08000000; // PendingIntent.FLAG_UPDATE_CURRENT
constexpr int kFlagImmutable = 0x04000000;     // PendingIntent.FLAG_IMMUTABLE
constexpr int kNotifyId = 0x5A710001;
constexpr int kReqOpen = 0x5A710003;
constexpr int kReqToggle = 0x5A710004;
constexpr long long kKickIntervalMs = 10 * 60 * 1000;
constexpr long long kTickIntervalMs = 3000;
constexpr long long kRepostGapMs = 3000;
constexpr const char *kChannel = "satori-wx-status";
constexpr const char *kChannelName = "知言服务状态";
constexpr const char *kLockTag = "satori-wx:wakelock";
constexpr const char *kHostPackage = "com.tencent.mm";
constexpr const char *kHostService = "com.tencent.mm.booter.CoreService";
constexpr const char *kTogglerPackage = "com.satori.wx";
constexpr const char *kTogglerReceiver = "com.satori.wx.keepalive.WakeToggleReceiver";

JavaVM *g_vm = nullptr;
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER;
bool g_started = false;
char g_token[129] = {};
unsigned g_port = 5601;
bool g_send = false;

volatile bool g_want_lock = false;
volatile bool g_lock_held = false;
volatile bool g_notify_ok = false;
volatile bool g_notify_enabled = false;
volatile long long g_reposts = 0;
char g_notify_detail[48] = "init";
char g_service_detail[64] = "init";
long long g_kick_ms = 0;
char g_last_key[320] = {};
long long g_last_post_ms = 0;

long long NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

long long ReadLong(const char *path, long long fallback) {
    char buffer[64];
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return fallback;
    const ssize_t n = read(fd, buffer, sizeof(buffer) - 1);
    close(fd);
    if (n <= 0) return fallback;
    buffer[n] = 0;
    char *end = nullptr;
    const long long value = strtoll(buffer, &end, 10);
    return end == buffer ? fallback : value;
}

void ReadText(const char *path, char *out, size_t size) {
    if (!size) return;
    out[0] = 0;
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return;
    const ssize_t n = read(fd, out, size - 1);
    close(fd);
    if (n <= 0) { out[0] = 0; return; }
    out[n] = 0;
    for (ssize_t i = 0; i < n; ++i)
        if (out[i] == '\n' || out[i] == '\r') { out[i] = 0; break; }
}

// Everything resolved from the host application once. Method IDs stay valid for framework
// classes; the few objects we keep (context, managers, locks) are global references.
struct Java {
    bool ok = false;
    // Set once currentApplication() has returned: a later resolution failure must not retry,
    // or every retry would leak another set of global references.
    bool started = false;
    jobject context = nullptr;   // Application
    jobject power = nullptr;     // PowerManager
    jobject wifi = nullptr;      // WifiManager
    jobject nm = nullptr;        // NotificationManager
    jobject lock = nullptr;      // PowerManager.WakeLock
    jobject wifi_lock = nullptr; // WifiManager.WifiLock
    jclass cls_context = nullptr, cls_pm = nullptr, cls_nm = nullptr, cls_builder = nullptr,
           cls_big = nullptr, cls_pi = nullptr, cls_intent = nullptr, cls_component = nullptr,
           cls_channel = nullptr, cls_sbn = nullptr;
    jmethodID get_service = nullptr, get_package_name = nullptr, get_app_context = nullptr,
              get_app_info = nullptr, get_package_manager = nullptr, start_service = nullptr;
    jfieldID app_icon = nullptr;
    jmethodID launch_intent = nullptr;
    jmethodID nm_create = nullptr, nm_notify = nullptr, nm_enabled = nullptr, nm_active = nullptr;
    jmethodID builder_ctor = nullptr, b_icon = nullptr, b_title = nullptr, b_text = nullptr,
              b_style = nullptr, b_ongoing = nullptr, b_alert = nullptr, b_when = nullptr,
              b_category = nullptr, b_content_intent = nullptr, b_action = nullptr, b_build = nullptr;
    jmethodID big_ctor = nullptr, big_set = nullptr;
    jmethodID pi_activity = nullptr, pi_broadcast = nullptr;
    jmethodID intent_ctor = nullptr, intent_component = nullptr, intent_put_int = nullptr,
              intent_put_string = nullptr, intent_put_bool = nullptr;
    jmethodID component_ctor = nullptr;
    jmethodID channel_ctor = nullptr, ch_desc = nullptr, ch_badge = nullptr;
    jmethodID pm_new_lock = nullptr, lock_acquire = nullptr, lock_release = nullptr,
              lock_held = nullptr, lock_refcount = nullptr;
    jmethodID wifi_new_lock = nullptr, wlock_acquire = nullptr, wlock_release = nullptr,
              wlock_held = nullptr, wlock_refcount = nullptr;
    jmethodID sbn_id = nullptr;
    jint icon = 0;
};
Java g_j;

jstring String(JNIEnv *env, const char *text) { return env->NewStringUTF(text ? text : ""); }

void Clear(JNIEnv *env) {
    if (env->ExceptionCheck()) env->ExceptionClear();
}

// Builds the class cache. Requires a PushLocalFrame; the kept objects are global refs.
bool ResolveJava(JNIEnv *env) {
    jclass activity_thread = env->FindClass("android/app/ActivityThread");
    if (!activity_thread) { Clear(env); return false; }
    jmethodID current = env->GetStaticMethodID(activity_thread, "currentApplication", "()Landroid/app/Application;");
    jobject application = current ? env->CallStaticObjectMethod(activity_thread, current) : nullptr;
    Clear(env);
    if (!application) return false;
    g_j.started = true;

    g_j.cls_context = static_cast<jclass>(env->NewGlobalRef(env->FindClass("android/content/Context")));
    Clear(env);
    if (!g_j.cls_context) return false;
    jclass app_info_class = env->FindClass("android/content/pm/ApplicationInfo");
    jclass package_manager = env->FindClass("android/content/pm/PackageManager");
    g_j.cls_pm = static_cast<jclass>(env->NewGlobalRef(env->FindClass("android/os/PowerManager")));
    g_j.cls_nm = static_cast<jclass>(env->NewGlobalRef(env->FindClass("android/app/NotificationManager")));
    g_j.cls_builder = static_cast<jclass>(env->NewGlobalRef(env->FindClass("android/app/Notification$Builder")));
    g_j.cls_big = static_cast<jclass>(env->NewGlobalRef(env->FindClass("android/app/Notification$BigTextStyle")));
    g_j.cls_pi = static_cast<jclass>(env->NewGlobalRef(env->FindClass("android/app/PendingIntent")));
    g_j.cls_intent = static_cast<jclass>(env->NewGlobalRef(env->FindClass("android/content/Intent")));
    g_j.cls_component = static_cast<jclass>(env->NewGlobalRef(env->FindClass("android/content/ComponentName")));
    g_j.cls_channel = static_cast<jclass>(env->NewGlobalRef(env->FindClass("android/app/NotificationChannel")));
    g_j.cls_sbn = static_cast<jclass>(env->NewGlobalRef(env->FindClass("android/service/notification/StatusBarNotification")));
    Clear(env);

    const bool resolved = g_j.cls_pm && g_j.cls_nm && g_j.cls_builder && g_j.cls_big && g_j.cls_pi &&
                          g_j.cls_intent && g_j.cls_component && g_j.cls_channel && g_j.cls_sbn &&
                          app_info_class && package_manager;
    if (!resolved) return false;

    g_j.get_service = env->GetMethodID(g_j.cls_context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    g_j.get_package_name = env->GetMethodID(g_j.cls_context, "getPackageName", "()Ljava/lang/String;");
    g_j.get_app_context = env->GetMethodID(g_j.cls_context, "getApplicationContext", "()Landroid/content/Context;");
    g_j.get_app_info = env->GetMethodID(g_j.cls_context, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    g_j.get_package_manager = env->GetMethodID(g_j.cls_context, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    g_j.start_service = env->GetMethodID(g_j.cls_context, "startService", "(Landroid/content/Intent;)Landroid/content/ComponentName;");
    g_j.app_icon = env->GetFieldID(app_info_class, "icon", "I");
    g_j.launch_intent = env->GetMethodID(package_manager, "getLaunchIntentForPackage", "(Ljava/lang/String;)Landroid/content/Intent;");

    g_j.nm_create = env->GetMethodID(g_j.cls_nm, "createNotificationChannel", "(Landroid/app/NotificationChannel;)V");
    g_j.nm_notify = env->GetMethodID(g_j.cls_nm, "notify", "(ILandroid/app/Notification;)V");
    g_j.nm_enabled = env->GetMethodID(g_j.cls_nm, "areNotificationsEnabled", "()Z");
    g_j.nm_active = env->GetMethodID(g_j.cls_nm, "getActiveNotifications", "()[Landroid/service/notification/StatusBarNotification;");
    g_j.sbn_id = env->GetMethodID(g_j.cls_sbn, "getId", "()I");

    const char *builder = "Landroid/app/Notification$Builder;";
    char chain[64];
    snprintf(chain, sizeof(chain), "(I)%s", builder);
    g_j.b_icon = env->GetMethodID(g_j.cls_builder, "setSmallIcon", chain);
    snprintf(chain, sizeof(chain), "(Ljava/lang/CharSequence;)%s", builder);
    g_j.b_title = env->GetMethodID(g_j.cls_builder, "setContentTitle", chain);
    g_j.b_text = env->GetMethodID(g_j.cls_builder, "setContentText", chain);
    snprintf(chain, sizeof(chain), "(Landroid/app/Notification$Style;)%s", builder);
    g_j.b_style = env->GetMethodID(g_j.cls_builder, "setStyle", chain);
    snprintf(chain, sizeof(chain), "(Z)%s", builder);
    g_j.b_ongoing = env->GetMethodID(g_j.cls_builder, "setOngoing", chain);
    g_j.b_alert = env->GetMethodID(g_j.cls_builder, "setOnlyAlertOnce", chain);
    g_j.b_when = env->GetMethodID(g_j.cls_builder, "setShowWhen", chain);
    snprintf(chain, sizeof(chain), "(Ljava/lang/String;)%s", builder);
    g_j.b_category = env->GetMethodID(g_j.cls_builder, "setCategory", chain);
    snprintf(chain, sizeof(chain), "(Landroid/app/PendingIntent;)%s", builder);
    g_j.b_content_intent = env->GetMethodID(g_j.cls_builder, "setContentIntent", chain);
    snprintf(chain, sizeof(chain), "(ILjava/lang/CharSequence;Landroid/app/PendingIntent;)%s", builder);
    g_j.b_action = env->GetMethodID(g_j.cls_builder, "addAction", chain);
    g_j.b_build = env->GetMethodID(g_j.cls_builder, "build", "()Landroid/app/Notification;");

    g_j.builder_ctor = env->GetMethodID(g_j.cls_builder, "<init>", "(Landroid/content/Context;Ljava/lang/String;)V");
    g_j.big_ctor = env->GetMethodID(g_j.cls_big, "<init>", "()V");
    g_j.big_set = env->GetMethodID(g_j.cls_big, "setBigText",
                                   "(Ljava/lang/CharSequence;)Landroid/app/Notification$BigTextStyle;");
    if (!g_j.big_set) {
        Clear(env);
        g_j.big_set = env->GetMethodID(g_j.cls_big, "bigText",
                                       "(Ljava/lang/CharSequence;)Landroid/app/Notification$BigTextStyle;");
    }

    const char *pi_sig = "(Landroid/content/Context;ILandroid/content/Intent;I)Landroid/app/PendingIntent;";
    g_j.pi_activity = env->GetStaticMethodID(g_j.cls_pi, "getActivity", pi_sig);
    g_j.pi_broadcast = env->GetStaticMethodID(g_j.cls_pi, "getBroadcast", pi_sig);
    g_j.intent_ctor = env->GetMethodID(g_j.cls_intent, "<init>", "()V");
    g_j.intent_component = env->GetMethodID(g_j.cls_intent, "setComponent",
                                            "(Landroid/content/ComponentName;)Landroid/content/Intent;");
    g_j.intent_put_int = env->GetMethodID(g_j.cls_intent, "putExtra", "(Ljava/lang/String;I)Landroid/content/Intent;");
    g_j.intent_put_string = env->GetMethodID(g_j.cls_intent, "putExtra", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;");
    g_j.intent_put_bool = env->GetMethodID(g_j.cls_intent, "putExtra", "(Ljava/lang/String;Z)Landroid/content/Intent;");
    g_j.component_ctor = env->GetMethodID(g_j.cls_component, "<init>", "(Ljava/lang/String;Ljava/lang/String;)V");
    g_j.channel_ctor = env->GetMethodID(g_j.cls_channel, "<init>", "(Ljava/lang/String;Ljava/lang/CharSequence;I)V");
    g_j.ch_desc = env->GetMethodID(g_j.cls_channel, "setDescription", "(Ljava/lang/String;)V");
    g_j.ch_badge = env->GetMethodID(g_j.cls_channel, "setShowBadge", "(Z)V");
    Clear(env);

    jobject app_context = env->CallObjectMethod(application, g_j.get_app_context);
    Clear(env);
    if (!app_context) app_context = application;
    g_j.context = env->NewGlobalRef(app_context);
    if (app_context != application) env->DeleteLocalRef(app_context);
    env->DeleteLocalRef(application);
    if (!g_j.context) return false;

    jobject power = env->CallObjectMethod(g_j.context, g_j.get_service, String(env, "power"));
    jobject wifi = env->CallObjectMethod(g_j.context, g_j.get_service, String(env, "wifi"));
    jobject nm = env->CallObjectMethod(g_j.context, g_j.get_service, String(env, "notification"));
    Clear(env);
    if (!power || !wifi || !nm) {
        if (power) env->DeleteLocalRef(power);
        if (wifi) env->DeleteLocalRef(wifi);
        if (nm) env->DeleteLocalRef(nm);
        return false;
    }
    g_j.power = env->NewGlobalRef(power);
    g_j.wifi = env->NewGlobalRef(wifi);
    g_j.nm = env->NewGlobalRef(nm);

    jclass power_class = env->GetObjectClass(power);
    jclass wifi_class = env->GetObjectClass(wifi);
    g_j.pm_new_lock = env->GetMethodID(power_class, "newWakeLock", "(ILjava/lang/String;)Landroid/os/PowerManager$WakeLock;");
    g_j.wifi_new_lock = env->GetMethodID(wifi_class, "createWifiLock", "(ILjava/lang/String;)Landroid/net/wifi/WifiManager$WifiLock;");
    Clear(env);
    jobject lock = g_j.pm_new_lock ? env->CallObjectMethod(power, g_j.pm_new_lock, kPartialWakeLock, String(env, kLockTag)) : nullptr;
    jobject wifi_lock = g_j.wifi_new_lock ? env->CallObjectMethod(wifi, g_j.wifi_new_lock, kWifiFullHighPerf, String(env, kLockTag)) : nullptr;
    Clear(env);
    if (lock) {
        jclass lock_class = env->GetObjectClass(lock);
        g_j.lock_acquire = env->GetMethodID(lock_class, "acquire", "()V");
        g_j.lock_release = env->GetMethodID(lock_class, "release", "()V");
        g_j.lock_held = env->GetMethodID(lock_class, "isHeld", "()Z");
        g_j.lock_refcount = env->GetMethodID(lock_class, "setReferenceCounted", "(Z)V");
        if (g_j.lock_refcount) { env->CallVoidMethod(lock, g_j.lock_refcount, JNI_FALSE); Clear(env); }
        g_j.lock = env->NewGlobalRef(lock);
        env->DeleteLocalRef(lock_class);
    }
    if (wifi_lock) {
        jclass wlock_class = env->GetObjectClass(wifi_lock);
        g_j.wlock_acquire = env->GetMethodID(wlock_class, "acquire", "()V");
        g_j.wlock_release = env->GetMethodID(wlock_class, "release", "()V");
        g_j.wlock_held = env->GetMethodID(wlock_class, "isHeld", "()Z");
        g_j.wlock_refcount = env->GetMethodID(wlock_class, "setReferenceCounted", "(Z)V");
        if (g_j.wlock_refcount) { env->CallVoidMethod(wifi_lock, g_j.wlock_refcount, JNI_FALSE); Clear(env); }
        g_j.wifi_lock = env->NewGlobalRef(wifi_lock);
        env->DeleteLocalRef(wlock_class);
    }
    env->DeleteLocalRef(power_class);
    env->DeleteLocalRef(wifi_class);
    env->DeleteLocalRef(power);
    env->DeleteLocalRef(wifi);
    env->DeleteLocalRef(nm);

    // The host's own launcher icon, resolved once; the fallback is a system drawable so the
    // notification is always valid even if the icon lookup fails.
    jobject app_info = env->CallObjectMethod(g_j.context, g_j.get_app_info);
    Clear(env);
    if (app_info && g_j.app_icon) {
        g_j.icon = env->GetIntField(app_info, g_j.app_icon);
        Clear(env);
    }
    if (app_info) env->DeleteLocalRef(app_info);
    if (!g_j.icon) {
        jclass drawable = env->FindClass("android/R$drawable");
        if (drawable) {
            jfieldID field = env->GetStaticFieldID(drawable, "ic_dialog_info", "I");
            if (!field) { Clear(env); field = env->GetStaticFieldID(drawable, "stat_sys_download", "I"); }
            if (field) { g_j.icon = env->GetStaticIntField(drawable, field); Clear(env); }
            env->DeleteLocalRef(drawable);
        } else {
            Clear(env);
        }
    }

    // One low-importance, silent channel. Re-created on every start: it is idempotent.
    jobject channel = env->NewObject(g_j.cls_channel, g_j.channel_ctor, String(env, kChannel),
                                     String(env, kChannelName), kImportanceLow);
    Clear(env);
    if (channel) {
        env->CallVoidMethod(channel, g_j.ch_desc, String(env, "知言 Satori 服务的运行状态"));
        env->CallVoidMethod(channel, g_j.ch_badge, JNI_FALSE);
        Clear(env);
        env->CallVoidMethod(g_j.nm, g_j.nm_create, channel);
        Clear(env);
        env->DeleteLocalRef(channel);
    }

    const bool complete = g_j.lock && g_j.wifi_lock && g_j.builder_ctor && g_j.b_icon && g_j.b_title &&
                          g_j.b_text && g_j.b_build && g_j.big_ctor && g_j.big_set && g_j.pi_activity &&
                          g_j.pi_broadcast && g_j.intent_ctor && g_j.intent_component && g_j.component_ctor;
    g_j.ok = complete;
    return complete;
}

bool IsHeld(JNIEnv *env, jobject lock, jmethodID method) {
    if (!lock || !method) return false;
    const jboolean held = env->CallBooleanMethod(lock, method);
    Clear(env);
    return held == JNI_TRUE;
}

// Brings the OS locks in line with the user's intent. Caller holds g_mu and g_j.ok.
void ApplyLock(JNIEnv *env) {
    const bool want = g_want_lock;
    const bool cpu_held = IsHeld(env, g_j.lock, g_j.lock_held);
    if (want && !cpu_held && g_j.lock_acquire) env->CallVoidMethod(g_j.lock, g_j.lock_acquire);
    else if (!want && cpu_held && g_j.lock_release) env->CallVoidMethod(g_j.lock, g_j.lock_release);
    Clear(env);
    const bool wifi_held = IsHeld(env, g_j.wifi_lock, g_j.wlock_held);
    if (want && !wifi_held && g_j.wlock_acquire) env->CallVoidMethod(g_j.wifi_lock, g_j.wlock_acquire);
    else if (!want && wifi_held && g_j.wlock_release) env->CallVoidMethod(g_j.wifi_lock, g_j.wlock_release);
    Clear(env);
    g_lock_held = IsHeld(env, g_j.lock, g_j.lock_held) || IsHeld(env, g_j.wifi_lock, g_j.wlock_held);
}

// Periodic restart of WeChat's own core service: a started service in the main process keeps
// it at SERVICE_ADJ, above the freezer cutoff, without any hook.
void Kick(JNIEnv *env) {
    if (!g_j.start_service || !g_j.context || !g_j.intent_ctor) return;
    jobject intent = env->NewObject(g_j.cls_intent, g_j.intent_ctor);
    if (!intent) { Clear(env); snprintf(g_service_detail, sizeof(g_service_detail), "intent-failed"); return; }
    jobject component = env->NewObject(g_j.cls_component, g_j.component_ctor,
                                       String(env, kHostPackage), String(env, kHostService));
    if (!component) { Clear(env); env->DeleteLocalRef(intent); snprintf(g_service_detail, sizeof(g_service_detail), "component-failed"); return; }
    env->CallObjectMethod(intent, g_j.intent_component, component);
    Clear(env);
    env->CallObjectMethod(g_j.context, g_j.start_service, intent);
    if (env->ExceptionCheck()) {
        Clear(env);
        snprintf(g_service_detail, sizeof(g_service_detail), "blocked");
    } else {
        snprintf(g_service_detail, sizeof(g_service_detail), "ok");
    }
    g_kick_ms = NowMs();
    env->DeleteLocalRef(component);
    env->DeleteLocalRef(intent);
}

void RenderState(char *title, size_t title_size, char *text, size_t text_size, char *big, size_t big_size) {
    if (!g_server_ready) {
        snprintf(title, title_size, "知言 · 服务未启动");
        snprintf(text, text_size, "端口 %u 没有监听，请检查配置", g_port);
    } else if (g_login_count <= 0) {
        snprintf(title, title_size, "知言 · 等待登录");
        snprintf(text, text_size, "服务在 127.0.0.1:%u，等待微信登录", g_port);
    } else {
        snprintf(title, title_size, "知言 · 运行中");
        snprintf(text, text_size, "已登录 · 127.0.0.1:%u · %s", g_port, g_send ? "发送已开启" : "只收不发");
    }
    snprintf(big, big_size, "%s\n%s\n唤醒锁：%s", title, text, g_want_lock ? "已开启" : "已关闭");
}

bool StillPosted(JNIEnv *env) {
    if (!g_j.nm_active || !g_j.nm || !g_j.sbn_id) return true;
    jobjectArray active = static_cast<jobjectArray>(env->CallObjectMethod(g_j.nm, g_j.nm_active));
    if (env->ExceptionCheck()) { Clear(env); return true; }
    if (!active) return true;
    bool found = false;
    const jsize count = env->GetArrayLength(active);
    for (jsize i = 0; i < count; ++i) {
        jobject sbn = env->GetObjectArrayElement(active, i);
        if (!sbn) continue;
        const jint id = env->CallIntMethod(sbn, g_j.sbn_id);
        Clear(env);
        env->DeleteLocalRef(sbn);
        if (id == kNotifyId) { found = true; break; }
    }
    env->DeleteLocalRef(active);
    return found;
}

// Posts or refreshes the resident entry. Caller holds g_mu and g_j.ok.
void Notify(JNIEnv *env) {
    if (!g_j.builder_ctor) return;
    bool enabled = true;
    if (g_j.nm_enabled) {
        enabled = env->CallBooleanMethod(g_j.nm, g_j.nm_enabled) == JNI_TRUE;
        Clear(env);
    }
    g_notify_enabled = enabled;

    char title[96], text[160], big[320], key[320];
    RenderState(title, sizeof(title), text, sizeof(text), big, sizeof(big));
    snprintf(key, sizeof(key), "%s|%s|%d|%d|%d", title, text, g_login_count, g_want_lock ? 1 : 0, g_send ? 1 : 0);
    const bool changed = strcmp(key, g_last_key) != 0;
    const long long now = NowMs();
    const bool alive = changed || StillPosted(env);
    if (!changed && (!enabled || (now - g_last_post_ms < kRepostGapMs) || alive)) return;
    if (changed && !enabled) { snprintf(g_notify_detail, sizeof(g_notify_detail), "disabled"); return; }

    jobject builder = env->NewObject(g_j.cls_builder, g_j.builder_ctor, g_j.context, String(env, kChannel));
    if (!builder) {
        Clear(env);
        snprintf(g_notify_detail, sizeof(g_notify_detail), "builder-failed");
        return;
    }
    jobject jtitle = String(env, title), jtext = String(env, text);
    env->CallObjectMethod(builder, g_j.b_icon, g_j.icon);
    env->CallObjectMethod(builder, g_j.b_title, jtitle);
    env->CallObjectMethod(builder, g_j.b_text, jtext);
    jobject style = env->NewObject(g_j.cls_big, g_j.big_ctor);
    if (style) {
        env->CallObjectMethod(style, g_j.big_set, String(env, big));
        env->CallObjectMethod(builder, g_j.b_style, style);
    }
    env->CallObjectMethod(builder, g_j.b_ongoing, JNI_TRUE);
    env->CallObjectMethod(builder, g_j.b_alert, JNI_TRUE);
    env->CallObjectMethod(builder, g_j.b_when, JNI_FALSE);
    env->CallObjectMethod(builder, g_j.b_category, String(env, "service"));
    Clear(env);

    // Tap opens WeChat; the launcher intent already carries FLAG_ACTIVITY_NEW_TASK.
    if (g_j.launch_intent && g_j.get_package_manager && g_j.pi_activity) {
        jobject manager = env->CallObjectMethod(g_j.context, g_j.get_package_manager);
        jobject launch = manager ? env->CallObjectMethod(manager, g_j.launch_intent, String(env, kHostPackage)) : nullptr;
        Clear(env);
        if (manager) env->DeleteLocalRef(manager);
        if (launch) {
            jobject open = env->CallStaticObjectMethod(g_j.cls_pi, g_j.pi_activity, g_j.context, kReqOpen, launch,
                                                       kFlagUpdateCurrent | kFlagImmutable);
            Clear(env);
            if (open) { env->CallObjectMethod(builder, g_j.b_content_intent, open); Clear(env); env->DeleteLocalRef(open); }
            env->DeleteLocalRef(launch);
        }
    }

    // Action button: a broadcast to the companion app, which forwards the toggle to
    // POST /v1/internal/wakelock. The module cannot host a receiver without loading code.
    if (g_j.pi_broadcast && g_j.intent_ctor && g_j.component_ctor && g_j.b_action) {
        jobject action = env->NewObject(g_j.cls_intent, g_j.intent_ctor);
        jobject component = env->NewObject(g_j.cls_component, g_j.component_ctor,
                                           String(env, kTogglerPackage), String(env, kTogglerReceiver));
        if (action && component) {
            env->CallObjectMethod(action, g_j.intent_component, component);
            env->CallObjectMethod(action, g_j.intent_put_int, String(env, "port"), static_cast<jint>(g_port));
            env->CallObjectMethod(action, g_j.intent_put_string, String(env, "token"), String(env, g_token));
            env->CallObjectMethod(action, g_j.intent_put_bool, String(env, "on"), g_want_lock ? JNI_FALSE : JNI_TRUE);
            Clear(env);
            jobject toggle = env->CallStaticObjectMethod(g_j.cls_pi, g_j.pi_broadcast, g_j.context, kReqToggle, action,
                                                         kFlagUpdateCurrent | kFlagImmutable);
            Clear(env);
            if (toggle) {
                env->CallObjectMethod(builder, g_j.b_action, g_j.icon,
                                      String(env, g_want_lock ? "释放唤醒锁" : "获取唤醒锁"), toggle);
                Clear(env);
                env->DeleteLocalRef(toggle);
            }
        }
        if (action) env->DeleteLocalRef(action);
        if (component) env->DeleteLocalRef(component);
    }

    jobject notification = env->CallObjectMethod(builder, g_j.b_build);
    if (!notification) {
        Clear(env);
        snprintf(g_notify_detail, sizeof(g_notify_detail), "build-failed");
    } else {
        env->CallVoidMethod(g_j.nm, g_j.nm_notify, kNotifyId, notification);
        if (env->ExceptionCheck()) {
            Clear(env);
            snprintf(g_notify_detail, sizeof(g_notify_detail), "notify-failed");
            g_notify_ok = false;
        } else {
            snprintf(g_notify_detail, sizeof(g_notify_detail), "posted");
            g_notify_ok = true;
            if (!changed) g_reposts = g_reposts + 1;
            snprintf(g_last_key, sizeof(g_last_key), "%s", key);
            g_last_post_ms = now;
        }
        env->DeleteLocalRef(notification);
    }
    env->DeleteLocalRef(style);
    env->DeleteLocalRef(jtitle);
    env->DeleteLocalRef(jtext);
    env->DeleteLocalRef(builder);
}

JNIEnv *Env() {
    JavaVM *vm = g_vm;
    if (!vm) return nullptr;
    JNIEnv *env = nullptr;
    if (vm->GetEnv(reinterpret_cast<void **>(&env), JNI_VERSION_1_6) == JNI_OK) return env;
    if (vm->AttachCurrentThreadAsDaemon(&env, nullptr) != JNI_OK) return nullptr;
    return env;
}

void *Manager(void *) {
    pthread_setname_np(pthread_self(), "satori-wx-keep");
    // The application object is not ready at postAppSpecialize; poll for it.
    for (int i = 0; i < 2400; ++i) {
        JNIEnv *env = Env();
        if (env) {
            pthread_mutex_lock(&g_mu);
            if (!g_j.ok && !g_j.started) { if (env->PushLocalFrame(256) == 0) { ResolveJava(env); env->PopLocalFrame(nullptr); } }
            const bool ready = g_j.ok;
            const bool started = g_j.started; // resolution was attempted; do not retry and leak
            pthread_mutex_unlock(&g_mu);
            if (ready || started) break;
        }
        const timespec delay{0, 500000000};
        nanosleep(&delay, nullptr);
    }
    if (!g_j.ok) {
        snprintf(g_notify_detail, sizeof(g_notify_detail), "unavailable");
        __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "keepalive: application context never became usable; notification disabled");
        return nullptr;
    }
    __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "keepalive: resident notification armed");
    for (;;) {
        JNIEnv *env = Env();
        if (env) {
            pthread_mutex_lock(&g_mu);
            if (g_j.ok) {
                if (env->PushLocalFrame(96) == 0) {
                    ApplyLock(env);
                    if (NowMs() - g_kick_ms >= kKickIntervalMs) Kick(env);
                    Notify(env);
                    env->PopLocalFrame(nullptr);
                }
            }
            pthread_mutex_unlock(&g_mu);
        }
        const timespec delay{kTickIntervalMs / 1000, (kTickIntervalMs % 1000) * 1000000};
        nanosleep(&delay, nullptr);
    }
}
} // namespace

void KeepaliveStart(void *vm, const Config &config) {
    if (g_started || !vm) return;
    g_started = true;
    g_vm = static_cast<JavaVM *>(vm);
    g_port = config.port;
    g_send = config.send;
    snprintf(g_token, sizeof(g_token), "%s", config.token);
    pthread_t thread;
    if (pthread_create(&thread, nullptr, Manager, nullptr)) {
        __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "keepalive thread creation failed");
        return;
    }
    pthread_detach(thread);
}

void KeepaliveWakelock(int action, bool *held) {
    JavaVM *vm = g_vm;
    if (held) *held = g_want_lock;
    if (!vm) return;
    JNIEnv *env = Env();
    if (!env) return;
    pthread_mutex_lock(&g_mu);
    if (action == 1) g_want_lock = true;
    else if (action == 0) g_want_lock = false;
    else g_want_lock = !g_want_lock;
    if (g_j.ok && g_j.builder_ctor && env->PushLocalFrame(128) == 0) {
        ApplyLock(env);
        Notify(env);
        env->PopLocalFrame(nullptr);
    }
    if (held) *held = g_want_lock;
    pthread_mutex_unlock(&g_mu);
}

void KeepaliveStatus(cJSON *object) {
    if (!object) return;
    cJSON *keep = cJSON_AddObjectToObject(object, "keepalive");
    if (!keep) return;
    cJSON_AddBoolToObject(keep, "notification", g_notify_ok);
    cJSON_AddBoolToObject(keep, "notifications_enabled", g_notify_enabled);
    cJSON_AddBoolToObject(keep, "wakelock", g_want_lock);
    cJSON_AddBoolToObject(keep, "wakelock_held", g_lock_held);
    cJSON_AddStringToObject(keep, "service", g_service_detail);
    cJSON_AddStringToObject(keep, "notify", g_notify_detail);
    cJSON_AddNumberToObject(keep, "reposts", static_cast<double>(g_reposts));
    cJSON_AddNumberToObject(keep, "oom_score_adj", static_cast<double>(ReadLong("/proc/self/oom_score_adj", -1)));
    char wchan[48];
    ReadText("/proc/self/wchan", wchan, sizeof(wchan));
    cJSON_AddStringToObject(keep, "wchan", wchan);
}
} // namespace satori
