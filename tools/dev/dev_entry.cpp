// Hot-load entry for on-device development (never part of the module ZIP).
//
// The Zygisk module's .so is pinned by Zygisk Next at boot, so a rebuilt module only runs
// after a phone restart. This file builds the same server, backend and sender into a second
// library that `tools/dev/inject.c` dlopen()s into the running WeChat main process; it then
// listens on its own loopback port (dev.conf next to the library) with the production token, so
// the exact same HTTP calls exercise the new code without restarting anything.
//
// Not started here: keepalive (the pinned module owns the notification and wake lock) and key
// capture (the pinned module already wrote key.log, which the live store reads).
#include "protocol.h"
#include "server.h"
#include "media.h"
#include "wx_adapter.h"
#include "wx_backend.h"
#include "wx_live.h"
#include "wx_media.h"
#include "wx_pat.h"
#include "wx_send.h"
#include <android/log.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <jni.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

namespace {
satori::Config g_config;
satori::EventBus *g_bus = nullptr;
const char kDataDir[] = "/data/user/0/com.tencent.mm";
char g_conf_path[512] = {};

void Status(cJSON *object, bool) {
    satori::SendStatus status{};
    satori::SendStatusGet(&status);
    cJSON *send = cJSON_CreateObject();
    if (send) {
        cJSON_AddItemToObject(object, "send", send);
        cJSON_AddBoolToObject(send, "ready", status.ready);
        cJSON_AddBoolToObject(send, "resolved", status.resolved);
        cJSON_AddBoolToObject(send, "dispatcher", status.dispatcher);
        cJSON_AddNumberToObject(send, "sent", static_cast<double>(status.sent));
        cJSON_AddNumberToObject(send, "failed", static_cast<double>(status.failed));
        cJSON_AddNumberToObject(send, "media", static_cast<double>(status.media));
        if (status.last_error[0]) cJSON_AddStringToObject(send, "last_error", status.last_error);
    }
    satori::LiveStats live{};
    satori::LiveStatsGet(&live);
    cJSON *events = cJSON_CreateObject();
    if (events) {
        cJSON_AddItemToObject(object, "events", events);
        cJSON_AddBoolToObject(events, "open", live.open);
        cJSON_AddBoolToObject(events, "watching", live.watching);
    }
    cJSON_AddStringToObject(object, "dev", "hot-loaded build");
}

void *Serve(void *) {
    pthread_setname_np(pthread_self(), "satori-dev");
    const int listener = satori::Listen(g_config);
    if (listener < 0) { __android_log_print(ANDROID_LOG_ERROR, "SatoriDev", "listen failed"); return nullptr; }
    __android_log_print(ANDROID_LOG_INFO, "SatoriDev", "dev Satori listening at 127.0.0.1:%u", g_config.port);
    satori::Run(listener, g_config, g_bus, satori::WeChatBackend());
    return nullptr;
}

void *WatchAccount(void *) {
    pthread_setname_np(pthread_self(), "satori-dev-acct");
    satori::Adapter *adapter = satori::CreateAdapter(kDataDir, g_bus);
    if (!adapter) return nullptr;
    for (;;) {
        satori::AdapterRefresh(adapter);
        const timespec delay{3, 0};
        nanosleep(&delay, nullptr);
    }
}

// One-line control channel on port+100 for poking the sender without going through HTTP:
//   file <talker> <path> <title...>
// Replies with one JSON-ish line. Development only.
void *Control(void *) {
    pthread_setname_np(pthread_self(), "satori-dev-ctl");
    const int listener = socket(AF_INET, SOCK_STREAM | SOCK_CLOEXEC, 0);
    int one = 1;
    setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(g_config.port + 100));
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(listener, reinterpret_cast<sockaddr *>(&address), sizeof(address)) || listen(listener, 4)) return nullptr;
    for (;;) {
        const int fd = accept(listener, nullptr, nullptr);
        if (fd < 0) continue;
        char line[2048] = {};
        const ssize_t got = recv(fd, line, sizeof(line) - 1, 0);
        char reply[512] = "bad command\n";
        if (got > 0) {
            line[strcspn(line, "\r\n")] = 0;
            char *cursor = line;
            const char *verb = strsep(&cursor, " ");
            if (verb && !strcmp(verb, "video")) {
                const char *talker = strsep(&cursor, " ");
                const char *path = strsep(&cursor, " ");
                const char *thumb = cursor && strcmp(cursor, "-") ? cursor : "";
                if (talker && path) {
                    satori::SendResult r = satori::SendVideo(talker, path, thumb, 3);
                    snprintf(reply, sizeof(reply), "{\"ok\":%d,\"detail\":\"%s\"}\n", r.ok ? 1 : 0, r.detail);
                }
            } else if (verb && !strcmp(verb, "voice")) {
                // voice <talker> <silk path> <duration ms>
                const char *talker = strsep(&cursor, " ");
                const char *path = strsep(&cursor, " ");
                if (talker && path && cursor) {
                    satori::SendResult r = satori::SendVoice(talker, path, atoi(cursor));
                    snprintf(reply, sizeof(reply), "{\"ok\":%d,\"detail\":\"%s\"}\n", r.ok ? 1 : 0, r.detail);
                }
            } else if (verb && !strcmp(verb, "quote")) {
                // quote <talker> <svrid> <localid> <sender> <text...>
                const char *talker = strsep(&cursor, " ");
                const char *svr = strsep(&cursor, " ");
                const char *local = strsep(&cursor, " ");
                const char *sender = strsep(&cursor, " ");
                if (talker && svr && local && sender && cursor) {
                    satori::QuoteRef ref{};
                    ref.svr_id = atoll(svr);
                    ref.local_id = atoll(local);
                    ref.created_s = time(nullptr) - 100;
                    snprintf(ref.talker, sizeof(ref.talker), "%s", talker);
                    snprintf(ref.sender, sizeof(ref.sender), "%s", sender);
                    snprintf(ref.display, sizeof(ref.display), "%s", "测试");
                    snprintf(ref.text, sizeof(ref.text), "%s", "被引用的一行");
                    satori::SendResult r = satori::SendQuote(talker, cursor, ref, nullptr);
                    snprintf(reply, sizeof(reply), "{\"ok\":%d,\"id\":%lld,\"net\":%d,\"detail\":\"%s\"}\n", r.ok ? 1 : 0, r.local_id, r.net_id, r.detail);
                }
            } else if (verb && !strcmp(verb, "file")) {
                const char *talker = strsep(&cursor, " ");
                const char *path = strsep(&cursor, " ");
                if (talker && path && cursor) {
                    satori::SendResult r = satori::SendFile(talker, path, cursor);
                    snprintf(reply, sizeof(reply), "{\"ok\":%d,\"id\":%lld,\"detail\":\"%s\"}\n", r.ok ? 1 : 0, r.local_id, r.detail);
                }
            }
        }
        send(fd, reply, strlen(reply), MSG_NOSIGNAL);
        close(fd);
    }
}

void *Boot(void *) {
    pthread_setname_np(pthread_self(), "satori-dev-boot");
    using GetVms = jint (*)(JavaVM **, jsize, jsize *);
    auto get_vms = reinterpret_cast<GetVms>(dlsym(RTLD_DEFAULT, "JNI_GetCreatedJavaVMs"));
    if (!get_vms) {
        void *art = dlopen("libnativehelper.so", RTLD_NOW);
        if (art) get_vms = reinterpret_cast<GetVms>(dlsym(art, "JNI_GetCreatedJavaVMs"));
    }
    JavaVM *vm = nullptr;
    jsize count = 0;
    if (!get_vms || get_vms(&vm, 1, &count) != JNI_OK || !vm) {
        __android_log_print(ANDROID_LOG_ERROR, "SatoriDev", "no JavaVM");
        return nullptr;
    }
    const int fd = open(g_conf_path, O_RDONLY | O_CLOEXEC);
    if (fd < 0 || !satori::ReadConfig(fd, &g_config)) {
        __android_log_print(ANDROID_LOG_ERROR, "SatoriDev", "bad dev conf %s", g_conf_path);
        if (fd >= 0) close(fd);
        return nullptr;
    }
    close(fd);
    satori::SetTempDir("/data/user/0/com.tencent.mm/files/satori-wx-tmp-dev");
    satori::MediaSetSecret(g_config.token);
    satori::SetMediaResolver(satori::WeChatMediaResolver);
    satori::SetStatusProvider(Status);
    satori::SetPatProvider(satori::PatDispatch);
    satori::SendInit(vm);
    g_bus = satori::CreateBus();
    if (!g_bus) return nullptr;
    pthread_t thread;
    if (pthread_create(&thread, nullptr, Serve, nullptr) == 0) pthread_detach(thread);
    if (pthread_create(&thread, nullptr, WatchAccount, nullptr) == 0) pthread_detach(thread);
    satori::StartLiveStore(kDataDir, g_bus, 1);
    if (pthread_create(&thread, nullptr, Control, nullptr) == 0) pthread_detach(thread);
    for (int i = 0; i < 90; ++i) {
        if (satori::SendWarmUp() && satori::SendDispatcherReady()) break;
        const timespec delay{2, 0};
        nanosleep(&delay, nullptr);
    }
    return nullptr;
}
} // namespace

// dlopen() holds the linker lock: only derive the config path and start a thread here.
__attribute__((constructor)) static void DevLoaded() {
    Dl_info info;
    if (!dladdr(reinterpret_cast<void *>(&DevLoaded), &info) || !info.dli_fname) return;
    snprintf(g_conf_path, sizeof(g_conf_path), "%s", info.dli_fname);
    char *dot = strrchr(g_conf_path, '.');
    if (!dot) return;
    strcpy(dot, ".conf");
    pthread_t thread;
    if (pthread_create(&thread, nullptr, Boot, nullptr) == 0) pthread_detach(thread);
}
