#include "server.h"
#include "protocol.h"
#include "multipart.h"
#include "wx_adapter.h"
#include <arpa/inet.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

namespace {
satori::EventBus *bus;
satori::Adapter *adapter;
void *Refresh(void *) {
    for (;;) { satori::AdapterRefresh(adapter); usleep(100000); }
}
void *Input(void *) {
    const char *fd = getenv("SATORI_TEST_EVENT_FD");
    if (!fd) return nullptr;
    FILE *input = fdopen(atoi(fd), "r");
    if (!input) return nullptr;
    char line[satori::kEventSize + 2];
    while (fgets(line, sizeof(line), input)) {
        line[strcspn(line, "\n")] = 0;
        while (!satori::Publish(bus, line + 1, line[0] == 'M')) usleep(1000);
    }
    fclose(input); return nullptr;
}
// Stands in for the module's signed message-media resolver: `_msg/<name>` maps to a file in
// $SATORI_TEST_MEDIA_DIR, and `_msg/denied` is refused, so the route contract (404 for a
// refused link, streaming for an accepted one) is exercised without a WeChat store.
bool Media(const char *, const char *path, satori::MediaFile *out) {
    const char *directory = getenv("SATORI_TEST_MEDIA_DIR");
    if (!directory || strncmp(path, "_msg/", 5) || !strcmp(path + 5, "denied") || strchr(path + 5, '/') || strstr(path + 5, "..")) return false;
    snprintf(out->path, sizeof(out->path), "%s/%s", directory, path + 5);
    snprintf(out->content_type, sizeof(out->content_type), "%s", "video/mp4");
    return true;
}
satori::Response Call(void *, const satori::Request &r) {
    cJSON *result = nullptr;
    const char *name = r.method->name;
    if (!strcmp(name, "message.list") && cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r.params, "channel_id")) &&
        !strcmp(cJSON_GetObjectItemCaseSensitive(r.params, "channel_id")->valuestring, "big")) {
        // A response far larger than any fixed buffer the server used to have.
        result = cJSON_CreateObject(); cJSON *data = cJSON_AddArrayToObject(result, "data");
        char text[4001]; memset(text, 'x', 4000); text[4000] = 0;
        for (int i = 0; i < 40; ++i) { cJSON *m = cJSON_CreateObject(); cJSON_AddStringToObject(m, "content", text); cJSON_AddItemToArray(data, m); }
        return {200, result};
    }
    if (!strcmp(name, "message.create")) {
        result = cJSON_CreateArray(); cJSON *m = cJSON_CreateObject();
        cJSON_AddStringToObject(m, "id", "fixture-message");
        cJSON_AddStringToObject(m, "content", cJSON_GetObjectItemCaseSensitive(r.params, "content")->valuestring);
        cJSON_AddItemToArray(result, m);
    } else if (strstr(name, ".list")) {
        result = cJSON_CreateObject(); cJSON_AddArrayToObject(result, "data");
        if (!strcmp(name, "message.list") && !cJSON_GetObjectItemCaseSensitive(r.params, "next")) cJSON_AddStringToObject(result, "next", "page-2");
    } else {
        result = cJSON_CreateObject();
        if (strstr(name, ".get") || strstr(name, "channel.create") || !strcmp(name, "guild.role.create")) cJSON_AddStringToObject(result, "id", "fixture");
        if (strstr(name, "channel.get") || strstr(name, "channel.create")) cJSON_AddNumberToObject(result, "type", 0);
    }
    return {200, result};
}
}
int main(int argc, char **) {
    satori::Config config;
    if (!satori::ReadConfig(STDIN_FILENO, &config)) return 2;
    config.port = 0;
    const int fd = satori::Listen(config);
    if (fd < 0) { perror("listen"); return 3; }
    const bool fixture = argc > 1;
    const char *account_dir = getenv("SATORI_TEST_ACCOUNT_DIR");
    if (fixture || account_dir || getenv("SATORI_TEST_EVENT_FD")) {
        bus = satori::CreateBus(); if (!bus) return 5;
    }
    if (fixture) {
        cJSON *event = cJSON_CreateObject(), *login = cJSON_CreateObject(), *features = cJSON_CreateArray();
        cJSON_AddStringToObject(event, "type", "login-added"); cJSON_AddItemToObject(event, "login", login);
        cJSON_AddNumberToObject(login, "sn", 0); cJSON_AddNumberToObject(login, "status", 1);
        cJSON_AddStringToObject(login, "adapter", "test-fixture"); cJSON_AddStringToObject(login, "platform", "wechat");
        cJSON *user = cJSON_AddObjectToObject(login, "user"); cJSON_AddStringToObject(user, "id", "fixture");
        cJSON_AddItemToObject(login, "features", features);
        for (size_t i = 0; i < satori::kMethodCount; ++i) cJSON_AddItemToArray(features, cJSON_CreateString(satori::kMethods[i].name));
        char *json = cJSON_PrintUnformatted(event);
        if (!json || !satori::Publish(bus, json)) return 6;
        free(json); cJSON_Delete(event);
    }
    if (account_dir) {
        adapter = satori::CreateAdapter(account_dir, bus);
        if (!adapter) return 8;
        pthread_t thread;
        if (pthread_create(&thread, nullptr, Refresh, nullptr)) return 9;
        pthread_detach(thread);
    }
    if (getenv("SATORI_TEST_EVENT_FD")) {
        pthread_t thread; if (pthread_create(&thread, nullptr, Input, nullptr)) return 7; pthread_detach(thread);
    }
    sockaddr_in address{}; socklen_t size = sizeof(address);
    if (getsockname(fd, reinterpret_cast<sockaddr *>(&address), &size)) return 4;
    printf("%u\n", ntohs(address.sin_port)); fflush(stdout);
    satori::SetMediaResolver(Media);
    const satori::Backend backend{nullptr, Call};
    satori::Run(fd, config, bus, fixture ? &backend : nullptr);
}
