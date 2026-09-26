#include "wx_backend.h"
#include "wx_live.h"
#include "wx_store.h"
#include <stdlib.h>
#include <string.h>

namespace satori {
namespace {
const char *Text(const Request &request, const char *key) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(request.params, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

int Limit(const Request &request, int fallback) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(request.params, "limit");
    return cJSON_IsNumber(value) ? static_cast<int>(value->valuedouble) : fallback;
}

Response Read(cJSON *object) {
    if (!object) return {404, nullptr};
    return {200, object};
}

Response Call(void *, const Request &request) {
    const char *name = request.method->name;
    Store *store = LiveStore();
    if (!store) return {503, nullptr};

    if (!strcmp(name, "message.get"))
        return Read(StoreMessageGet(store, Text(request, "channel_id"), Text(request, "message_id")));
    if (!strcmp(name, "message.list")) {
        cJSON *list = StoreMessageList(store, Text(request, "channel_id"), Text(request, "next"), Limit(request, 20));
        return list ? Response{200, list} : Response{400, nullptr};
    }
    if (!strcmp(name, "user.get"))
        return Read(StoreUserGet(store, Text(request, "user_id")));
    if (!strcmp(name, "friend.list")) {
        cJSON *list = StoreFriendList(store, Text(request, "next"), Limit(request, 50));
        return list ? Response{200, list} : Response{400, nullptr};
    }
    if (!strcmp(name, "guild.get"))
        return Read(StoreGuildGet(store, Text(request, "guild_id")));
    if (!strcmp(name, "guild.list")) {
        cJSON *list = StoreGuildList(store, Text(request, "next"), Limit(request, 50));
        return list ? Response{200, list} : Response{400, nullptr};
    }
    if (!strcmp(name, "channel.get"))
        return Read(StoreChannelGet(store, Text(request, "channel_id")));
    if (!strcmp(name, "channel.list")) {
        cJSON *list = StoreChannelList(store, Text(request, "guild_id"), Text(request, "next"), Limit(request, 50));
        return list ? Response{200, list} : Response{400, nullptr};
    }
    return {501, nullptr};
}

const char *const kFeatures[] = {
    "message.get", "message.list",
    "user.get", "friend.list",
    "guild.get", "guild.list",
    "channel.get", "channel.list",
};
} // namespace

const Backend *WeChatBackend() {
    static const Backend backend{nullptr, Call};
    return &backend;
}

const char *const *WeChatFeatures(size_t *count) {
    if (count) *count = sizeof(kFeatures) / sizeof(kFeatures[0]);
    return kFeatures;
}
} // namespace satori
