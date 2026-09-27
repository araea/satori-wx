#include "zygisk.hpp"
#include "server.h"
#include "protocol.h"
#include "wx_adapter.h"
#include "wx_backend.h"
#include "wx_capabilities.h"
#include "wx_keepalive.h"
#include "wx_key.h"
#include "wx_live.h"
#include "wx_send.h"
#include <android/log.h>
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace {
satori::Config g_config;
satori::EventBus *g_bus = nullptr;
char g_data_dir[256] = {};
constexpr unsigned kAccountIntervalMs = 3000;

// Adds backend-specific fields to /v1/internal/status and /v1/internal/capabilities: the
// sender's state and counters, plus (for capabilities) the methods WeChat cannot express.
void AddBackendStatus(cJSON *object, bool capabilities) {
    satori::SendStatus status{};
    satori::SendStatusGet(&status);
    cJSON *send = cJSON_CreateObject();
    if (send) {
        cJSON_AddItemToObject(object, "send", send);
        cJSON_AddBoolToObject(send, "enabled", status.enabled);
        cJSON_AddBoolToObject(send, "ready", status.ready);
        cJSON_AddBoolToObject(send, "resolved", status.resolved);
        cJSON_AddBoolToObject(send, "dispatcher", status.dispatcher);
        cJSON_AddNumberToObject(send, "sent", static_cast<double>(status.sent));
        cJSON_AddNumberToObject(send, "failed", static_cast<double>(status.failed));
        cJSON_AddNumberToObject(send, "rejected", static_cast<double>(status.rejected));
        cJSON_AddNumberToObject(send, "recalled", static_cast<double>(status.recalled));
        if (status.last_age_ms >= 0) {
            cJSON_AddNumberToObject(send, "last_age_ms", static_cast<double>(status.last_age_ms));
            cJSON_AddBoolToObject(send, "last_ok", status.last_ok);
            cJSON_AddStringToObject(send, "last_target", status.last_target);
            cJSON_AddNumberToObject(send, "last_net_id", static_cast<double>(status.last_net_id));
            cJSON_AddNumberToObject(send, "last_local_id", static_cast<double>(status.last_local_id));
            if (status.last_error[0]) cJSON_AddStringToObject(send, "last_error", status.last_error);
        }
    }
    satori::KeepaliveStatus(object);
    if (!capabilities) return;
    size_t unsupported_count = 0;
    const char *const *unsupported = satori::WeChatUnsupported(&unsupported_count);
    cJSON *removed = cJSON_CreateArray();
    if (!removed) return;
    cJSON_AddItemToObject(object, "unsupported", removed);
    for (size_t i = 0; i < unsupported_count; ++i) cJSON_AddItemToArray(removed, cJSON_CreateString(unsupported[i]));
}

void *Serve(void *) {
    pthread_setname_np(pthread_self(), "satori-wx");
    const int listener = satori::Listen(g_config);
    if (listener < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "listen failed: %s", strerror(errno));
        return nullptr;
    }
    satori::g_server_ready = true;
    __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "native Satori listening at 127.0.0.1:%u", g_config.port);
    satori::Run(listener, g_config, g_bus, satori::WeChatBackend());
    return nullptr;
}

// Resolves the sender once the account is online so /v1/internal/status reports real
// capability without waiting for the first message.create. Runs on its own thread, well
// after the app has finished starting, and only ever reads.
void *WarmSend(void *) {
    pthread_setname_np(pthread_self(), "satori-wx-sendwarm");
    if (!satori::SendEnabled()) return nullptr;
    for (int i = 0; i < 300 && satori::g_login_count <= 0; ++i) {
        const timespec second{1, 0};
        nanosleep(&second, nullptr);
    }
    if (satori::g_login_count <= 0) return nullptr;
    // Classes resolve as soon as the app class loader is usable, but the network dispatcher
    // only appears once the connection is up: keep probing until both are true.
    for (int i = 0; i < 90; ++i) {
        if (satori::SendWarmUp() && satori::SendDispatcherReady()) break;
        const timespec delay{10, 0};
        nanosleep(&delay, nullptr);
    }
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
        if (!satori::AdapterRefresh(adapter))
            __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "account prefs unavailable; keeping last snapshot");
        const timespec delay{kAccountIntervalMs / 1000, static_cast<long>(kAccountIntervalMs % 1000) * 1000000};
        nanosleep(&delay, nullptr);
    }
}

class WxServerModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api *api, JNIEnv *env) override { api_ = api; env_ = env; }
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
            const int fd = dir >= 0 ? openat(dir, "satori-wx.conf", O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK) : -1;
            configured_ = fd >= 0 && satori::ReadConfig(fd, &g_config);
            if (fd >= 0) close(fd);
            if (dir >= 0) close(dir);
            if (!configured_) __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "missing or invalid satori-wx.conf; server disabled");
            // The sender is opt-in; once on, any talker is accepted and pacing still applies.
            satori::SetSendEnabled(configured_ && g_config.send);
            satori::SetStatusProvider(AddBackendStatus);
            satori::SetWakelockProvider(satori::KeepaliveWakelock);
            if (configured_ && g_config.send)
                __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "message sender enabled for every talker");
        }
        if (!target_ || !configured_) api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }
    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!target_ || !configured_) return;
        // The sender needs the process JavaVM; classes are resolved lazily on first use.
        JavaVM *vm = nullptr;
        if (env_->GetJavaVM(&vm) == JNI_OK) satori::SendInit(vm);
        else __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "GetJavaVM failed; message sender disabled");
        // Resident notification + wake lock + in-process core-service keepalive. Needs the VM
        // and the parsed config; no-ops when the context never appears.
        if (vm) satori::KeepaliveStart(vm, g_config);
        g_bus = satori::CreateBus();
        if (!g_bus) {
            __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "event bus allocation failed; server disabled");
            return;
        }
        pthread_t server, account;
        if (pthread_create(&server, nullptr, Serve, nullptr)) {
            __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "server thread creation failed: %s", strerror(errno));
            return;
        }
        pthread_detach(server);
        if (!g_data_dir[0]) {
            __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "app data directory unknown; account identity unavailable");
        } else if (pthread_create(&account, nullptr, WatchAccount, nullptr)) {
            __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "account thread creation failed: %s", strerror(errno));
        } else {
            pthread_detach(account);
        }
        // Capture the SQLCipher key ourselves (no separate probe module) and read the
        // database read-only to publish message-created events. Disabled if the key never
        // appears.
        if (g_data_dir[0]) satori::KeyCaptureStart(vm, g_data_dir);
        if (g_data_dir[0] && !satori::StartLiveStore(g_data_dir, g_bus, 1))
            __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "live message store not started");
        // Resolve the sender once the account is online so status reflects real capability.
        if (satori::SendEnabled()) {
            pthread_t warm;
            if (pthread_create(&warm, nullptr, WarmSend, nullptr)) {
                __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "send warm-up thread creation failed: %s", strerror(errno));
            } else {
                pthread_detach(warm);
            }
        }
    }
    void preServerSpecialize(zygisk::ServerSpecializeArgs *) override {
        api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }
private:
    zygisk::Api *api_ = nullptr;
    JNIEnv *env_ = nullptr;
    bool target_ = false, configured_ = false;
};
}
REGISTER_ZYGISK_MODULE(WxServerModule)
