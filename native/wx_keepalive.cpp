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
// An automatic (module-driven) hold can never outlive a wedged send by more than this.
constexpr long long kAutoTimeoutMs = 180 * 1000;
// Notification accent roles, matching satori-qq: brand / readable amber / Material error.
constexpr int kColorOnline = 0xFF5D438B;
constexpr int kColorWait = 0xFF775A0B;
constexpr int kColorDegraded = 0xFFBA1A1A;
constexpr const char *kChannel = "satori-wx-status";
// Used only if the primary channel cannot be (re)created, e.g. a ROM that keeps a deleted id blocked.
constexpr const char *kChannelFallback = "satori-wx-status-2";
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

// The user toggle defaults to on: the module exists to keep the service responsive, so the
// untimed CPU + Wi-Fi hold is armed from the first keeper tick. The button and
// POST /v1/internal/wakelock turn it off for the current boot.
volatile bool g_want_lock = true;
volatile bool g_lock_held = false;
// Nesting depth of module-driven holds around outbound work; a user hold is independent.
volatile int g_auto_depth = 0;
// Hold the Wi-Fi radio for as long as a Satori client is attached, independent of the CPU hold.
volatile bool g_sustain_wifi = false;
volatile bool g_cpu_held = false;
volatile bool g_wifi_held = false;
bool g_untimed = false; // g_mu: the CPU lock we currently hold was taken without a timeout
long long g_serving_since_ms = 0; // when online && listening most recently became true
bool g_was_serving = false;
volatile bool g_notify_ok = false;
volatile bool g_channel_ok = false;
// The channel notifications are posted to; EnsureChannel() may switch it to the fallback id.
const char *g_active_channel = kChannel;
volatile bool g_notify_enabled = false;
volatile long long g_reposts = 0;
char g_notify_detail[48] = "init";
char g_service_detail[64] = "init";
long long g_kick_ms = 0;
char g_last_key[480] = {};
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
    jmethodID nm_create = nullptr, nm_notify = nullptr, nm_enabled = nullptr, nm_active = nullptr,
              nm_get = nullptr;
    jmethodID builder_ctor = nullptr, b_icon = nullptr, b_title = nullptr, b_text = nullptr,
              b_style = nullptr, b_ongoing = nullptr, b_alert = nullptr, b_when = nullptr,
              b_category = nullptr, b_content_intent = nullptr, b_action = nullptr, b_build = nullptr,
              b_color = nullptr;
    jmethodID big_ctor = nullptr, big_set = nullptr;
    jmethodID pi_activity = nullptr, pi_broadcast = nullptr;
    jmethodID intent_ctor = nullptr, intent_component = nullptr, intent_put_int = nullptr,
              intent_put_string = nullptr, intent_put_bool = nullptr;
    jmethodID component_ctor = nullptr;
    jmethodID channel_ctor = nullptr, ch_desc = nullptr, ch_badge = nullptr;
    jmethodID pm_new_lock = nullptr, lock_acquire = nullptr, lock_release = nullptr,
              lock_held = nullptr, lock_refcount = nullptr, lock_acquire_timeout = nullptr;
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

// Every speculative lookup may fail on another framework version and leaves a pending
// NoSuchMethod/NoSuchFieldError. ART aborts the process when the next lookup throws on top
// of one, so each helper drops the pending exception immediately. The caller only sees a
// null id (or class), which the `complete` checks below turn into "notification disabled".
jclass Class(JNIEnv *env, const char *name) {
    jclass cls = env->FindClass(name);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return cls;
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

jfieldID StaticField(JNIEnv *env, jclass cls, const char *name, const char *signature) {
    if (!cls) return nullptr;
    jfieldID id = env->GetStaticFieldID(cls, name, signature);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return id;
}

// Creates (or re-creates) the low-importance, silent status channel. Idempotent, and called
// before every post: WeChat or ColorOS can delete the channel out from under us, and a
// notification posted to a missing channel is rejected by the system (“No Channel found”)
// without throwing — the keeper would then re-post every tick and the entry would never show.
// If the primary id cannot be resurrected, the fallback id is used. Returns whether a usable
// channel now exists and records its id in g_active_channel.
bool EnsureChannel(JNIEnv *env) {
    g_channel_ok = false;
    if (!g_j.cls_channel || !g_j.channel_ctor || !g_j.nm || !g_j.nm_create) return false;
    const char *const ids[] = {kChannel, kChannelFallback};
    for (const char *id : ids) {
        g_active_channel = id;
        jobject channel = env->NewObject(g_j.cls_channel, g_j.channel_ctor, String(env, id),
                                         String(env, kChannelName), kImportanceLow);
        if (!channel) { Clear(env); continue; }
        // The description and badge are cosmetic; a missing setter must not block channel creation.
        if (g_j.ch_desc) env->CallVoidMethod(channel, g_j.ch_desc, String(env, "知言 Satori 服务的运行状态"));
        if (g_j.ch_badge) env->CallVoidMethod(channel, g_j.ch_badge, JNI_FALSE);
        Clear(env);
        env->CallVoidMethod(g_j.nm, g_j.nm_create, channel);
        Clear(env);
        env->DeleteLocalRef(channel);
        // getNotificationChannel() returns null for a missing or deleted channel: verify, because
        // createNotificationChannel() silently ignores a channel whose id a ROM refuses to undelete.
        if (!g_j.nm_get) { g_channel_ok = true; return true; }
        jobject existing = env->CallObjectMethod(g_j.nm, g_j.nm_get, String(env, id));
        Clear(env);
        if (existing) { env->DeleteLocalRef(existing); g_channel_ok = true; return true; }
    }
    g_active_channel = kChannel;
    return false;
}

// Builds the class cache. Requires a PushLocalFrame; the kept objects are global refs.
bool ResolveJava(JNIEnv *env) {
    jclass activity_thread = Class(env, "android/app/ActivityThread");
    if (!activity_thread) return false;
    jmethodID current = StaticMethod(env, activity_thread, "currentApplication", "()Landroid/app/Application;");
    jobject application = current ? env->CallStaticObjectMethod(activity_thread, current) : nullptr;
    Clear(env);
    if (!application) return false;
    g_j.started = true;

    g_j.cls_context = static_cast<jclass>(env->NewGlobalRef(Class(env, "android/content/Context")));
    if (!g_j.cls_context) return false;
    jclass app_info_class = Class(env, "android/content/pm/ApplicationInfo");
    jclass package_manager = Class(env, "android/content/pm/PackageManager");
    g_j.cls_pm = static_cast<jclass>(env->NewGlobalRef(Class(env, "android/os/PowerManager")));
    g_j.cls_nm = static_cast<jclass>(env->NewGlobalRef(Class(env, "android/app/NotificationManager")));
    g_j.cls_builder = static_cast<jclass>(env->NewGlobalRef(Class(env, "android/app/Notification$Builder")));
    g_j.cls_big = static_cast<jclass>(env->NewGlobalRef(Class(env, "android/app/Notification$BigTextStyle")));
    g_j.cls_pi = static_cast<jclass>(env->NewGlobalRef(Class(env, "android/app/PendingIntent")));
    g_j.cls_intent = static_cast<jclass>(env->NewGlobalRef(Class(env, "android/content/Intent")));
    g_j.cls_component = static_cast<jclass>(env->NewGlobalRef(Class(env, "android/content/ComponentName")));
    g_j.cls_channel = static_cast<jclass>(env->NewGlobalRef(Class(env, "android/app/NotificationChannel")));
    g_j.cls_sbn = static_cast<jclass>(env->NewGlobalRef(Class(env, "android/service/notification/StatusBarNotification")));

    const bool resolved = g_j.cls_pm && g_j.cls_nm && g_j.cls_builder && g_j.cls_big && g_j.cls_pi &&
                          g_j.cls_intent && g_j.cls_component && g_j.cls_channel && g_j.cls_sbn &&
                          app_info_class && package_manager;
    if (!resolved) return false;

    g_j.get_service = Method(env, g_j.cls_context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    g_j.get_package_name = Method(env, g_j.cls_context, "getPackageName", "()Ljava/lang/String;");
    g_j.get_app_context = Method(env, g_j.cls_context, "getApplicationContext", "()Landroid/content/Context;");
    g_j.get_app_info = Method(env, g_j.cls_context, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    g_j.get_package_manager = Method(env, g_j.cls_context, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    g_j.start_service = Method(env, g_j.cls_context, "startService", "(Landroid/content/Intent;)Landroid/content/ComponentName;");
    g_j.app_icon = Field(env, app_info_class, "icon", "I");
    g_j.launch_intent = Method(env, package_manager, "getLaunchIntentForPackage", "(Ljava/lang/String;)Landroid/content/Intent;");

    g_j.nm_create = Method(env, g_j.cls_nm, "createNotificationChannel", "(Landroid/app/NotificationChannel;)V");
    g_j.nm_get = Method(env, g_j.cls_nm, "getNotificationChannel", "(Ljava/lang/String;)Landroid/app/NotificationChannel;");
    g_j.nm_notify = Method(env, g_j.cls_nm, "notify", "(ILandroid/app/Notification;)V");
    g_j.nm_enabled = Method(env, g_j.cls_nm, "areNotificationsEnabled", "()Z");
    g_j.nm_active = Method(env, g_j.cls_nm, "getActiveNotifications", "()[Landroid/service/notification/StatusBarNotification;");
    g_j.sbn_id = Method(env, g_j.cls_sbn, "getId", "()I");

    // The chain signatures embed the full Builder descriptor (34 chars), so the buffer has
    // to hold the longest one (addAction: 88 chars) plus the terminator. Truncating it makes
    // GetMethodID fail and, before the helpers above, took the whole process down.
    const char *builder = "Landroid/app/Notification$Builder;";
    char chain[160];
    snprintf(chain, sizeof(chain), "(I)%s", builder);
    g_j.b_icon = Method(env, g_j.cls_builder, "setSmallIcon", chain);
    snprintf(chain, sizeof(chain), "(Ljava/lang/CharSequence;)%s", builder);
    g_j.b_title = Method(env, g_j.cls_builder, "setContentTitle", chain);
    g_j.b_text = Method(env, g_j.cls_builder, "setContentText", chain);
    snprintf(chain, sizeof(chain), "(Landroid/app/Notification$Style;)%s", builder);
    g_j.b_style = Method(env, g_j.cls_builder, "setStyle", chain);
    snprintf(chain, sizeof(chain), "(Z)%s", builder);
    g_j.b_ongoing = Method(env, g_j.cls_builder, "setOngoing", chain);
    g_j.b_alert = Method(env, g_j.cls_builder, "setOnlyAlertOnce", chain);
    g_j.b_when = Method(env, g_j.cls_builder, "setShowWhen", chain);
    snprintf(chain, sizeof(chain), "(Ljava/lang/String;)%s", builder);
    g_j.b_category = Method(env, g_j.cls_builder, "setCategory", chain);
    snprintf(chain, sizeof(chain), "(I)%s", builder);
    g_j.b_color = Method(env, g_j.cls_builder, "setColor", chain);
    snprintf(chain, sizeof(chain), "(Landroid/app/PendingIntent;)%s", builder);
    g_j.b_content_intent = Method(env, g_j.cls_builder, "setContentIntent", chain);
    snprintf(chain, sizeof(chain), "(ILjava/lang/CharSequence;Landroid/app/PendingIntent;)%s", builder);
    g_j.b_action = Method(env, g_j.cls_builder, "addAction", chain);
    g_j.b_build = Method(env, g_j.cls_builder, "build", "()Landroid/app/Notification;");

    g_j.builder_ctor = Method(env, g_j.cls_builder, "<init>", "(Landroid/content/Context;Ljava/lang/String;)V");
    g_j.big_ctor = Method(env, g_j.cls_big, "<init>", "()V");
    g_j.big_set = Method(env, g_j.cls_big, "setBigText",
                         "(Ljava/lang/CharSequence;)Landroid/app/Notification$BigTextStyle;");
    if (!g_j.big_set)
        g_j.big_set = Method(env, g_j.cls_big, "bigText",
                             "(Ljava/lang/CharSequence;)Landroid/app/Notification$BigTextStyle;");

    const char *pi_sig = "(Landroid/content/Context;ILandroid/content/Intent;I)Landroid/app/PendingIntent;";
    g_j.pi_activity = StaticMethod(env, g_j.cls_pi, "getActivity", pi_sig);
    g_j.pi_broadcast = StaticMethod(env, g_j.cls_pi, "getBroadcast", pi_sig);
    g_j.intent_ctor = Method(env, g_j.cls_intent, "<init>", "()V");
    g_j.intent_component = Method(env, g_j.cls_intent, "setComponent",
                                  "(Landroid/content/ComponentName;)Landroid/content/Intent;");
    g_j.intent_put_int = Method(env, g_j.cls_intent, "putExtra", "(Ljava/lang/String;I)Landroid/content/Intent;");
    g_j.intent_put_string = Method(env, g_j.cls_intent, "putExtra", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;");
    g_j.intent_put_bool = Method(env, g_j.cls_intent, "putExtra", "(Ljava/lang/String;Z)Landroid/content/Intent;");
    g_j.component_ctor = Method(env, g_j.cls_component, "<init>", "(Ljava/lang/String;Ljava/lang/String;)V");
    g_j.channel_ctor = Method(env, g_j.cls_channel, "<init>", "(Ljava/lang/String;Ljava/lang/CharSequence;I)V");
    g_j.ch_desc = Method(env, g_j.cls_channel, "setDescription", "(Ljava/lang/String;)V");
    g_j.ch_badge = Method(env, g_j.cls_channel, "setShowBadge", "(Z)V");

    if (!g_j.get_app_context || !g_j.get_service) return false;
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
    g_j.pm_new_lock = Method(env, power_class, "newWakeLock", "(ILjava/lang/String;)Landroid/os/PowerManager$WakeLock;");
    g_j.wifi_new_lock = Method(env, wifi_class, "createWifiLock", "(ILjava/lang/String;)Landroid/net/wifi/WifiManager$WifiLock;");
    Clear(env);
    jobject lock = g_j.pm_new_lock ? env->CallObjectMethod(power, g_j.pm_new_lock, kPartialWakeLock, String(env, kLockTag)) : nullptr;
    jobject wifi_lock = g_j.wifi_new_lock ? env->CallObjectMethod(wifi, g_j.wifi_new_lock, kWifiFullHighPerf, String(env, kLockTag)) : nullptr;
    Clear(env);
    if (lock) {
        jclass lock_class = env->GetObjectClass(lock);
        g_j.lock_acquire = Method(env, lock_class, "acquire", "()V");
        g_j.lock_acquire_timeout = Method(env, lock_class, "acquire", "(J)V");
        g_j.lock_release = Method(env, lock_class, "release", "()V");
        g_j.lock_held = Method(env, lock_class, "isHeld", "()Z");
        g_j.lock_refcount = Method(env, lock_class, "setReferenceCounted", "(Z)V");
        if (g_j.lock_refcount) { env->CallVoidMethod(lock, g_j.lock_refcount, JNI_FALSE); Clear(env); }
        g_j.lock = env->NewGlobalRef(lock);
        env->DeleteLocalRef(lock_class);
    }
    if (wifi_lock) {
        jclass wlock_class = env->GetObjectClass(wifi_lock);
        g_j.wlock_acquire = Method(env, wlock_class, "acquire", "()V");
        g_j.wlock_release = Method(env, wlock_class, "release", "()V");
        g_j.wlock_held = Method(env, wlock_class, "isHeld", "()Z");
        g_j.wlock_refcount = Method(env, wlock_class, "setReferenceCounted", "(Z)V");
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
    jobject app_info = g_j.get_app_info ? env->CallObjectMethod(g_j.context, g_j.get_app_info) : nullptr;
    Clear(env);
    if (app_info && g_j.app_icon) {
        g_j.icon = env->GetIntField(app_info, g_j.app_icon);
        Clear(env);
    }
    if (app_info) env->DeleteLocalRef(app_info);
    if (!g_j.icon) {
        jclass drawable = Class(env, "android/R$drawable");
        if (drawable) {
            jfieldID field = StaticField(env, drawable, "ic_dialog_info", "I");
            if (!field) field = StaticField(env, drawable, "stat_sys_download", "I");
            if (field) { g_j.icon = env->GetStaticIntField(drawable, field); Clear(env); }
            env->DeleteLocalRef(drawable);
        }
    }

    // One low-importance, silent channel. Re-created on every start and again before each post.
    EnsureChannel(env);

    // Only mark ready when every method called unconditionally below is present; a null
    // method ID would be a hard crash, not a Java exception.
    const bool complete = g_j.lock && g_j.wifi_lock && g_j.builder_ctor && g_j.b_icon && g_j.b_title &&
                          g_j.b_text && g_j.b_style && g_j.b_ongoing && g_j.b_alert && g_j.b_when &&
                          g_j.b_category && g_j.b_build && g_j.big_ctor && g_j.big_set && g_j.nm_notify;
    g_j.ok = complete;
    return complete;
}

bool IsHeld(JNIEnv *env, jobject lock, jmethodID method) {
    if (!lock || !method) return false;
    const jboolean held = env->CallBooleanMethod(lock, method);
    Clear(env);
    return held == JNI_TRUE;
}

// Brings the OS locks in line with the two reasons we might want them: the user toggle and any
// automatic hold around outbound work. Caller holds g_mu and g_j.ok.
void ApplyLock(JNIEnv *env) {
    const bool want_cpu = g_want_lock || g_auto_depth > 0;
    if (want_cpu) {
        if (g_want_lock) {
            // A user hold is untimed. PowerManager only cancels the pending timeout releaser on
            // release(), so promoting a timed lock has to release first; otherwise it expires.
            if (IsHeld(env, g_j.lock, g_j.lock_held) && !g_untimed && g_j.lock_release) {
                env->CallVoidMethod(g_j.lock, g_j.lock_release);
                Clear(env);
            }
            if (!IsHeld(env, g_j.lock, g_j.lock_held) && g_j.lock_acquire) env->CallVoidMethod(g_j.lock, g_j.lock_acquire);
            g_untimed = true;
        } else if (g_j.lock_acquire_timeout) {
            // An automatic hold expires on its own if a send wedges; re-arms on every nested hold.
            env->CallVoidMethod(g_j.lock, g_j.lock_acquire_timeout, static_cast<jlong>(kAutoTimeoutMs));
            g_untimed = false;
        } else if (!IsHeld(env, g_j.lock, g_j.lock_held) && g_j.lock_acquire) {
            env->CallVoidMethod(g_j.lock, g_j.lock_acquire);
            g_untimed = true;
        }
        Clear(env);
    } else if (IsHeld(env, g_j.lock, g_j.lock_held) && g_j.lock_release) {
        env->CallVoidMethod(g_j.lock, g_j.lock_release);
        Clear(env);
        g_untimed = false;
    }
    // The Wi-Fi radio outlives a single send: inbound events queue behind screen-off power save
    // just as badly as an upload fails behind it.
    const bool want_wifi = want_cpu || g_sustain_wifi;
    const bool wifi_held = IsHeld(env, g_j.wifi_lock, g_j.wlock_held);
    if (want_wifi && !wifi_held && g_j.wlock_acquire) env->CallVoidMethod(g_j.wifi_lock, g_j.wlock_acquire);
    else if (!want_wifi && wifi_held && g_j.wlock_release) env->CallVoidMethod(g_j.wifi_lock, g_j.wlock_release);
    Clear(env);
    g_cpu_held = IsHeld(env, g_j.lock, g_j.lock_held);
    g_wifi_held = IsHeld(env, g_j.wifi_lock, g_j.wlock_held);
    g_lock_held = g_cpu_held || g_wifi_held;
}

// Periodic restart of WeChat's own core service: a started service in the main process keeps
// it at SERVICE_ADJ, above the freezer cutoff, without any hook.
void Kick(JNIEnv *env) {
    if (!g_j.start_service || !g_j.context || !g_j.cls_intent || !g_j.intent_ctor ||
        !g_j.cls_component || !g_j.component_ctor || !g_j.intent_component) {
        snprintf(g_service_detail, sizeof(g_service_detail), "unavailable");
        return;
    }
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

// Minute resolution: the notification is only redrawn when its text changes, so a per-second
// figure would repost it every refresh. Under a minute reads as "just came online".
void FormatUptime(long long ms, char *out, size_t size) {
    if (ms < 0) ms = 0;
    const long long minutes = ms / 60000, hours = minutes / 60, days = hours / 24;
    if (days > 0) snprintf(out, size, "已在线 %lld 天 %lld 小时", days, hours % 24);
    else if (hours > 0) snprintf(out, size, "已在线 %lld 小时 %lld 分", hours, minutes % 60);
    else if (minutes > 0) snprintf(out, size, "已在线 %lld 分", minutes);
    else snprintf(out, size, "刚刚上线");
}

// Renders the resident entry from live state and returns its accent color. The online/listening
// split and the attached-client count mirror satori-qq's StatusNotice. The collapsed row is the
// title plus `text`; `big` (the expanded body) repeats `text` and adds the uptime, and never the
// title again. Nothing else belongs here: the wake-lock state is the button's own label, and
// sending is always on, so there is no state to report for either.
int RenderState(long long serving_ms, char *title, size_t title_size, char *text, size_t text_size, char *big, size_t big_size) {
    const bool online = g_login_count > 0;
    const bool listening = g_server_ready;
    const int clients = g_client_count > 0 ? g_client_count : 0;
    int color = kColorDegraded;
    if (online && listening) {
        color = kColorOnline;
        snprintf(title, title_size, "知言 · 运行中");
        if (clients == 0) snprintf(text, text_size, "等待客户端连接 · 端口 %u", g_port);
        else snprintf(text, text_size, "已连接 %d 个客户端 · 端口 %u", clients, g_port);
    } else if (!online) {
        color = kColorWait;
        snprintf(title, title_size, "知言 · 等待登录");
        if (listening) snprintf(text, text_size, "打开微信登录，即可连接服务");
        else snprintf(text, text_size, "等待微信登录与本地服务启动");
    } else {
        snprintf(title, title_size, "知言 · 服务异常");
        snprintf(text, text_size, "本地端口 %u 未监听", g_port);
    }
    snprintf(big, big_size, "%s", text);
    if (online && listening && serving_ms > 0) {
        char uptime[64];
        FormatUptime(serving_ms, uptime, sizeof(uptime));
        const size_t used = strlen(big);
        if (used < big_size) snprintf(big + used, big_size - used, "\n%s", uptime);
    }
    return color;
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

    char title[96], text[160], big[320], key[480];
    const int color = RenderState(g_serving_since_ms > 0 ? NowMs() - g_serving_since_ms : 0, title, sizeof(title),
                                  text, sizeof(text), big, sizeof(big));
    // Everything the entry shows: the collapsed row, the expanded body (uptime), the button label.
    snprintf(key, sizeof(key), "%s|%s|%d|%d", title, big, g_login_count, g_want_lock ? 1 : 0);
    const bool changed = strcmp(key, g_last_key) != 0;
    const long long now = NowMs();
    const bool alive = changed || StillPosted(env);
    if (!changed && (!enabled || (now - g_last_post_ms < kRepostGapMs) || alive)) return;
    if (changed && !enabled) { snprintf(g_notify_detail, sizeof(g_notify_detail), "disabled"); return; }

    // The channel can disappear (WeChat or ColorOS deleting it): recreate it right before posting.
    EnsureChannel(env);
    jobject builder = env->NewObject(g_j.cls_builder, g_j.builder_ctor, g_j.context, String(env, g_active_channel));
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
    if (g_j.b_color) env->CallObjectMethod(builder, g_j.b_color, color);
    Clear(env);

    // Tap opens WeChat; the launcher intent already carries FLAG_ACTIVITY_NEW_TASK.
    if (g_j.launch_intent && g_j.get_package_manager && g_j.pi_activity && g_j.b_content_intent) {
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
    if (g_j.pi_broadcast && g_j.intent_ctor && g_j.intent_component && g_j.intent_put_int &&
        g_j.intent_put_string && g_j.intent_put_bool && g_j.component_ctor && g_j.b_action) {
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
                    // Derive the Wi-Fi sustain from live state: a client is actually attached.
                    const bool serving = g_server_ready && g_login_count > 0;
                    if (serving && !g_was_serving) g_serving_since_ms = NowMs();
                    if (!serving) g_serving_since_ms = 0;
                    g_was_serving = serving;
                    g_sustain_wifi = serving && g_client_count > 0;
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

int KeepaliveRender(long long serving_ms, char *title, size_t title_size, char *text, size_t text_size,
                    char *big, size_t big_size) {
    return RenderState(serving_ms, title, title_size, text, text_size, big, big_size);
}

void KeepaliveStart(void *vm, const Config &config) {
    if (g_started || !vm) return;
    g_started = true;
    g_vm = static_cast<JavaVM *>(vm);
    g_port = config.port;
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

// Ref-counted automatic hold around one outbound mutation. A wedged send cannot hold the CPU
// past kAutoTimeoutMs; nested calls share the one underlying lock. No notification redraw:
// the user intent did not change, only the transient CPU/Wi-Fi state.
void KeepaliveWakelockBegin() {
    pthread_mutex_lock(&g_mu);
    g_auto_depth = g_auto_depth + 1;
    JNIEnv *env = g_vm ? Env() : nullptr;
    if (env && g_j.ok && env->PushLocalFrame(32) == 0) { ApplyLock(env); env->PopLocalFrame(nullptr); }
    pthread_mutex_unlock(&g_mu);
}

void KeepaliveWakelockEnd() {
    pthread_mutex_lock(&g_mu);
    if (g_auto_depth > 0) g_auto_depth = g_auto_depth - 1;
    JNIEnv *env = g_vm ? Env() : nullptr;
    if (env && g_j.ok && env->PushLocalFrame(32) == 0) { ApplyLock(env); env->PopLocalFrame(nullptr); }
    pthread_mutex_unlock(&g_mu);
}

void KeepaliveStatus(cJSON *object) {
    if (!object) return;
    cJSON *keep = cJSON_AddObjectToObject(object, "keepalive");
    if (!keep) return;
    cJSON_AddBoolToObject(keep, "notification", g_notify_ok);
    cJSON_AddBoolToObject(keep, "notifications_enabled", g_notify_enabled);
    cJSON_AddBoolToObject(keep, "channel", g_channel_ok);
    cJSON_AddBoolToObject(keep, "wakelock", g_want_lock);
    cJSON_AddBoolToObject(keep, "wakelock_held", g_lock_held);
    cJSON_AddBoolToObject(keep, "user", g_want_lock);
    cJSON_AddNumberToObject(keep, "auto", g_auto_depth);
    cJSON_AddBoolToObject(keep, "cpu_held", g_cpu_held);
    cJSON_AddBoolToObject(keep, "wifi_held", g_wifi_held);
    cJSON_AddBoolToObject(keep, "sustain_wifi", g_sustain_wifi);
    cJSON_AddNumberToObject(keep, "clients", g_client_count);
    cJSON_AddNumberToObject(keep, "uptime_ms", static_cast<double>(g_serving_since_ms > 0 ? NowMs() - g_serving_since_ms : 0));
    cJSON_AddStringToObject(keep, "service", g_service_detail);
    cJSON_AddStringToObject(keep, "notify", g_notify_detail);
    cJSON_AddNumberToObject(keep, "reposts", static_cast<double>(g_reposts));
    cJSON_AddNumberToObject(keep, "oom_score_adj", static_cast<double>(ReadLong("/proc/self/oom_score_adj", -1)));
    char wchan[48];
    ReadText("/proc/self/wchan", wchan, sizeof(wchan));
    cJSON_AddStringToObject(keep, "wchan", wchan);
}
} // namespace satori
