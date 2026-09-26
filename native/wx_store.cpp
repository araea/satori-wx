#include "wx_store.h"
#include "wcdb.h"
#include "protocol.h"
#include "vendor/cjson/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace satori {
namespace {
constexpr char kLibrary[] = "libWCDB.so";
constexpr long long kPollLimit = 50;
constexpr size_t kContentMax = 2000;
} // namespace

struct Store {
    Wcdb *db;
    char self_id[80];
    char error[256];
};

namespace {
// Group messages are stored as "<sender>:\n<text>"; private messages use the talker as sender.
bool SplitGroupSender(const char *content, char *sender, size_t sender_size, const char **text) {
    const char *marker = strstr(content, ":\n");
    if (!marker || marker == content) return false;
    const size_t length = static_cast<size_t>(marker - content);
    if (length >= sender_size) return false;
    for (size_t i = 0; i < length; ++i)
        if (content[i] == ' ' || content[i] == '\n' || content[i] == '\r') return false;
    memcpy(sender, content, length);
    sender[length] = 0;
    *text = marker + 2;
    return true;
}

cJSON *BuildEvent(const Store &store, const char *id, int is_send, long long create_time,
                  const char *talker, const char *content, int login_sn) {
    const bool group = talker && strstr(talker, "@chatroom") != nullptr;
    char sender[80] = {};
    const char *text = content ? content : "";
    if (is_send) {
        snprintf(sender, sizeof(sender), "%s", store.self_id);
    } else if (!(group && SplitGroupSender(text, sender, sizeof(sender), &text))) {
        snprintf(sender, sizeof(sender), "%s", talker ? talker : "");
    }
    char escaped[kContentMax + 64];
    if (!EscapeText(text, escaped, sizeof(escaped))) snprintf(escaped, sizeof(escaped), "%s", text);
    cJSON *root = cJSON_CreateObject();
    cJSON *login = cJSON_CreateObject();
    cJSON *channel = cJSON_CreateObject();
    cJSON *message = cJSON_CreateObject();
    cJSON *user = cJSON_CreateObject();
    if (!root || !login || !channel || !message || !user) {
        cJSON_Delete(root); cJSON_Delete(login); cJSON_Delete(channel); cJSON_Delete(message); cJSON_Delete(user);
        return nullptr;
    }
    cJSON_AddItemToObject(root, "login", login);
    cJSON_AddItemToObject(root, "channel", channel);
    cJSON_AddItemToObject(root, "message", message);
    cJSON_AddItemToObject(root, "user", user);
    cJSON_AddStringToObject(root, "type", "message-created");
    cJSON_AddNumberToObject(root, "timestamp", static_cast<double>(create_time));
    cJSON_AddNumberToObject(login, "sn", login_sn);
    cJSON_AddStringToObject(channel, "id", talker ? talker : "");
    cJSON_AddNumberToObject(channel, "type", group ? 0 : 1);
    if (!cJSON_AddItemToObject(message, "channel", cJSON_Duplicate(channel, true))) {
        cJSON_Delete(root);
        return nullptr;
    }
    cJSON_AddStringToObject(message, "id", id ? id : "");
    cJSON_AddStringToObject(message, "content", escaped);
    cJSON_AddStringToObject(user, "id", sender[0] ? sender : talker ? talker : "");
    return root;
}

struct PollContext {
    const Store *store;
    int login_sn;
    bool (*emit)(void *context, const char *event);
    void *sink;
};

bool MessageRow(Wcdb *db, void *stmt, void *context) {
    auto *poll = static_cast<PollContext *>(context);
    const char *id = WcdbText(db, stmt, 1);
    const int type = static_cast<int>(WcdbInt(db, stmt, 2));
    const int is_send = static_cast<int>(WcdbInt(db, stmt, 3));
    const long long create_time = WcdbInt(db, stmt, 4);
    const char *talker = WcdbText(db, stmt, 5);
    const char *content = WcdbText(db, stmt, 6);
    // Only plain text and system notices are mapped for now; media is skipped, not faked.
    if (type != 1 && type != 10000) return true;
    cJSON *event = BuildEvent(*poll->store, id, is_send, create_time, talker, content, poll->login_sn);
    if (!event) return true;
    char *text = cJSON_PrintUnformatted(event);
    cJSON_Delete(event);
    if (!text) return false;
    const bool ok = poll->emit(poll->sink, text);
    free(text);
    return ok;
}

bool WatermarkRow(Wcdb *db, void *stmt, void *context) {
    *static_cast<long long *>(context) = WcdbIsNull(db, stmt, 0) ? -1 : WcdbInt(db, stmt, 0);
    return false;
}
} // namespace

Store *CreateStore(const char *path, const void *key, int key_size, int compatibility, const char *self_id) {
    return CreateStoreEx(kLibrary, path, key, key_size, compatibility, self_id);
}

Store *CreateStoreEx(const char *library, const char *path, const void *key, int key_size, int compatibility, const char *self_id) {
    if (!library || !path) return nullptr;
    auto *store = static_cast<Store *>(calloc(1, sizeof(Store)));
    if (!store) return nullptr;
    store->db = WcdbOpenEx(library, path, key, key_size, 0, compatibility, 0, 1);
    if (!store->db) {
        snprintf(store->error, sizeof(store->error), "open failed: %s", WcdbError(nullptr));
        free(store);
        return nullptr;
    }
    if (self_id) snprintf(store->self_id, sizeof(store->self_id), "%s", self_id);
    WcdbExec(store->db, "PRAGMA query_only = 1");
    return store;
}

void DestroyStore(Store *store) {
    if (!store) return;
    if (store->db) WcdbClose(store->db);
    free(store);
}

bool StoreReady(Store *store) { return store && store->db; }
const char *StoreError(Store *store) { return store ? store->error : "no store"; }

long long StoreWatermark(Store *store) {
    if (!store || !store->db) return -1;
    long long watermark = -1;
    if (!WcdbQuery(store->db, "SELECT max(rowid) FROM message", WatermarkRow, &watermark)) return -1;
    return watermark;
}

long long StorePoll(Store *store, long long since, int login_sn, bool (*emit)(void *context, const char *event), void *context) {
    if (!store || !store->db || !emit) return since;
    char sql[160];
    snprintf(sql, sizeof(sql),
             "SELECT rowid, msgId, type, isSend, createTime, talker, content FROM message "
             "WHERE rowid > %lld ORDER BY rowid ASC LIMIT %lld", since, kPollLimit);
    PollContext poll{store, login_sn, emit, context};
    WcdbQuery(store->db, sql, MessageRow, &poll);
    const long long watermark = StoreWatermark(store);
    return watermark > since ? watermark : since;
}
} // namespace satori
