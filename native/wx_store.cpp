#include "wx_store.h"
#include "wcdb.h"
#include "protocol.h"
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace satori {
namespace {
constexpr char kLibrary[] = "libWCDB.so";
constexpr long long kPollLimit = 50;
constexpr size_t kContentMax = 2000;
constexpr int kListMax = 50;
}

struct Store {
    Wcdb *db;
    char self_id[80];
    char error[256];
    pthread_mutex_t mutex;
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

bool TextType(int type) { return type == 1 || type == 10000; }

// Builds a Satori Message object: {id, content, timestamp, channel:{id,type}, user:{id}}.
cJSON *MessageObject(const Store &store, const char *id, int is_send, long long create_time,
                     const char *talker, const char *content) {
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
    cJSON *message = cJSON_CreateObject();
    cJSON *channel = cJSON_CreateObject();
    cJSON *user = cJSON_CreateObject();
    if (!message || !channel || !user) { cJSON_Delete(message); cJSON_Delete(channel); cJSON_Delete(user); return nullptr; }
    cJSON_AddItemToObject(message, "channel", channel);
    cJSON_AddItemToObject(message, "user", user);
    cJSON_AddStringToObject(message, "id", id ? id : "");
    cJSON_AddStringToObject(message, "content", escaped);
    cJSON_AddNumberToObject(message, "timestamp", static_cast<double>(create_time));
    cJSON_AddStringToObject(channel, "id", talker ? talker : "");
    cJSON_AddNumberToObject(channel, "type", group ? 0 : 1);
    cJSON_AddStringToObject(user, "id", sender[0] ? sender : (talker ? talker : ""));
    return message;
}

cJSON *BuildEvent(const Store &store, const char *id, int is_send, long long create_time,
                  const char *talker, const char *content, int login_sn) {
    cJSON *message = MessageObject(store, id, is_send, create_time, talker, content);
    if (!message) return nullptr;
    cJSON *root = cJSON_CreateObject();
    cJSON *login = cJSON_CreateObject();
    if (!root || !login) { cJSON_Delete(root); cJSON_Delete(login); cJSON_Delete(message); return nullptr; }
    cJSON_AddItemToObject(root, "login", login);
    cJSON_AddNumberToObject(login, "sn", login_sn);
    cJSON_AddStringToObject(root, "type", "message-created");
    cJSON_AddItemToObject(root, "message", message);
    cJSON *channel = cJSON_GetObjectItemCaseSensitive(message, "channel");
    cJSON *user = cJSON_GetObjectItemCaseSensitive(message, "user");
    if (!cJSON_AddItemToObject(root, "channel", cJSON_Duplicate(channel, true)) ||
        !cJSON_AddItemToObject(root, "user", cJSON_Duplicate(user, true))) {
        cJSON_Delete(root);
        return nullptr;
    }
    cJSON_AddNumberToObject(root, "timestamp", static_cast<double>(create_time));
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
    if (!TextType(type)) return true; // media is skipped, not faked
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

long long WatermarkLocked(Store *store) {
    long long watermark = -1;
    if (!WcdbQuery(store->db, "SELECT max(rowid) FROM message", WatermarkRow, &watermark)) return -1;
    return watermark;
}

struct ListContext {
    const Store *store;
    cJSON *data;
    long long cursor;
    long long rowids[kListMax + 1];
    int rows;
    int messages;
    int limit;
};

bool ListRow(Wcdb *db, void *stmt, void *context) {
    auto *list = static_cast<ListContext *>(context);
    const long long rowid = WcdbInt(db, stmt, 0);
    const char *id = WcdbText(db, stmt, 1);
    const int is_send = static_cast<int>(WcdbInt(db, stmt, 3));
    const long long create_time = WcdbInt(db, stmt, 4);
    const char *talker = WcdbText(db, stmt, 5);
    const char *content = WcdbText(db, stmt, 6);
    if (list->rows < list->limit + 1) list->rowids[list->rows] = rowid;
    cJSON *message = MessageObject(*list->store, id, is_send, create_time, talker, content);
    if (message) { cJSON_AddItemToArray(list->data, message); ++list->messages; }
    ++list->rows;
    return list->rows <= list->limit;
}

bool SafeSql(const char *text) { return text && *text && !strchr(text, '\'') && !strchr(text, '\\') && !strchr(text, '"'); }
} // namespace

Store *CreateStore(const char *path, const void *key, int key_size, int compatibility, const char *self_id) {
    return CreateStoreEx(kLibrary, path, key, key_size, compatibility, self_id);
}

Store *CreateStoreEx(const char *library, const char *path, const void *key, int key_size, int compatibility, const char *self_id) {
    if (!library || !path) return nullptr;
    auto *store = static_cast<Store *>(calloc(1, sizeof(Store)));
    if (!store) return nullptr;
    if (pthread_mutex_init(&store->mutex, nullptr)) { free(store); return nullptr; }
    store->db = WcdbOpenEx(library, path, key, key_size, 0, compatibility, 0, 1);
    if (!store->db) {
        snprintf(store->error, sizeof(store->error), "open failed: %s", WcdbError(nullptr));
        pthread_mutex_destroy(&store->mutex);
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
    pthread_mutex_destroy(&store->mutex);
    free(store);
}

bool StoreReady(Store *store) { return store && store->db; }
const char *StoreError(Store *store) { return store ? store->error : "no store"; }

long long StoreWatermark(Store *store) {
    if (!store || !store->db) return -1;
    pthread_mutex_lock(&store->mutex);
    const long long watermark = WatermarkLocked(store);
    pthread_mutex_unlock(&store->mutex);
    return watermark;
}

long long StorePoll(Store *store, long long since, int login_sn, bool (*emit)(void *context, const char *event), void *context) {
    if (!store || !store->db || !emit) return since;
    pthread_mutex_lock(&store->mutex);
    char sql[160];
    snprintf(sql, sizeof(sql),
             "SELECT rowid, msgId, type, isSend, createTime, talker, content FROM message "
             "WHERE rowid > %lld ORDER BY rowid ASC LIMIT %lld", since, kPollLimit);
    PollContext poll{store, login_sn, emit, context};
    WcdbQuery(store->db, sql, MessageRow, &poll);
    const long long watermark = WatermarkLocked(store);
    pthread_mutex_unlock(&store->mutex);
    return watermark > since ? watermark : since;
}

cJSON *StoreMessageList(Store *store, const char *channel_id, const char *next, int limit) {
    if (!store || !store->db || !SafeSql(channel_id)) return nullptr;
    if (limit < 1) limit = 20;
    if (limit > kListMax) limit = kListMax;
    long long cursor = -1;
    if (next && *next) {
        char *end = nullptr;
        cursor = strtoll(next, &end, 10);
        if (!end || *end || cursor < 0) return nullptr;
    }
    pthread_mutex_lock(&store->mutex);
    char sql[512];
    if (cursor > 0)
        snprintf(sql, sizeof(sql),
                 "SELECT rowid, msgId, type, isSend, createTime, talker, content FROM message "
                 "WHERE talker = '%s' AND type IN (1,10000) AND rowid < %lld ORDER BY rowid DESC LIMIT %d",
                 channel_id, cursor, limit + 1);
    else
        snprintf(sql, sizeof(sql),
                 "SELECT rowid, msgId, type, isSend, createTime, talker, content FROM message "
                 "WHERE talker = '%s' AND type IN (1,10000) ORDER BY rowid DESC LIMIT %d", channel_id, limit + 1);
    cJSON *result = cJSON_CreateObject();
    cJSON *data = cJSON_CreateArray();
    if (!result || !data) { cJSON_Delete(result); cJSON_Delete(data); pthread_mutex_unlock(&store->mutex); return nullptr; }
    cJSON_AddItemToObject(result, "data", data);
    ListContext list{store, data, 0, {}, 0, 0, limit};
    // The +1 row is fetched only to know whether another page exists.
    const bool ok = WcdbQuery(store->db, sql, ListRow, &list);
    if (ok && list.messages > limit) {
        cJSON_DeleteItemFromArray(data, list.messages - 1);
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%lld", list.rowids[limit - 1]);
        cJSON_AddStringToObject(result, "next", buffer);
    }
    pthread_mutex_unlock(&store->mutex);
    return result;
}

cJSON *StoreMessageGet(Store *store, const char *channel_id, const char *message_id) {
    if (!store || !store->db || !SafeSql(channel_id) || !SafeSql(message_id)) return nullptr;
    pthread_mutex_lock(&store->mutex);
    char sql[512];
    snprintf(sql, sizeof(sql),
             "SELECT rowid, msgId, type, isSend, createTime, talker, content FROM message "
             "WHERE talker = '%s' AND msgId = '%s' LIMIT 1", channel_id, message_id);
    struct Context { const Store *store; cJSON *message; };
    Context context{store, nullptr};
    WcdbRow row = [](Wcdb *db, void *stmt, void *raw) -> bool {
        auto *ctx = static_cast<Context *>(raw);
        const int type = static_cast<int>(WcdbInt(db, stmt, 2));
        if (!TextType(type)) return false;
        ctx->message = MessageObject(*ctx->store, WcdbText(db, stmt, 1), static_cast<int>(WcdbInt(db, stmt, 3)),
                                     WcdbInt(db, stmt, 4), WcdbText(db, stmt, 5), WcdbText(db, stmt, 6));
        return false;
    };
    WcdbQuery(store->db, sql, row, &context);
    pthread_mutex_unlock(&store->mutex);
    return context.message;
}

namespace {
cJSON *UserObject(const char *username, const char *alias, const char *remark, const char *nickname) {
    cJSON *user = cJSON_CreateObject();
    if (!user) return nullptr;
    cJSON_AddStringToObject(user, "id", username ? username : "");
    const char *name = (remark && *remark) ? remark : alias;
    if (name && *name) cJSON_AddStringToObject(user, "name", name);
    if (nickname && *nickname) cJSON_AddStringToObject(user, "nick", nickname);
    return user;
}

const char *ContactName(const char *alias, const char *remark, const char *nickname) {
    if (remark && *remark) return remark;
    if (nickname && *nickname) return nickname;
    return alias ? alias : "";
}

// kind: 1 = friend List<Friend>, 0 = guild, 2 = channel.
struct ContactContext {
    cJSON *data;
    long long rowids[64];
    int rows;
    int limit;
    int kind;
};

bool ContactRow(Wcdb *db, void *stmt, void *context) {
    auto *list = static_cast<ContactContext *>(context);
    const char *username = WcdbText(db, stmt, 0);
    const char *alias = WcdbText(db, stmt, 1);
    const char *remark = WcdbText(db, stmt, 2);
    const char *nickname = WcdbText(db, stmt, 3);
    if (list->rows < list->limit + 1) list->rowids[list->rows] = WcdbInt(db, stmt, 4);
    if (list->kind == 1) {
        cJSON *entry = cJSON_CreateObject();
        cJSON *user = UserObject(username, alias, remark, nickname);
        if (entry && user) {
            cJSON_AddItemToObject(entry, "user", user);
            if (nickname && *nickname) cJSON_AddStringToObject(entry, "nick", nickname);
            cJSON_AddItemToArray(list->data, entry);
        } else { cJSON_Delete(entry); cJSON_Delete(user); }
    } else {
        cJSON *object = cJSON_CreateObject();
        if (object) {
            cJSON_AddStringToObject(object, "id", username ? username : "");
            const char *name = ContactName(alias, remark, nickname);
            if (name && *name) cJSON_AddStringToObject(object, "name", name);
            if (list->kind == 2) cJSON_AddNumberToObject(object, "type",
                username && strstr(username, "@chatroom") ? 0 : 1);
            cJSON_AddItemToArray(list->data, object);
        }
    }
    ++list->rows;
    return list->rows <= list->limit;
}

cJSON *ContactList(Store *store, const char *sql, int kind, int limit) {
    cJSON *result = cJSON_CreateObject();
    cJSON *data = cJSON_CreateArray();
    if (!result || !data) { cJSON_Delete(result); cJSON_Delete(data); return nullptr; }
    cJSON_AddItemToObject(result, "data", data);
    if (limit < 1) limit = 50;
    if (limit > 60) limit = 60;
    ContactContext list{data, {}, 0, limit, kind};
    const bool ok = WcdbQuery(store->db, sql, ContactRow, &list);
    if (ok && list.rows > limit) {
        const int index = cJSON_GetArraySize(data);
        if (index > 0) cJSON_DeleteItemFromArray(data, index - 1);
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%lld", list.rowids[limit - 1]);
        cJSON_AddStringToObject(result, "next", buffer);
    }
    return result;
}

struct OneContext { cJSON *object; int kind; };

bool OneRow(Wcdb *db, void *stmt, void *context) {
    auto *one = static_cast<OneContext *>(context);
    const char *username = WcdbText(db, stmt, 0);
    const char *alias = WcdbText(db, stmt, 1);
    const char *remark = WcdbText(db, stmt, 2);
    const char *nickname = WcdbText(db, stmt, 3);
    if (one->kind == 1) {
        one->object = UserObject(username, alias, remark, nickname);
    } else {
        cJSON *object = cJSON_CreateObject();
        if (object) {
            cJSON_AddStringToObject(object, "id", username ? username : "");
            const char *name = ContactName(alias, remark, nickname);
            if (name && *name) cJSON_AddStringToObject(object, "name", name);
            if (one->kind == 2) cJSON_AddNumberToObject(object, "type",
                username && strstr(username, "@chatroom") ? 0 : 1);
        }
        one->object = object;
    }
    return false;
}

cJSON *OneContact(Store *store, const char *id, int kind) {
    if (!store || !store->db || !SafeSql(id)) return nullptr;
    pthread_mutex_lock(&store->mutex);
    char sql[400];
    snprintf(sql, sizeof(sql), "SELECT username, alias, conRemark, nickname FROM rcontact WHERE username = '%s' LIMIT 1", id);
    OneContext context{nullptr, kind};
    WcdbQuery(store->db, sql, OneRow, &context);
    pthread_mutex_unlock(&store->mutex);
    return context.object;
}

bool Cursor(const char *next, long long *cursor) {
    *cursor = 0;
    if (!next || !*next) return true;
    char *end = nullptr;
    *cursor = strtoll(next, &end, 10);
    return end && !*end && *cursor > 0;
}
} // namespace

cJSON *StoreUserGet(Store *store, const char *user_id) { return OneContact(store, user_id, 1); }
cJSON *StoreGuildGet(Store *store, const char *guild_id) { return OneContact(store, guild_id, 0); }

cJSON *StoreChannelGet(Store *store, const char *channel_id) {
    cJSON *channel = OneContact(store, channel_id, 2);
    if (channel) return channel;
    // Unknown id: still describe it as a direct channel rather than failing.
    if (!store || !store->db || !SafeSql(channel_id)) return nullptr;
    cJSON *object = cJSON_CreateObject();
    if (!object) return nullptr;
    cJSON_AddStringToObject(object, "id", channel_id);
    cJSON_AddNumberToObject(object, "type", strstr(channel_id, "@chatroom") ? 0 : 1);
    return object;
}

cJSON *StoreFriendList(Store *store, const char *next, int limit) {
    if (!store || !store->db) return nullptr;
    long long cursor = 0;
    if (!Cursor(next, &cursor)) return nullptr;
    pthread_mutex_lock(&store->mutex);
    char sql[640];
    if (cursor > 0)
        snprintf(sql, sizeof(sql), "SELECT username, alias, conRemark, nickname, rowid FROM rcontact "
                 "WHERE type = 3 AND deleteFlag = 0 AND username NOT LIKE '%%@chatroom' AND username NOT LIKE 'gh_%%' "
                 "AND username != '%s' AND rowid < %lld ORDER BY rowid LIMIT %d", store->self_id, cursor, limit + 1);
    else
        snprintf(sql, sizeof(sql), "SELECT username, alias, conRemark, nickname, rowid FROM rcontact "
                 "WHERE type = 3 AND deleteFlag = 0 AND username NOT LIKE '%%@chatroom' AND username NOT LIKE 'gh_%%' "
                 "AND username != '%s' ORDER BY rowid LIMIT %d", store->self_id, limit + 1);
    cJSON *result = ContactList(store, sql, 1, limit);
    pthread_mutex_unlock(&store->mutex);
    return result;
}

cJSON *StoreGuildList(Store *store, const char *next, int limit) {
    if (!store || !store->db) return nullptr;
    long long cursor = 0;
    if (!Cursor(next, &cursor)) return nullptr;
    pthread_mutex_lock(&store->mutex);
    char sql[640];
    if (cursor > 0)
        snprintf(sql, sizeof(sql), "SELECT username, alias, conRemark, nickname, rowid FROM rcontact "
                 "WHERE username LIKE '%%@chatroom' AND deleteFlag = 0 AND rowid < %lld ORDER BY rowid LIMIT %d", cursor, limit + 1);
    else
        snprintf(sql, sizeof(sql), "SELECT username, alias, conRemark, nickname, rowid FROM rcontact "
                 "WHERE username LIKE '%%@chatroom' AND deleteFlag = 0 ORDER BY rowid LIMIT %d", limit + 1);
    cJSON *result = ContactList(store, sql, 0, limit);
    pthread_mutex_unlock(&store->mutex);
    return result;
}

cJSON *StoreChannelList(Store *store, const char *guild_id, const char *next, int limit) {
    (void)next;
    (void)limit;
    cJSON *channel = StoreChannelGet(store, guild_id);
    if (!channel) return nullptr;
    cJSON *result = cJSON_CreateObject();
    cJSON *data = cJSON_CreateArray();
    if (!result || !data) { cJSON_Delete(result); cJSON_Delete(data); cJSON_Delete(channel); return nullptr; }
    cJSON_AddItemToObject(result, "data", data);
    cJSON_AddItemToArray(data, channel);
    return result;
}
} // namespace satori
