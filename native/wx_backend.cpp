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

Response Call(void *, const Request &request) {
    const char *name = request.method->name;
    Store *store = LiveStore();
    if (!strcmp(name, "message.get")) {
        if (!store) return {503, nullptr};
        cJSON *message = StoreMessageGet(store, Text(request, "channel_id"), Text(request, "message_id"));
        if (!message) return {404, nullptr};
        return {200, message};
    }
    if (!strcmp(name, "message.list")) {
        if (!store) return {503, nullptr};
        const cJSON *limit = cJSON_GetObjectItemCaseSensitive(request.params, "limit");
        const int count = cJSON_IsNumber(limit) ? static_cast<int>(limit->valuedouble) : 20;
        cJSON *list = StoreMessageList(store, Text(request, "channel_id"), Text(request, "next"), count);
        if (!list) return {400, nullptr};
        return {200, list};
    }
    return {501, nullptr};
}

const char *const kFeatures[] = {"message.get", "message.list"};
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
