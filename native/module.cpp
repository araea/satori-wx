#include "zygisk.hpp"
#include "server.h"
#include "protocol.h"
#include "wx_adapter.h"
#include "wx_backend.h"
#include "wx_capabilities.h"
#include "wx_discover.h"
#include "wx_keepalive.h"
#include "wx_key.h"
#include "wx_live.h"
#include "wx_media.h"
#include "wx_pat.h"
#include "media.h"
#include "wx_send.h"
#include "version.h"
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace {
satori::Config g_config;
satori::EventBus *g_bus = nullptr;
char g_data_dir[256] = {};
void *g_vm = nullptr;
constexpr unsigned kAccountIntervalMs = 3000;

// Master switch. The companion app writes <WeChat data>/files/satori-wx/enabled; a 1-second
// gate thread mirrors it here, so toggling never needs a WeChat restart.
volatile bool g_enabled_flag = true;
// Serialises start/stop against the gate thread. The server itself is stopped through its pipe.
pthread_mutex_t g_gate_mu = PTHREAD_MUTEX_INITIALIZER;
bool g_aux_started = false;
volatile bool g_serving = false;

bool ReadEnabled(const char *dir) {
    if (!dir || !*dir) return true;
    char path[320];
    snprintf(path, sizeof(path), "%s/files/satori-wx/enabled", dir);
    const int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) return true; // fresh installs have no flag: enabled
    char byte = 0;
    const ssize_t n = read(fd, &byte, 1);
    close(fd);
    if (n <= 0) return true;
    return byte != '0';
}

// Adds backend-specific fields to /v1/internal/status and /v1/internal/capabilities: the
// sender's state and counters, plus (for capabilities) the methods WeChat cannot express.
void AddBackendStatus(cJSON *object, bool capabilities) {
    // The cached state only: the keeper probes on its own thread (after a login, on a retry schedule,
    // on the heartbeat, and when KeepaliveStatus below asks it to), never on this one.
    satori::SendStatus status{};
    satori::SendStatusGet(&status);
    cJSON *send = cJSON_CreateObject();
    if (send) {
        cJSON_AddItemToObject(object, "send", send);
        cJSON_AddBoolToObject(send, "ready", status.ready);
        cJSON_AddBoolToObject(send, "resolved", status.resolved);
        cJSON_AddBoolToObject(send, "dispatcher", status.dispatcher);
        // ready: it can send. waiting: classes found, network not up yet. incompatible: this WeChat build
        // lacks what the sender reflects on (WeChat is obfuscated and renames them between releases).
        cJSON_AddStringToObject(send, "state",
                                status.state == satori::kSendReady          ? "ready"
                                : status.state == satori::kSendWaiting      ? "waiting"
                                : status.state == satori::kSendIncompatible ? "incompatible"
                                                                            : "unknown");
        if (status.state_detail[0]) cJSON_AddStringToObject(send, "detail", status.state_detail);
        if (status.names[0]) cJSON_AddStringToObject(send, "names", status.names);
        cJSON_AddNumberToObject(send, "sent", static_cast<double>(status.sent));
        cJSON_AddNumberToObject(send, "failed", static_cast<double>(status.failed));
        cJSON_AddNumberToObject(send, "rejected", static_cast<double>(status.rejected));
        cJSON_AddNumberToObject(send, "recalled", static_cast<double>(status.recalled));
        cJSON_AddNumberToObject(send, "media", static_cast<double>(status.media));
        if (status.last_age_ms >= 0) {
            cJSON_AddNumberToObject(send, "last_age_ms", static_cast<double>(status.last_age_ms));
            cJSON_AddBoolToObject(send, "last_ok", status.last_ok);
            cJSON_AddStringToObject(send, "last_target", status.last_target);
            cJSON_AddNumberToObject(send, "last_net_id", static_cast<double>(status.last_net_id));
            cJSON_AddNumberToObject(send, "last_local_id", static_cast<double>(status.last_local_id));
            if (status.last_error[0]) cJSON_AddStringToObject(send, "last_error", status.last_error);
        }
    }
    satori::LiveStats live{};
    satori::LiveStatsGet(&live);
    cJSON *events = cJSON_CreateObject();
    if (events) {
        cJSON_AddItemToObject(object, "events", events);
        cJSON_AddBoolToObject(events, "open", live.open);
        cJSON_AddBoolToObject(events, "watching", live.watching);
        cJSON_AddNumberToObject(events, "wakes", static_cast<double>(live.wakes));
        cJSON_AddNumberToObject(events, "emitted", static_cast<double>(live.emitted));
        cJSON_AddNumberToObject(events, "skipped", static_cast<double>(live.skipped));
        cJSON_AddNumberToObject(events, "dropped", static_cast<double>(live.dropped));
    }
    satori::KeepaliveStatus(object);
    if (!capabilities) return;
    // The vocabulary shared with satori-qq's internal/capabilities (adapter, version, platform,
    // standard_methods, unsupported, event_types, message_elements, limits): acumen negotiates
    // on it instead of guessing from the adapter's name. standard_methods is added by the server
    // from login.features; `unsupported` below is the standard methods WeChat has no concept for.
    cJSON_AddStringToObject(object, "adapter", "satori-wx");
    cJSON_AddStringToObject(object, "version", SATORI_WX_VERSION);
    cJSON_AddStringToObject(object, "platform", "wechat");
    // The events this backend can raise, so a client can tell "nothing happened" from "cannot".
    static const char *const kEvents[] = {
        "message-created",    "message-deleted",      "guild-added",   "guild-removed",
        "guild-member-added", "guild-member-removed", "friend-added",  "friend-removed",
        "login-added",        "login-updated",        "login-removed",
    };
    cJSON *event_names = cJSON_CreateArray();
    if (event_names) {
        cJSON_AddItemToObject(object, "event_types", event_names);
        for (const char *name : kEvents) cJSON_AddItemToArray(event_names, cJSON_CreateString(name));
    }
    // What message.create understands, so a client can pick its elements without trial and error:
    // each media element is its own WeChat message; a <video> that is not an MP4 or an <audio> that
    // WeChat cannot play as a voice message goes out as a file (the returned Message says so).
    static const char *const kElements[] = {"text",  "at",  "a",     "br",    "p",   "message",
                                            "quote", "img", "video", "audio", "file"};
    cJSON *elements = cJSON_CreateArray();
    if (elements) {
        cJSON_AddItemToObject(object, "message_elements", elements);
        for (const char *name : kElements) cJSON_AddItemToArray(elements, cJSON_CreateString(name));
    }
    cJSON *limits = cJSON_CreateObject();
    if (limits) {
        cJSON_AddItemToObject(object, "limits", limits);
        cJSON_AddNumberToObject(limits, "upload_bytes",
                                static_cast<double>(1ull << 30)); // upload.create, streamed to disk
        cJSON_AddNumberToObject(limits, "inline_media_bytes",
                                static_cast<double>(12u << 20)); // data: / base64:// non-picture media
        cJSON_AddNumberToObject(limits, "inline_image_bytes", static_cast<double>(8u << 20));
        cJSON_AddNumberToObject(limits, "messages_per_request", 16); // <message> parts
        cJSON_AddNumberToObject(limits, "media_per_message", 8);
        cJSON_AddNumberToObject(limits, "images_per_message", 4);
        cJSON_AddNumberToObject(limits, "voice_max_seconds", 60); // longer <audio> goes out as a file
        cJSON_AddNumberToObject(limits, "upload_ttl_seconds", 300);
    }
    // The internal/* routes, named like satori-qq's `actions` so a client checks one list on both.
    static const char *const kActions[] = {"status", "capabilities", "wakelock", "pat", "at_all_quota"};
    cJSON *actions = cJSON_CreateArray();
    if (actions) {
        cJSON_AddItemToObject(object, "actions", actions);
        for (const char *name : kActions) cJSON_AddItemToArray(actions, cJSON_CreateString(name));
    }
    size_t unsupported_count = 0;
    const char *const *unsupported = satori::WeChatUnsupported(&unsupported_count);
    cJSON *removed = cJSON_CreateArray();
    if (!removed) return;
    cJSON_AddItemToObject(object, "unsupported", removed);
    for (size_t i = 0; i < unsupported_count; ++i) cJSON_AddItemToArray(removed, cJSON_CreateString(unsupported[i]));
}

void *Serve(void *) {
    pthread_setname_np(pthread_self(), "satori-wx");
    if (!g_bus) {
        g_serving = false;
        return nullptr;
    }
    const int listener = satori::Listen(g_config);
    if (listener < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "listen failed: %s", strerror(errno));
        g_serving = false;
        return nullptr;
    }
    satori::g_server_ready = true;
    satori::NotifyStateChanged();
    __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "native Satori listening at 127.0.0.1:%u", g_config.port);
    satori::Run(listener, g_config, g_bus, satori::WeChatBackend());
    g_serving = false;
    return nullptr;
}

void *WatchAccount(void *) {
    pthread_setname_np(pthread_self(), "satori-wx-account");
    satori::Adapter *adapter = satori::CreateAdapter(g_data_dir, g_bus);
    if (!adapter) {
        __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "account adapter not started");
        return nullptr;
    }
    for (;;) {
        // While switched off the identity is not refreshed and no login event is published; the
        // first pass after enabling reads the current account and announces it.
        if (g_enabled_flag && !satori::AdapterRefresh(adapter))
            __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "account prefs unavailable; keeping last snapshot");
        const timespec delay{kAccountIntervalMs / 1000, static_cast<long>(kAccountIntervalMs % 1000) * 1000000};
        nanosleep(&delay, nullptr);
    }
}

// Creates the parts the server needs (event bus, account watcher, live message store) once.
bool StartAux() {
    if (g_aux_started) return g_bus != nullptr;
    g_bus = satori::CreateBus();
    if (!g_bus) {
        __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "event bus allocation failed");
        return false;
    }
    g_aux_started = true;
    if (!g_data_dir[0]) {
        __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "app data directory unknown; account identity unavailable");
        return true;
    }
    pthread_t account;
    if (pthread_create(&account, nullptr, WatchAccount, nullptr))
        __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "account thread creation failed: %s", strerror(errno));
    else
        pthread_detach(account);
    if (!satori::StartLiveStore(g_data_dir, g_bus, 1))
        __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "live message store not started");
    return true;
}

void StartServing() {
    pthread_mutex_lock(&g_gate_mu);
    if (g_serving) {
        pthread_mutex_unlock(&g_gate_mu);
        return;
    }
    if (!StartAux()) {
        pthread_mutex_unlock(&g_gate_mu);
        return;
    }
    g_enabled_flag = true;
    satori::g_module_enabled = true;
    satori::KeepaliveSetEnabled(true);
    satori::Begin();
    g_serving = true;
    pthread_t server;
    if (pthread_create(&server, nullptr, Serve, nullptr)) {
        g_serving = false;
        __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "server thread creation failed: %s", strerror(errno));
    } else {
        pthread_detach(server);
    }
    pthread_mutex_unlock(&g_gate_mu);
}

void StopServing() {
    pthread_mutex_lock(&g_gate_mu);
    g_enabled_flag = false;
    satori::g_module_enabled = false;
    g_serving = false;
    pthread_mutex_unlock(&g_gate_mu);
    satori::Stop(); // wakes a running Run(): closes the port and every client
    satori::KeepaliveSetEnabled(false);
}

// One wake per second while watching; this is the only thread left when the switch is off.
void *Gate(void *) {
    pthread_setname_np(pthread_self(), "satori-wx-gate");
    bool last = g_enabled_flag;
    for (;;) {
        const timespec delay{1, 0};
        nanosleep(&delay, nullptr);
        const bool now = ReadEnabled(g_data_dir);
        if (now == last) continue;
        last = now;
        __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "master switch -> %s", now ? "on" : "off");
        if (now)
            StartServing();
        else
            StopServing();
    }
    return nullptr;
}

class WxServerModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override {
        api_ = api;
        env_ = env;
    }
    void preAppSpecialize(zygisk::AppSpecializeArgs *args) override {
        if (args && args->nice_name && !env_->ExceptionCheck()) {
            const char *name = env_->GetStringUTFChars(args->nice_name, nullptr);
            target_ = name && strcmp(name, "com.tencent.mm") == 0;
            if (name) env_->ReleaseStringUTFChars(args->nice_name, name);
            if (env_->ExceptionCheck()) env_->ExceptionClear();
        }
        if (target_ && args->app_data_dir && !env_->ExceptionCheck()) {
            // Copy the path before specialization: the account watcher runs after it.
            const char *dir = env_->GetStringUTFChars(args->app_data_dir, nullptr);
            if (dir) {
                if (strlen(dir) < sizeof(g_data_dir)) strcpy(g_data_dir, dir);
                env_->ReleaseStringUTFChars(args->app_data_dir, dir);
            }
            if (env_->ExceptionCheck()) env_->ExceptionClear();
        }
        if (target_) {
            // Read configuration while the module directory is still accessible.
            const int dir = api_->getModuleDir();
            const int fd =
                dir >= 0 ? openat(dir, "satori-wx.conf", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) : -1;
            configured_ = fd >= 0 && satori::ReadConfig(fd, &g_config);
            if (fd >= 0) close(fd);
            if (dir >= 0) close(dir);
            if (!configured_)
                __android_log_print(ANDROID_LOG_ERROR, "SatoriWx",
                                    "missing or invalid satori-wx.conf; server disabled");
            if (g_data_dir[0]) {
                char temp_dir[300];
                snprintf(temp_dir, sizeof(temp_dir), "%s/files/satori-wx-tmp", g_data_dir);
                satori::SetTempDir(temp_dir);
            }
            // Received-media links are signed with a key derived from the token, and served by
            // the store-backed resolver; nothing verifies until the token is known.
            if (configured_) satori::MediaSetSecret(g_config.token);
            satori::SetMediaResolver(satori::WeChatMediaResolver);
            satori::SetStatusProvider(AddBackendStatus);
            satori::SetWakelockProvider(satori::KeepaliveWakelock);
            satori::SetPatProvider(satori::PatDispatch);
        }
        if (!target_ || !configured_) api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }
    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!target_ || !configured_) return;
        // The sender needs the process JavaVM; classes are resolved lazily on first use.
        JavaVM *vm = nullptr;
        if (env_->GetJavaVM(&vm) == JNI_OK) {
            g_vm = vm;
            satori::SendInit(vm);
            satori::DiscoverInit(vm, g_data_dir);
        } else
            __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "GetJavaVM failed; message sender disabled");
        // Key capture is installed even while switched off: WeChat registers its SQLCipher key
        // during startup, and enabling later must not wait for the next process restart.
        if (g_data_dir[0]) satori::KeyCaptureStart(vm, g_data_dir);
        g_enabled_flag = ReadEnabled(g_data_dir);
        satori::g_module_enabled = g_enabled_flag;
        // The keeper always starts: when disabled it runs one shutdown pass, cancels any leftover
        // entry, releases the locks and publishes {"enabled":false} in the state file.
        satori::KeepaliveStart(vm, g_config, g_data_dir, 0, g_enabled_flag);
        if (g_enabled_flag) StartServing();
        pthread_t gate;
        if (pthread_create(&gate, nullptr, Gate, nullptr))
            __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "gate thread creation failed: %s", strerror(errno));
        else
            pthread_detach(gate);
    }
    void preServerSpecialize(zygisk::ServerSpecializeArgs *) override {
        api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool target_ = false, configured_ = false;
};
} // namespace
REGISTER_ZYGISK_MODULE(WxServerModule)
