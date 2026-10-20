// In-process residency: resident status notification, wake lock, a periodic restart of WeChat's
// own core service, and the published state file. See wx_keepalive.h for the contract.
//
// Everything is plain JNI on public framework classes. No dex is embedded, no class is
// defined at runtime and no ArtMethod is touched, so nothing here loads code into WeChat.
//
// Nothing polls. The keeper thread sleeps on a condition variable (CLOCK_MONOTONIC) and a pass runs
// only when a producer says a value changed (NotifyStateChanged), the heartbeat is due, or one of
// the keeper's own deadlines has come: a send probe retry, an automatic wake-lock hold expiring, or
// the one-off wait for the host Application to exist at startup. A pass is idempotent, so producers
// only ever poke; they never describe what changed.
#include "wx_keepalive.h"
#include "server.h"
#include "protocol.h"
#include "version.h"
#include "wx_send.h"
#include "jni_helpers.h"
#include <jni.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <fcntl.h>
#include <sys/stat.h>
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
constexpr int kNotifyIdBase = 0x5A710001;
constexpr int kReqOpen = 0x5A710003;
// The heartbeat: restart WeChat's core service, and check that the notification, its channel and
// the permission to show it are still there. There is no event for any of these going away.
constexpr long long kHeartbeatMs = 10 * 60 * 1000;
// An automatic (module-driven) hold can never outlive a wedged send by more than this.
constexpr long long kAutoTimeoutMs = 180 * 1000;
// A burst of changes (a login, the listener and a client arriving together) is one pass.
constexpr long long kSettleMs = 40;
constexpr long long kSettleCapMs = 250;
// The shortest wait the keeper ever takes on a deadline, so that nothing it schedules can spin it.
constexpr long long kMinWaitMs = 20;
// A status request may ask the keeper to verify the notification, at most this often.
constexpr long long kVerifyGapMs = 30 * 1000;
// Waiting for the host Application to exist (startup only): short at first, then every 3 seconds.
constexpr int kResolveMaxTries = 200;
// Send probe retries after a login while the sender is not ready yet; after the last, the
// heartbeat and status requests keep it honest.
constexpr long long kProbeDelaysMs[] = {0, 2500, 5000, 10000, 30000, 60000};
// Notification accent roles, matching satori-qq: brand / readable amber / Material error.
constexpr int kColorOnline = 0xFF5D438B;
constexpr int kColorWait = 0xFF775A0B;
constexpr int kColorDegraded = 0xFFBA1A1A;
constexpr const char *kChannelName = "知言服务状态";
constexpr const char *kHostPackage = "com.tencent.mm";
constexpr const char *kHostService = "com.tencent.mm.booter.CoreService";

JavaVM *g_vm = nullptr;
pthread_mutex_t g_mu = PTHREAD_MUTEX_INITIALIZER; // the Java side and the lock/notification state
bool g_started = false;
// The master switch, mirrored here so the keeper can cancel/repaint without asking the module.
volatile bool g_enabled = true;
char g_token[129] = {};
unsigned g_port = 5601;
int g_instance = 0;
int g_notify_id = kNotifyIdBase;
// Primary ids, and the ones used only if the primary channel cannot be (re)created, e.g. a ROM
// that keeps a deleted id blocked.
char g_channel[40] = "satori-wx-status";
char g_channel_fallback[40] = "satori-wx-status-2";
char g_lock_tag[48] = "satori-wx:wakelock";
char g_state_path[600] = {};
char g_state_tmp[620] = {};
char g_state_dir[560] = {};

// ---- The event hub: a generation counter and a condition variable. -------------------------------
pthread_mutex_t g_ev_mu = PTHREAD_MUTEX_INITIALIZER;
pthread_cond_t g_ev_cv;
unsigned long long g_ev_gen = 0;
volatile long long g_wakes = 0;       // passes run, for any reason
volatile long long g_event_wakes = 0; // ... of which because something changed

// The user toggle defaults to on: the module exists to keep the service responsive, so the
// untimed CPU + Wi-Fi hold is armed from the first pass. POST /v1/internal/wakelock (the
// module page in the root manager) turns it off for the current boot.
volatile bool g_want_lock = true;
volatile bool g_lock_held = false;
// Nesting depth of module-driven holds around outbound work; a user hold is independent.
volatile int g_auto_depth = 0;
long long g_auto_since_ms = 0; // g_mu: when the latest automatic hold began (it re-arms the timeout)
// Hold the Wi-Fi radio for as long as a Satori client is attached, independent of the CPU hold.
volatile bool g_sustain_wifi = false;
volatile bool g_cpu_held = false;
volatile bool g_wifi_held = false;
bool g_untimed = false;           // g_mu: the CPU lock we currently hold was taken without a timeout
long long g_serving_since_ms = 0; // monotonic: when online && listening most recently became true
long long g_serving_wall_ms = 0;  // the same instant on the wall clock (the chronometer's base)
bool g_was_serving = false;
bool g_was_online = false;
volatile bool g_notify_ok = false;
volatile bool g_channel_ok = false;
// The channel notifications are posted to; EnsureChannel() may switch it to the fallback id.
const char *g_active_channel = g_channel;
volatile bool g_notify_enabled = false;
volatile long long g_reposts = 0;
char g_notify_detail[48] = "init";
char g_service_detail[64] = "init";
long long g_kick_ms = 0;
long long g_next_beat_ms = 0;      // 0: due as soon as the Java side is ready
long long g_beat_ms = 0;           // when the heartbeat last ran
volatile bool g_verify = false;    // a status request asked for the notification to be verified
volatile bool g_probe_req = false; // ... or for the sender to be probed again (it is not ready)
volatile long long g_verify_asked_ms = 0;
char g_last_key[600] = {};
// Send probe scheduling (keeper thread only).
int g_probe_step = 0;
long long g_probe_next_ms = 0;
volatile int g_send_state = kSendUnknown;
// The Java side's bring-up (keeper thread only).
int g_resolve_tries = 0;
long long g_resolve_next_ms = 0;
bool g_resolve_gave_up = false;
// The published state file (keeper thread only, except the status block's reads).
char *g_last_state = nullptr;
char g_state_file[24] = "disabled";

long long NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

long long WallMs() {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
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
    if (n <= 0) {
        out[0] = 0;
        return;
    }
    out[n] = 0;
    for (ssize_t i = 0; i < n; ++i)
        if (out[i] == '\n' || out[i] == '\r') {
            out[i] = 0;
            break;
        }
}

// ---- Event hub ----------------------------------------------------------------------------------

void Poke() {
    pthread_mutex_lock(&g_ev_mu);
    ++g_ev_gen;
    pthread_cond_signal(&g_ev_cv);
    pthread_mutex_unlock(&g_ev_mu);
}

// Blocks until the generation moves past `*seen`, or `deadline_ms` (monotonic) passes; a negative
// deadline waits for an event alone, forever if need be. True when an event woke it.
bool WaitEvent(unsigned long long *seen, long long deadline_ms) {
    pthread_mutex_lock(&g_ev_mu);
    bool event = g_ev_gen != *seen;
    while (!event) {
        if (deadline_ms < 0) {
            pthread_cond_wait(&g_ev_cv, &g_ev_mu);
        } else {
            const timespec until{static_cast<time_t>(deadline_ms / 1000),
                                 static_cast<long>((deadline_ms % 1000) * 1000000)};
            if (pthread_cond_timedwait(&g_ev_cv, &g_ev_mu, &until) == ETIMEDOUT) {
                event = g_ev_gen != *seen;
                break;
            }
        }
        event = g_ev_gen != *seen;
    }
    *seen = g_ev_gen;
    pthread_mutex_unlock(&g_ev_mu);
    return event;
}

// After an event, give a burst a moment to finish so it is handled in one pass.
void Settle(unsigned long long *seen) {
    const long long cap = NowMs() + kSettleCapMs;
    while (NowMs() < cap && WaitEvent(seen, NowMs() + kSettleMs)) {}
}

// ---- Framework bring-up ---------------------------------------------------------------------------

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
    jclass cls_context = nullptr, cls_pm = nullptr, cls_nm = nullptr, cls_builder = nullptr, cls_big = nullptr,
           cls_pi = nullptr, cls_intent = nullptr, cls_component = nullptr, cls_channel = nullptr, cls_sbn = nullptr;
    jmethodID get_service = nullptr, get_package_name = nullptr, get_app_context = nullptr, get_app_info = nullptr,
              get_package_manager = nullptr, start_service = nullptr;
    jfieldID app_icon = nullptr;
    jmethodID launch_intent = nullptr;
    jmethodID nm_create = nullptr, nm_notify = nullptr, nm_cancel = nullptr, nm_enabled = nullptr, nm_active = nullptr,
              nm_get = nullptr;
    jmethodID builder_ctor = nullptr, b_icon = nullptr, b_title = nullptr, b_text = nullptr, b_style = nullptr,
              b_ongoing = nullptr, b_alert = nullptr, b_when = nullptr, b_category = nullptr,
              b_content_intent = nullptr, b_action = nullptr, b_build = nullptr, b_color = nullptr,
              b_when_time = nullptr, b_chrono = nullptr;
    jmethodID big_ctor = nullptr, big_set = nullptr;
    jmethodID pi_activity = nullptr, pi_broadcast = nullptr;
    jmethodID intent_ctor = nullptr, intent_component = nullptr, intent_put_int = nullptr, intent_put_string = nullptr,
              intent_put_bool = nullptr;
    jmethodID component_ctor = nullptr;
    jmethodID channel_ctor = nullptr, ch_desc = nullptr, ch_badge = nullptr;
    jmethodID pm_new_lock = nullptr, lock_acquire = nullptr, lock_release = nullptr, lock_held = nullptr,
              lock_refcount = nullptr, lock_acquire_timeout = nullptr;
    jmethodID wifi_new_lock = nullptr, wlock_acquire = nullptr, wlock_release = nullptr, wlock_held = nullptr,
              wlock_refcount = nullptr;
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
// without throwing, so the entry would simply never show.
// If the primary id cannot be resurrected, the fallback id is used. Returns whether a usable
// channel now exists and records its id in g_active_channel.
bool EnsureChannel(JNIEnv *env) {
    g_channel_ok = false;
    if (!g_j.cls_channel || !g_j.channel_ctor || !g_j.nm || !g_j.nm_create) return false;
    const char *const ids[] = {g_channel, g_channel_fallback};
    for (const char *id : ids) {
        g_active_channel = id;
        jobject channel = env->NewObject(g_j.cls_channel, g_j.channel_ctor, String(env, id), String(env, kChannelName),
                                         kImportanceLow);
        if (!channel) {
            Clear(env);
            continue;
        }
        // The description and badge are cosmetic; a missing setter must not block channel creation.
        if (g_j.ch_desc) env->CallVoidMethod(channel, g_j.ch_desc, String(env, "知言 Satori 服务的运行状态"));
        if (g_j.ch_badge) env->CallVoidMethod(channel, g_j.ch_badge, JNI_FALSE);
        Clear(env);
        env->CallVoidMethod(g_j.nm, g_j.nm_create, channel);
        Clear(env);
        env->DeleteLocalRef(channel);
        // getNotificationChannel() returns null for a missing or deleted channel: verify, because
        // createNotificationChannel() silently ignores a channel whose id a ROM refuses to undelete.
        if (!g_j.nm_get) {
            g_channel_ok = true;
            return true;
        }
        jobject existing = env->CallObjectMethod(g_j.nm, g_j.nm_get, String(env, id));
        Clear(env);
        if (existing) {
            env->DeleteLocalRef(existing);
            g_channel_ok = true;
            return true;
        }
    }
    g_active_channel = g_channel;
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
    g_j.cls_sbn =
        static_cast<jclass>(env->NewGlobalRef(Class(env, "android/service/notification/StatusBarNotification")));

    const bool resolved = g_j.cls_pm && g_j.cls_nm && g_j.cls_builder && g_j.cls_big && g_j.cls_pi && g_j.cls_intent &&
                          g_j.cls_component && g_j.cls_channel && g_j.cls_sbn && app_info_class && package_manager;
    if (!resolved) return false;

    g_j.get_service = Method(env, g_j.cls_context, "getSystemService", "(Ljava/lang/String;)Ljava/lang/Object;");
    g_j.get_package_name = Method(env, g_j.cls_context, "getPackageName", "()Ljava/lang/String;");
    g_j.get_app_context = Method(env, g_j.cls_context, "getApplicationContext", "()Landroid/content/Context;");
    g_j.get_app_info = Method(env, g_j.cls_context, "getApplicationInfo", "()Landroid/content/pm/ApplicationInfo;");
    g_j.get_package_manager =
        Method(env, g_j.cls_context, "getPackageManager", "()Landroid/content/pm/PackageManager;");
    g_j.start_service =
        Method(env, g_j.cls_context, "startService", "(Landroid/content/Intent;)Landroid/content/ComponentName;");
    g_j.app_icon = Field(env, app_info_class, "icon", "I");
    g_j.launch_intent =
        Method(env, package_manager, "getLaunchIntentForPackage", "(Ljava/lang/String;)Landroid/content/Intent;");

    g_j.nm_create = Method(env, g_j.cls_nm, "createNotificationChannel", "(Landroid/app/NotificationChannel;)V");
    g_j.nm_get =
        Method(env, g_j.cls_nm, "getNotificationChannel", "(Ljava/lang/String;)Landroid/app/NotificationChannel;");
    g_j.nm_notify = Method(env, g_j.cls_nm, "notify", "(ILandroid/app/Notification;)V");
    g_j.nm_cancel = Method(env, g_j.cls_nm, "cancel", "(I)V");
    g_j.nm_enabled = Method(env, g_j.cls_nm, "areNotificationsEnabled", "()Z");
    g_j.nm_active =
        Method(env, g_j.cls_nm, "getActiveNotifications", "()[Landroid/service/notification/StatusBarNotification;");
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
    g_j.b_chrono = Method(env, g_j.cls_builder, "setUsesChronometer", chain);
    snprintf(chain, sizeof(chain), "(J)%s", builder);
    g_j.b_when_time = Method(env, g_j.cls_builder, "setWhen", chain);
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
    g_j.big_set =
        Method(env, g_j.cls_big, "setBigText", "(Ljava/lang/CharSequence;)Landroid/app/Notification$BigTextStyle;");
    if (!g_j.big_set)
        g_j.big_set =
            Method(env, g_j.cls_big, "bigText", "(Ljava/lang/CharSequence;)Landroid/app/Notification$BigTextStyle;");

    const char *pi_sig = "(Landroid/content/Context;ILandroid/content/Intent;I)Landroid/app/PendingIntent;";
    g_j.pi_activity = StaticMethod(env, g_j.cls_pi, "getActivity", pi_sig);
    g_j.pi_broadcast = StaticMethod(env, g_j.cls_pi, "getBroadcast", pi_sig);
    g_j.intent_ctor = Method(env, g_j.cls_intent, "<init>", "()V");
    g_j.intent_component =
        Method(env, g_j.cls_intent, "setComponent", "(Landroid/content/ComponentName;)Landroid/content/Intent;");
    g_j.intent_put_int = Method(env, g_j.cls_intent, "putExtra", "(Ljava/lang/String;I)Landroid/content/Intent;");
    g_j.intent_put_string =
        Method(env, g_j.cls_intent, "putExtra", "(Ljava/lang/String;Ljava/lang/String;)Landroid/content/Intent;");
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
    g_j.pm_new_lock =
        Method(env, power_class, "newWakeLock", "(ILjava/lang/String;)Landroid/os/PowerManager$WakeLock;");
    g_j.wifi_new_lock =
        Method(env, wifi_class, "createWifiLock", "(ILjava/lang/String;)Landroid/net/wifi/WifiManager$WifiLock;");
    Clear(env);
    jobject lock = g_j.pm_new_lock
                       ? env->CallObjectMethod(power, g_j.pm_new_lock, kPartialWakeLock, String(env, g_lock_tag))
                       : nullptr;
    jobject wifi_lock = g_j.wifi_new_lock
                            ? env->CallObjectMethod(wifi, g_j.wifi_new_lock, kWifiFullHighPerf, String(env, g_lock_tag))
                            : nullptr;
    Clear(env);
    if (lock) {
        jclass lock_class = env->GetObjectClass(lock);
        g_j.lock_acquire = Method(env, lock_class, "acquire", "()V");
        g_j.lock_acquire_timeout = Method(env, lock_class, "acquire", "(J)V");
        g_j.lock_release = Method(env, lock_class, "release", "()V");
        g_j.lock_held = Method(env, lock_class, "isHeld", "()Z");
        g_j.lock_refcount = Method(env, lock_class, "setReferenceCounted", "(Z)V");
        if (g_j.lock_refcount) {
            env->CallVoidMethod(lock, g_j.lock_refcount, JNI_FALSE);
            Clear(env);
        }
        g_j.lock = env->NewGlobalRef(lock);
        env->DeleteLocalRef(lock_class);
    }
    if (wifi_lock) {
        jclass wlock_class = env->GetObjectClass(wifi_lock);
        g_j.wlock_acquire = Method(env, wlock_class, "acquire", "()V");
        g_j.wlock_release = Method(env, wlock_class, "release", "()V");
        g_j.wlock_held = Method(env, wlock_class, "isHeld", "()Z");
        g_j.wlock_refcount = Method(env, wlock_class, "setReferenceCounted", "(Z)V");
        if (g_j.wlock_refcount) {
            env->CallVoidMethod(wifi_lock, g_j.wlock_refcount, JNI_FALSE);
            Clear(env);
        }
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
            if (field) {
                g_j.icon = env->GetStaticIntField(drawable, field);
                Clear(env);
            }
            env->DeleteLocalRef(drawable);
        }
    }

    // One low-importance, silent channel. Re-created on every start and again before each post.
    EnsureChannel(env);

    // Only mark ready when every method called unconditionally below is present; a null
    // method ID would be a hard crash, not a Java exception. The chronometer pair is optional.
    const bool complete = g_j.lock && g_j.wifi_lock && g_j.builder_ctor && g_j.b_icon && g_j.b_title && g_j.b_text &&
                          g_j.b_style && g_j.b_ongoing && g_j.b_alert && g_j.b_when && g_j.b_category && g_j.b_build &&
                          g_j.big_ctor && g_j.big_set && g_j.nm_notify && g_j.nm_cancel;
    g_j.ok = complete;
    return complete;
}

// ---- Wake locks -----------------------------------------------------------------------------------

bool IsHeld(JNIEnv *env, jobject lock, jmethodID method) {
    if (!lock || !method) return false;
    const jboolean held = env->CallBooleanMethod(lock, method);
    Clear(env);
    return held == JNI_TRUE;
}

// Brings the OS locks in line with the two reasons we might want them: the user toggle and any
// automatic hold around outbound work. `rearm` is set when an automatic hold has just begun: it
// (re)starts the safety timeout; every other call leaves a running timeout alone, so the hold
// really does end on schedule. Caller holds g_mu and g_j.ok.
void ApplyLock(JNIEnv *env, bool rearm) {
    const bool want_cpu = g_want_lock || g_auto_depth > 0;
    if (want_cpu) {
        if (g_want_lock) {
            // A user hold is untimed. PowerManager only cancels the pending timeout releaser on
            // release(), so promoting a timed lock has to release first; otherwise it expires.
            if (IsHeld(env, g_j.lock, g_j.lock_held) && !g_untimed && g_j.lock_release) {
                env->CallVoidMethod(g_j.lock, g_j.lock_release);
                Clear(env);
            }
            if (!IsHeld(env, g_j.lock, g_j.lock_held) && g_j.lock_acquire)
                env->CallVoidMethod(g_j.lock, g_j.lock_acquire);
            g_untimed = true;
        } else if (g_j.lock_acquire_timeout) {
            // An automatic hold expires on its own if a send wedges.
            if (rearm || !IsHeld(env, g_j.lock, g_j.lock_held))
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
    if (want_wifi && !wifi_held && g_j.wlock_acquire)
        env->CallVoidMethod(g_j.wifi_lock, g_j.wlock_acquire);
    else if (!want_wifi && wifi_held && g_j.wlock_release)
        env->CallVoidMethod(g_j.wifi_lock, g_j.wlock_release);
    Clear(env);
    g_cpu_held = IsHeld(env, g_j.lock, g_j.lock_held);
    g_wifi_held = IsHeld(env, g_j.wifi_lock, g_j.wlock_held);
    g_lock_held = g_cpu_held || g_wifi_held;
}

// Periodic restart of WeChat's own core service: a started service in the main process keeps
// it at SERVICE_ADJ, above the freezer cutoff, without any hook.
void Kick(JNIEnv *env) {
    if (!g_enabled) {
        snprintf(g_service_detail, sizeof(g_service_detail), "disabled");
        return;
    }
    if (!g_j.start_service || !g_j.context || !g_j.cls_intent || !g_j.intent_ctor || !g_j.cls_component ||
        !g_j.component_ctor || !g_j.intent_component) {
        snprintf(g_service_detail, sizeof(g_service_detail), "unavailable");
        return;
    }
    jobject intent = env->NewObject(g_j.cls_intent, g_j.intent_ctor);
    if (!intent) {
        Clear(env);
        snprintf(g_service_detail, sizeof(g_service_detail), "intent-failed");
        return;
    }
    jobject component =
        env->NewObject(g_j.cls_component, g_j.component_ctor, String(env, kHostPackage), String(env, kHostService));
    if (!component) {
        Clear(env);
        env->DeleteLocalRef(intent);
        snprintf(g_service_detail, sizeof(g_service_detail), "component-failed");
        return;
    }
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

// ---- The notification -------------------------------------------------------------------------------

// "上线于 03:41", with the date when it was not today.
void FormatSince(long long since_ms, long long now_ms, char *out, size_t size) {
    const time_t then = static_cast<time_t>(since_ms / 1000), today = static_cast<time_t>(now_ms / 1000);
    tm a{}, b{};
    localtime_r(&then, &a);
    localtime_r(&today, &b);
    if (a.tm_year == b.tm_year && a.tm_yday == b.tm_yday)
        snprintf(out, size, "上线于 %02d:%02d", a.tm_hour, a.tm_min);
    else
        snprintf(out, size, "上线于 %d-%d %02d:%02d", a.tm_mon + 1, a.tm_mday, a.tm_hour, a.tm_min);
}

bool StillPosted(JNIEnv *env) {
    if (!g_j.nm_active || !g_j.nm || !g_j.sbn_id) return true;
    jobjectArray active = static_cast<jobjectArray>(env->CallObjectMethod(g_j.nm, g_j.nm_active));
    if (env->ExceptionCheck()) {
        Clear(env);
        return true;
    }
    if (!active) return true;
    bool found = false;
    const jsize count = env->GetArrayLength(active);
    for (jsize i = 0; i < count; ++i) {
        jobject sbn = env->GetObjectArrayElement(active, i);
        if (!sbn) continue;
        const jint id = env->CallIntMethod(sbn, g_j.sbn_id);
        Clear(env);
        env->DeleteLocalRef(sbn);
        if (id == g_notify_id) {
            found = true;
            break;
        }
    }
    env->DeleteLocalRef(active);
    return found;
}

// Removes the resident entry and forgets the cached key, so re-enabling posts a fresh one.
// Caller holds g_mu and g_j.ok.
void CancelNotify(JNIEnv *env) {
    if (g_j.nm && g_j.nm_cancel) {
        env->CallVoidMethod(g_j.nm, g_j.nm_cancel, g_notify_id);
        Clear(env);
    }
    g_last_key[0] = 0;
    g_notify_ok = false;
    snprintf(g_notify_detail, sizeof(g_notify_detail), "disabled");
}

// Posts the entry when what it shows changed, or, when `verify` is set (the heartbeat, a status
// request), when the system no longer has it. Caller holds g_mu and g_j.ok.
void Notify(JNIEnv *env, bool verify) {
    if (!g_enabled) {
        CancelNotify(env);
        return;
    }
    if (!g_j.builder_ctor) return;
    bool enabled = true;
    if (g_j.nm_enabled) {
        enabled = env->CallBooleanMethod(g_j.nm, g_j.nm_enabled) == JNI_TRUE;
        Clear(env);
    }
    g_notify_enabled = enabled;

    KeepaliveInputs in{};
    in.online = g_login_count > 0;
    in.listening = g_server_ready;
    in.clients = g_client_count > 0 ? g_client_count : 0;
    in.port = g_port;
    in.send_state = g_send_state;
    in.serving_since_ms = in.online && in.listening ? g_serving_wall_ms : 0;
    in.now_ms = WallMs();
    KeepaliveView view;
    KeepaliveRender(in, &view);
    // Everything the entry shows: the collapsed row, the expanded body, the chronometer's base, the
    // button label.
    char key[600];
    snprintf(key, sizeof(key), "%s|%s|%d|%lld|%d|%d", view.title, view.big, view.color, in.serving_since_ms,
             view.timer ? 1 : 0, g_want_lock ? 1 : 0);
    const bool changed = strcmp(key, g_last_key) != 0;
    if (!changed && !verify) return;
    if (!enabled) {
        // Nothing can be posted. Forget the key so the first pass after the permission comes back
        // (the next heartbeat or status check) posts again.
        g_last_key[0] = 0;
        snprintf(g_notify_detail, sizeof(g_notify_detail), "disabled");
        g_notify_ok = false;
        return;
    }
    if (!changed && StillPosted(env)) return;

    // The channel can disappear (WeChat or ColorOS deleting it): recreate it right before posting.
    EnsureChannel(env);
    jobject builder = env->NewObject(g_j.cls_builder, g_j.builder_ctor, g_j.context, String(env, g_active_channel));
    if (!builder) {
        Clear(env);
        snprintf(g_notify_detail, sizeof(g_notify_detail), "builder-failed");
        return;
    }
    jobject jtitle = String(env, view.title), jtext = String(env, view.text);
    env->CallObjectMethod(builder, g_j.b_icon, g_j.icon);
    env->CallObjectMethod(builder, g_j.b_title, jtitle);
    env->CallObjectMethod(builder, g_j.b_text, jtext);
    jobject style = env->NewObject(g_j.cls_big, g_j.big_ctor);
    if (style) {
        env->CallObjectMethod(style, g_j.big_set, String(env, view.big));
        env->CallObjectMethod(builder, g_j.b_style, style);
    }
    env->CallObjectMethod(builder, g_j.b_ongoing, JNI_TRUE);
    env->CallObjectMethod(builder, g_j.b_alert, JNI_TRUE);
    // The uptime is the system's chronometer counting from the moment the service came up: SystemUI
    // ticks it, so the entry is never redrawn for it. Without a service there is no time to show.
    const bool timer = view.timer && g_j.b_chrono && g_j.b_when_time;
    env->CallObjectMethod(builder, g_j.b_when, timer ? JNI_TRUE : JNI_FALSE);
    if (timer) {
        env->CallObjectMethod(builder, g_j.b_when_time, static_cast<jlong>(in.serving_since_ms));
        env->CallObjectMethod(builder, g_j.b_chrono, JNI_TRUE);
    }
    env->CallObjectMethod(builder, g_j.b_category, String(env, "service"));
    if (g_j.b_color) env->CallObjectMethod(builder, g_j.b_color, view.color);
    Clear(env);

    // Tap opens WeChat; the launcher intent already carries FLAG_ACTIVITY_NEW_TASK.
    if (g_j.launch_intent && g_j.get_package_manager && g_j.pi_activity && g_j.b_content_intent) {
        jobject manager = env->CallObjectMethod(g_j.context, g_j.get_package_manager);
        jobject launch =
            manager ? env->CallObjectMethod(manager, g_j.launch_intent, String(env, kHostPackage)) : nullptr;
        Clear(env);
        if (manager) env->DeleteLocalRef(manager);
        if (launch) {
            jobject open = env->CallStaticObjectMethod(g_j.cls_pi, g_j.pi_activity, g_j.context, kReqOpen, launch,
                                                       kFlagUpdateCurrent | kFlagImmutable);
            Clear(env);
            if (open) {
                env->CallObjectMethod(builder, g_j.b_content_intent, open);
                Clear(env);
                env->DeleteLocalRef(open);
            }
            env->DeleteLocalRef(launch);
        }
    }

    // No action button: the wake lock is switched from the module page in the root manager
    // (POST /v1/internal/wakelock); the module ships no dex, so nothing in WeChat could receive it.

    jobject notification = env->CallObjectMethod(builder, g_j.b_build);
    if (!notification) {
        Clear(env);
        snprintf(g_notify_detail, sizeof(g_notify_detail), "build-failed");
    } else {
        env->CallVoidMethod(g_j.nm, g_j.nm_notify, g_notify_id, notification);
        if (env->ExceptionCheck()) {
            Clear(env);
            snprintf(g_notify_detail, sizeof(g_notify_detail), "notify-failed");
            g_notify_ok = false;
        } else {
            snprintf(g_notify_detail, sizeof(g_notify_detail), "posted");
            g_notify_ok = true;
            if (!changed) g_reposts = g_reposts + 1;
            snprintf(g_last_key, sizeof(g_last_key), "%s", key);
        }
        env->DeleteLocalRef(notification);
    }
    env->DeleteLocalRef(style);
    env->DeleteLocalRef(jtitle);
    env->DeleteLocalRef(jtext);
    env->DeleteLocalRef(builder);
}

// ---- The published state ---------------------------------------------------------------------------

const char *SendStateName(int state) {
    switch (state) {
        case kSendReady: return "ready";
        case kSendWaiting: return "waiting";
        case kSendIncompatible: return "incompatible";
        default: return "unknown";
    }
}

// The document, as a tree. Only slow-moving facts: counters that change with every message stay
// in /v1/internal/status, or the file would be rewritten all day.
cJSON *BuildState(long long changed_at_ms) {
    cJSON *root = cJSON_CreateObject();
    if (!root) return nullptr;
    cJSON_AddNumberToObject(root, "schema", 1);
    cJSON_AddStringToObject(root, "version", SATORI_WX_VERSION);
    cJSON_AddNumberToObject(root, "pid", static_cast<double>(getpid()));
    cJSON_AddNumberToObject(root, "port", g_port);
    cJSON_AddBoolToObject(root, "listening", g_server_ready);
    cJSON_AddBoolToObject(root, "enabled", g_enabled);
    char id[128], name[128];
    const bool online = PrimaryLogin(id, sizeof(id), name, sizeof(name));
    cJSON *login = cJSON_AddObjectToObject(root, "login");
    if (login) {
        cJSON_AddNumberToObject(login, "online", g_login_count);
        cJSON_AddStringToObject(login, "id", online ? id : "");
        cJSON_AddStringToObject(login, "name", online ? name : "");
    }
    cJSON_AddNumberToObject(root, "clients", g_client_count > 0 ? g_client_count : 0);
    cJSON_AddNumberToObject(root, "serving_since",
                            static_cast<double>(g_login_count > 0 && g_server_ready ? g_serving_wall_ms : 0));
    SendStatus send{};
    SendStatusGet(&send);
    cJSON *sender = cJSON_AddObjectToObject(root, "send");
    if (sender) {
        cJSON_AddStringToObject(sender, "state", SendStateName(send.state));
        cJSON_AddStringToObject(sender, "detail", send.state_detail);
    }
    cJSON *keep = cJSON_AddObjectToObject(root, "keepalive");
    if (keep) {
        cJSON_AddStringToObject(keep, "notify", g_notify_detail);
        cJSON_AddBoolToObject(keep, "wakelock", g_want_lock);
    }
    if (changed_at_ms >= 0) cJSON_AddNumberToObject(root, "changed_at", static_cast<double>(changed_at_ms));
    return root;
}

// Writes the document when it differs from the last one written. Atomic (temp file + rename in the
// same directory), so a reader never sees half of it and an inotify watcher gets one clean
// "moved in" event. Keeper thread only.
void PublishState() {
    if (!g_state_path[0]) return;
    cJSON *root = BuildState(-1);
    if (!root) return;
    char *body = cJSON_PrintUnformatted(root);
    if (!body || (g_last_state && !strcmp(g_last_state, body))) {
        free(body);
        cJSON_Delete(root);
        return;
    }
    cJSON_AddNumberToObject(root, "changed_at", static_cast<double>(WallMs()));
    char *full = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    bool ok = false;
    if (full) {
        mkdir(g_state_dir, 0700);
        const int fd = open(g_state_tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd >= 0) {
            const size_t size = strlen(full);
            ok = write(fd, full, size) == static_cast<ssize_t>(size);
            close(fd);
            ok = ok && rename(g_state_tmp, g_state_path) == 0;
            if (!ok) unlink(g_state_tmp);
        }
    }
    if (ok) {
        free(g_last_state);
        g_last_state = body;
        snprintf(g_state_file, sizeof(g_state_file), "ok");
    } else {
        snprintf(g_state_file, sizeof(g_state_file), "error:%d", errno);
        free(body);
    }
    free(full);
}

// ---- The keeper thread -----------------------------------------------------------------------------------

// The sender's attach: it also gives the thread a Looper, which WeChat classes may want when the
// class lookups initialise them. One way of attaching for every JNI thread of the module.
JNIEnv *Env() { return g_vm ? static_cast<JNIEnv *>(ReflectEnv()) : nullptr; }

// The next moment the keeper has to wake on its own, or -1 when only an event can wake it.
// Keeper thread only; reads the auto-hold bookkeeping under g_mu.
long long NextDeadline() {
    if (!g_enabled) return -1;
    long long next = -1;
    auto consider = [&next](long long at) {
        if (at > 0 && (next < 0 || at < next)) next = at;
    };
    if (g_vm && !g_j.started && !g_resolve_gave_up) consider(g_resolve_next_ms ? g_resolve_next_ms : 1);
    if (g_j.ok) consider(g_next_beat_ms ? g_next_beat_ms : 1);
    consider(g_probe_next_ms);
    pthread_mutex_lock(&g_mu);
    if (g_auto_depth > 0) consider(g_auto_since_ms + kAutoTimeoutMs);
    pthread_mutex_unlock(&g_mu);
    return next;
}

// One pass: bring everything in line with the current state. Idempotent.
void Pass() {
    const long long now = NowMs();
    if (!g_enabled) {
        // One idempotent shutdown pass: no locks, no notification, no core-service restart, no
        // send probe. The state file still carries "enabled":false for the companion app.
        pthread_mutex_lock(&g_mu);
        g_want_lock = false;
        g_auto_depth = 0;
        g_sustain_wifi = false;
        g_serving_since_ms = 0;
        g_serving_wall_ms = 0;
        g_was_serving = false;
        g_was_online = false;
        g_probe_next_ms = 0;
        g_next_beat_ms = 0;
        g_send_state = kSendUnknown;
        pthread_mutex_unlock(&g_mu);
        JNIEnv *env = g_vm ? Env() : nullptr;
        if (env && g_j.ok && env->PushLocalFrame(32) == 0) {
            ApplyLock(env, false);
            CancelNotify(env);
            env->PopLocalFrame(nullptr);
        }
        PublishState();
        return;
    }
    const bool online = g_login_count > 0;
    const bool serving = online && g_server_ready;
    pthread_mutex_lock(&g_mu);
    if (serving && !g_was_serving) {
        g_serving_since_ms = now;
        g_serving_wall_ms = WallMs();
    }
    if (!serving) {
        g_serving_since_ms = 0;
        g_serving_wall_ms = 0;
    }
    g_was_serving = serving;
    g_sustain_wifi = serving && g_client_count > 0;
    // A hold that outlived its safety timeout is a send that never finished: let go of it.
    if (g_auto_depth > 0 && now - g_auto_since_ms >= kAutoTimeoutMs) {
        __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "keepalive: automatic wake-lock hold expired (%d open)",
                            g_auto_depth);
        g_auto_depth = 0;
    }
    pthread_mutex_unlock(&g_mu);

    JNIEnv *env = Env();
    // Bring-up of the Java side: the host Application exists a moment after the module loads.
    if (g_vm && !env && !g_j.started && !g_resolve_gave_up && now >= g_resolve_next_ms)
        g_resolve_next_ms = now + 1000; // the VM would not attach this thread; try again, do not spin
    if (env && !g_j.started && !g_resolve_gave_up && now >= g_resolve_next_ms) {
        pthread_mutex_lock(&g_mu);
        if (env->PushLocalFrame(256) == 0) {
            ResolveJava(env);
            env->PopLocalFrame(nullptr);
        }
        pthread_mutex_unlock(&g_mu);
        if (g_j.ok) {
            __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "keepalive: resident notification armed");
        } else if (g_j.started) {
            // Resolution was attempted and is incomplete: never retry (that would leak references).
            snprintf(g_notify_detail, sizeof(g_notify_detail), "unavailable");
            g_resolve_gave_up = true;
            __android_log_print(ANDROID_LOG_WARN, "SatoriWx",
                                "keepalive: framework lookup incomplete; notification off");
        } else if (++g_resolve_tries >= kResolveMaxTries) {
            snprintf(g_notify_detail, sizeof(g_notify_detail), "unavailable");
            g_resolve_gave_up = true;
            __android_log_print(ANDROID_LOG_WARN, "SatoriWx",
                                "keepalive: application context never became usable; notification disabled");
        } else {
            g_resolve_next_ms = NowMs() + (g_resolve_tries < 4 ? (200LL << g_resolve_tries) : 3000);
        }
    }

    // The sender's readiness: probe after a login, retry while it is not ready, and again on the
    // heartbeat. SendProbe may do real JNI work, so it runs outside g_mu.
    if (!g_vm || !online)
        g_probe_next_ms = 0; // nothing to probe: no VM, or no login for it to matter to
    else if (!g_was_online) {
        g_probe_step = 0;
        g_probe_next_ms = now;
    }
    g_was_online = online;
    const bool beat = g_j.ok && now >= g_next_beat_ms;
    const bool asked = g_probe_req;
    g_probe_req = false;
    if (g_vm && online && ((g_probe_next_ms && now >= g_probe_next_ms) || beat || asked)) {
        const int state = SendProbe();
        g_send_state = state;
        if (state == kSendReady || state == kSendIncompatible ||
            g_probe_step + 1 >= static_cast<int>(sizeof(kProbeDelaysMs) / sizeof(kProbeDelaysMs[0])))
            g_probe_next_ms = 0;
        else
            g_probe_next_ms = NowMs() + kProbeDelaysMs[++g_probe_step];
    }
    {
        SendStatus send{};
        SendStatusGet(&send); // a send attempt may have settled it since
        g_send_state = send.state;
    }

    if (g_j.ok && env) {
        const bool verify = beat || g_verify;
        g_verify = false;
        pthread_mutex_lock(&g_mu);
        if (env->PushLocalFrame(96) == 0) {
            ApplyLock(env, false);
            if (beat && g_instance == 0) Kick(env);
            Notify(env, verify);
            env->PopLocalFrame(nullptr);
        }
        pthread_mutex_unlock(&g_mu);
    }
    if (beat) {
        g_next_beat_ms = now + kHeartbeatMs;
        g_beat_ms = now;
    }
    PublishState();
}

void *Manager(void *) {
    pthread_setname_np(pthread_self(), "satori-wx-keep");
    unsigned long long seen = 0;
    for (;;) {
        long long deadline = NextDeadline();
        // A deadline already behind us is a bookkeeping slip, not a reason to spin: wait a beat.
        if (deadline >= 0 && deadline < NowMs() + kMinWaitMs) deadline = NowMs() + kMinWaitMs;
        const bool event = WaitEvent(&seen, deadline);
        if (event) Settle(&seen);
        g_wakes = g_wakes + 1;
        if (event) g_event_wakes = g_event_wakes + 1;
        Pass();
    }
    return nullptr;
}
} // namespace

// ---- Public interface ---------------------------------------------------------------------------------

void KeepaliveRender(const KeepaliveInputs &in, KeepaliveView *out) {
    *out = KeepaliveView{};
    out->color = kColorDegraded;
    if (in.online && in.listening) {
        out->color = kColorOnline;
        snprintf(out->title, sizeof(out->title), "知言 · 运行中");
        if (in.clients <= 0)
            snprintf(out->text, sizeof(out->text), "等待客户端连接 · 端口 %u", in.port);
        else
            snprintf(out->text, sizeof(out->text), "已连接 %d 个客户端 · 端口 %u", in.clients, in.port);
    } else if (!in.online) {
        out->color = kColorWait;
        snprintf(out->title, sizeof(out->title), "知言 · 等待登录");
        if (in.listening)
            snprintf(out->text, sizeof(out->text), "打开微信登录，即可连接服务");
        else
            snprintf(out->text, sizeof(out->text), "等待微信登录与本地服务启动");
    } else {
        snprintf(out->title, sizeof(out->title), "知言 · 服务异常");
        snprintf(out->text, sizeof(out->text), "本地端口 %u 未监听", in.port);
    }
    snprintf(out->big, sizeof(out->big), "%s", out->text);
    // The expanded body adds what is not already a row: when the service came up (the live
    // duration is the chronometer's job), and a sender that cannot work. Nothing about the wake lock
    // (its state is the button's own label) or sending being switched off (there is no switch).
    if (in.online && in.listening && in.serving_since_ms > 0) {
        out->timer = true;
        char since[64];
        FormatSince(in.serving_since_ms, in.now_ms, since, sizeof(since));
        const size_t used = strlen(out->big);
        snprintf(out->big + used, sizeof(out->big) - used, "\n%s", since);
    }
    if (in.online && in.send_state == kSendIncompatible) {
        const size_t used = strlen(out->big);
        snprintf(out->big + used, sizeof(out->big) - used, "\n发送与当前微信版本不兼容，暂时发不出消息");
    }
}

char *KeepaliveStateJson(long long changed_at_ms) {
    cJSON *root = BuildState(changed_at_ms);
    if (!root) return nullptr;
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

void KeepaliveStart(void *vm, const Config &config, const char *data_dir, int instance, bool enabled) {
    if (g_started) return;
    g_started = true;
    g_enabled = enabled;
    g_vm = static_cast<JavaVM *>(vm);
    g_port = config.port;
    g_instance = instance < 0 ? 0 : instance;
    snprintf(g_token, sizeof(g_token), "%s", config.token);
    if (g_instance > 0) {
        g_notify_id = kNotifyIdBase + 0x100 + g_instance;
        snprintf(g_channel, sizeof(g_channel), "satori-wx-status-dev");
        snprintf(g_channel_fallback, sizeof(g_channel_fallback), "satori-wx-status-dev-2");
        snprintf(g_lock_tag, sizeof(g_lock_tag), "satori-wx:wakelock-dev");
        g_active_channel = g_channel;
    }
    if (data_dir && *data_dir) {
        snprintf(g_state_dir, sizeof(g_state_dir), "%s/files/satori-wx", data_dir);
        snprintf(g_state_path, sizeof(g_state_path), "%s/%s", g_state_dir,
                 g_instance ? "state-dev.json" : "state.json");
        snprintf(g_state_tmp, sizeof(g_state_tmp), "%s.tmp", g_state_path);
        snprintf(g_state_file, sizeof(g_state_file), "pending");
    }
    pthread_condattr_t attr;
    pthread_condattr_init(&attr);
    pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
    pthread_cond_init(&g_ev_cv, &attr);
    pthread_condattr_destroy(&attr);
    SetStateListener(Poke);
    pthread_t thread;
    if (pthread_create(&thread, nullptr, Manager, nullptr)) {
        SetStateListener(nullptr);
        __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "keepalive thread creation failed");
        return;
    }
    pthread_detach(thread);
    Poke(); // the first pass: publish the state as it is now, and bring the Java side up
}

void KeepaliveSetEnabled(bool enabled) {
    if (g_enabled == enabled) return;
    g_enabled = enabled;
    if (enabled) {
        pthread_mutex_lock(&g_mu);
        g_want_lock = true;
        g_next_beat_ms = 0;
        g_beat_ms = 0;
        pthread_mutex_unlock(&g_mu);
    }
    Poke();
}

bool KeepaliveEnabled() { return g_enabled; }

void KeepaliveWakelock(int action, bool *held) {
    if (!g_enabled) {
        if (held) *held = false;
        return;
    }
    if (held) *held = g_want_lock;
    JNIEnv *env = g_vm ? Env() : nullptr;
    pthread_mutex_lock(&g_mu);
    if (action == 1)
        g_want_lock = true;
    else if (action == 0)
        g_want_lock = false;
    else
        g_want_lock = !g_want_lock;
    if (env && g_j.ok && env->PushLocalFrame(32) == 0) {
        ApplyLock(env, false);
        env->PopLocalFrame(nullptr);
    }
    if (held) *held = g_want_lock;
    pthread_mutex_unlock(&g_mu);
    // The button's label and the published intent follow; the keeper redraws, not this thread.
    Poke();
}

// Ref-counted automatic hold around one outbound mutation. A wedged send cannot hold the CPU
// past kAutoTimeoutMs; nested calls share the one underlying lock. No notification redraw:
// the user intent did not change, only the transient CPU/Wi-Fi state.
void KeepaliveWakelockBegin() {
    if (!g_enabled) return;
    JNIEnv *env = g_vm ? Env() : nullptr;
    pthread_mutex_lock(&g_mu);
    const bool first = g_auto_depth == 0;
    g_auto_depth = g_auto_depth + 1;
    g_auto_since_ms = NowMs();
    if (env && g_j.ok && env->PushLocalFrame(32) == 0) {
        ApplyLock(env, true);
        env->PopLocalFrame(nullptr);
    }
    pthread_mutex_unlock(&g_mu);
    if (first) Poke(); // the keeper has to be awake when the safety timeout comes due
}

void KeepaliveWakelockEnd() {
    if (!g_enabled) return;
    JNIEnv *env = g_vm ? Env() : nullptr;
    pthread_mutex_lock(&g_mu);
    if (g_auto_depth > 0) g_auto_depth = g_auto_depth - 1;
    if (env && g_j.ok && env->PushLocalFrame(32) == 0) {
        ApplyLock(env, false);
        env->PopLocalFrame(nullptr);
    }
    pthread_mutex_unlock(&g_mu);
}

void KeepaliveStatus(cJSON *object) {
    if (!object) return;
    // A status request is a natural moment to double-check the notification, and to look again at a
    // sender that is not ready: ask, at most every 30 seconds, and let the keeper do it (this thread
    // does no JNI work for it).
    const long long now = NowMs();
    if (g_started && g_vm && (g_j.ok || g_send_state != kSendReady) && now - g_verify_asked_ms >= kVerifyGapMs) {
        g_verify_asked_ms = now;
        g_verify = g_j.ok;
        g_probe_req = g_send_state != kSendReady && g_login_count > 0;
        Poke();
    }
    cJSON *keep = cJSON_AddObjectToObject(object, "keepalive");
    if (!keep) return;
    cJSON_AddBoolToObject(keep, "enabled", g_enabled);
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
    cJSON_AddNumberToObject(keep, "uptime_ms",
                            static_cast<double>(g_serving_since_ms > 0 ? NowMs() - g_serving_since_ms : 0));
    cJSON_AddStringToObject(keep, "service", g_service_detail);
    cJSON_AddStringToObject(keep, "notify", g_notify_detail);
    cJSON_AddNumberToObject(keep, "reposts", static_cast<double>(g_reposts));
    // The keeper's own activity: wakes minus event_wakes is the heartbeat and its deadlines. An
    // idle service shows these standing still.
    cJSON_AddNumberToObject(keep, "wakes", static_cast<double>(g_wakes));
    cJSON_AddNumberToObject(keep, "event_wakes", static_cast<double>(g_event_wakes));
    cJSON_AddNumberToObject(keep, "heartbeat_age_ms", static_cast<double>(g_beat_ms ? NowMs() - g_beat_ms : -1));
    cJSON_AddStringToObject(keep, "state_file", g_state_file);
    cJSON_AddNumberToObject(keep, "oom_score_adj", static_cast<double>(ReadLong("/proc/self/oom_score_adj", -1)));
    char wchan[48];
    ReadText("/proc/self/wchan", wchan, sizeof(wchan));
    cJSON_AddStringToObject(keep, "wchan", wchan);
}
} // namespace satori
