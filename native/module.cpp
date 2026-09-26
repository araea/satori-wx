#include "zygisk.hpp"
#include "server.h"
#include "protocol.h"
#include "wx_adapter.h"
#include "wx_live.h"
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

void *Serve(void *) {
    pthread_setname_np(pthread_self(), "satori-wx");
    const int listener = satori::Listen(g_config);
    if (listener < 0) {
        __android_log_print(ANDROID_LOG_ERROR, "SatoriWx", "listen failed: %s", strerror(errno));
        return nullptr;
    }
    __android_log_print(ANDROID_LOG_INFO, "SatoriWx", "native Satori listening at 127.0.0.1:%u", g_config.port);
    satori::Run(listener, g_config, g_bus);
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
        }
        if (!target_ || !configured_) api_->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }
    void postAppSpecialize(const zygisk::AppSpecializeArgs *) override {
        if (!target_ || !configured_) return;
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
        // Read-only message store: reads the probe's captured cipher spec and publishes
        // message-created events. Disabled automatically if the key is unavailable.
        if (g_data_dir[0] && !satori::StartLiveStore(g_data_dir, g_bus, 1))
            __android_log_print(ANDROID_LOG_WARN, "SatoriWx", "live message store not started");
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
