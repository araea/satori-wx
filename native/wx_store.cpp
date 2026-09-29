#include "wx_store.h"
#include "media.h"
#include "protocol.h"
#include "textbuf.h"
#include "wcdb.h"
#include "wx_message.h"
#include "xml_lite.h"
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace satori {
namespace {
constexpr char kLibrary[] = "libWCDB.so";
constexpr int kPollLimit = 50;
constexpr int kBatch = 100;
constexpr int kListMax = 50;
}

struct Store {
    Wcdb *db;
    char self_id[80];
    char error[256];
    char account_dir[1100];  // <app>/MicroMsg/<hash>: where WeChat keeps this account's media
    char app_dir[1100];      // <app>: the app's private data directory
    long long skipped;       // rows the poller could not turn into an event (too large)
    // Authors of the last few hundred announced messages, so a later recall can name who wrote
    // the message (the recalled row itself no longer says).
    struct { long long id; char user[80]; } authors[512];
    unsigned author_next;
    pthread_mutex_t mutex;
    // The module's own sends (see StoreSendBegin): open message.create windows per talker, and the
    // local ids of the last messages it created. Guarded by send_mutex, never held with `mutex`.
    struct { char talker[80]; int busy; long long until_ms; } sending[8];
    long long sent_ids[64];
    unsigned sent_next;
    pthread_mutex_t send_mutex;
};

namespace {
bool WatermarkRow(Wcdb *db, void *stmt, void *context) {
    *static_cast<long long *>(context) = WcdbIsNull(db, stmt, 0) ? -1 : WcdbInt(db, stmt, 0);
    return false;
}

long long WatermarkLocked(Store *store) {
    long long watermark = -1;
    if (!WcdbQuery(store->db, "SELECT max(rowid) FROM message", WatermarkRow, &watermark)) return -1;
    return watermark;
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
    if (pthread_mutex_init(&store->send_mutex, nullptr)) { pthread_mutex_destroy(&store->mutex); free(store); return nullptr; }
    store->db = WcdbOpenEx(library, path, key, key_size, 0, compatibility, 0, 1);
    if (!store->db) {
        snprintf(store->error, sizeof(store->error), "open failed: %s", WcdbError(nullptr));
        pthread_mutex_destroy(&store->mutex);
        pthread_mutex_destroy(&store->send_mutex);
        free(store);
        return nullptr;
    }
    if (self_id) snprintf(store->self_id, sizeof(store->self_id), "%s", self_id);
    // <app>/MicroMsg/<hash>/EnMicroMsg.db -> its directory, and the app directory above MicroMsg.
    const char *slash = strrchr(path, '/');
    if (slash && static_cast<size_t>(slash - path) < sizeof(store->account_dir)) {
        snprintf(store->account_dir, sizeof(store->account_dir), "%.*s", static_cast<int>(slash - path), path);
        const char *marker = strstr(store->account_dir, "/MicroMsg/");
        if (marker) snprintf(store->app_dir, sizeof(store->app_dir), "%.*s", static_cast<int>(marker - store->account_dir), store->account_dir);
    }
    WcdbExec(store->db, "PRAGMA query_only = 1");
    return store;
}

void DestroyStore(Store *store) {
    if (!store) return;
    if (store->db) WcdbClose(store->db);
    pthread_mutex_destroy(&store->mutex);
    pthread_mutex_destroy(&store->send_mutex);
    free(store);
}

namespace {
long long MonotonicMs() {
    timespec now{};
    clock_gettime(CLOCK_MONOTONIC, &now);
    return static_cast<long long>(now.tv_sec) * 1000 + now.tv_nsec / 1000000;
}
// How long after a message.create ends its rows still count as the module's: the poller is woken
// by the database write within milliseconds, but WeChat can be frozen for seconds in between.
constexpr long long kSendGraceMs = 5000;

// Slot for `talker`: the one already open, else a free or expired one, else the oldest.
int SendSlot(Store *store, const char *talker, long long now) {
    int free_slot = -1, oldest = 0;
    for (int i = 0; i < 8; ++i) {
        auto &slot = store->sending[i];
        if (!strcmp(slot.talker, talker)) return i;
        if (free_slot < 0 && !slot.busy && slot.until_ms <= now) free_slot = i;
        if (slot.until_ms < store->sending[oldest].until_ms) oldest = i;
    }
    return free_slot >= 0 ? free_slot : oldest;
}

bool SentByModule(Store *store, const char *talker, long long id) {
    const long long now = MonotonicMs();
    bool mine = false;
    pthread_mutex_lock(&store->send_mutex);
    for (int i = 0; i < 64 && !mine; ++i) mine = store->sent_ids[i] == id;
    for (int i = 0; i < 8 && !mine; ++i) {
        const auto &slot = store->sending[i];
        mine = !strcmp(slot.talker, talker) && (slot.busy > 0 || now < slot.until_ms);
    }
    pthread_mutex_unlock(&store->send_mutex);
    return mine;
}
} // namespace

void StoreSendBegin(Store *store, const char *talker) {
    if (!store || !talker || strlen(talker) >= sizeof(store->sending[0].talker)) return;
    pthread_mutex_lock(&store->send_mutex);
    auto &slot = store->sending[SendSlot(store, talker, MonotonicMs())];
    if (strcmp(slot.talker, talker)) { snprintf(slot.talker, sizeof(slot.talker), "%s", talker); slot.busy = 0; }
    ++slot.busy;
    pthread_mutex_unlock(&store->send_mutex);
}

void StoreSendEnd(Store *store, const char *talker) {
    if (!store || !talker) return;
    pthread_mutex_lock(&store->send_mutex);
    for (int i = 0; i < 8; ++i) {
        auto &slot = store->sending[i];
        if (strcmp(slot.talker, talker)) continue;
        if (slot.busy > 0) --slot.busy;
        slot.until_ms = MonotonicMs() + kSendGraceMs;
        break;
    }
    pthread_mutex_unlock(&store->send_mutex);
}

void StoreNoteSent(Store *store, long long local_id) {
    if (!store || local_id <= 0) return;
    pthread_mutex_lock(&store->send_mutex);
    store->sent_ids[store->sent_next++ % 64] = local_id;
    pthread_mutex_unlock(&store->send_mutex);
}

bool StoreReady(Store *store) { return store && store->db; }
const char *StoreError(Store *store) { return store ? store->error : "no store"; }
const char *StoreSelfId(Store *store) { return store ? store->self_id : ""; }

long long StoreWatermark(Store *store) {
    if (!store || !store->db) return -1;
    pthread_mutex_lock(&store->mutex);
    const long long watermark = WatermarkLocked(store);
    pthread_mutex_unlock(&store->mutex);
    return watermark;
}

namespace {
// The public head-image URL WeChat cached for a contact (`img_flag`: reserved2 is the large
// picture, reserved1 the small one). Caller holds the store lock. False when there is none, or
// when the table does not exist: an avatar is never worth failing a lookup over.
struct AvatarContext { char *out; size_t capacity; bool found; };
bool AvatarRow(Wcdb *db, void *stmt, void *context) {
    auto *avatar = static_cast<AvatarContext *>(context);
    for (int column = 0; column < 2; ++column) {
        const char *url = WcdbText(db, stmt, column);
        if (url && (!strncmp(url, "http://", 7) || !strncmp(url, "https://", 8)) && strlen(url) < avatar->capacity) {
            memcpy(avatar->out, url, strlen(url) + 1);
            avatar->found = true;
            break;
        }
    }
    return false;
}
bool AvatarLocked(Store *store, const char *username, char *out, size_t capacity) {
    out[0] = 0;
    if (!SafeSql(username)) return false;
    char sql[300];
    snprintf(sql, sizeof(sql), "SELECT reserved2, reserved1 FROM img_flag WHERE username = '%s' LIMIT 1", username);
    AvatarContext context{out, capacity, false};
    WcdbQuery(store->db, sql, AvatarRow, &context);
    return context.found;
}

cJSON *UserObject(const char *username, const char *alias, const char *remark, const char *nickname, const char *avatar = nullptr) {
    cJSON *user = cJSON_CreateObject();
    if (!user) return nullptr;
    cJSON_AddStringToObject(user, "id", username ? username : "");
    const char *name = (remark && *remark) ? remark : alias;
    if (name && *name) cJSON_AddStringToObject(user, "name", name);
    if (nickname && *nickname) cJSON_AddStringToObject(user, "nick", nickname);
    if (avatar && *avatar) cJSON_AddStringToObject(user, "avatar", avatar);
    return user;
}

const char *ContactName(const char *alias, const char *remark, const char *nickname) {
    if (remark && *remark) return remark;
    if (nickname && *nickname) return nickname;
    return alias ? alias : "";
}

// kind: 1 = friend List<Friend>, 0 = guild, 2 = channel.
struct ContactContext {
    Store *store;
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
        char avatar[512];
        AvatarLocked(list->store, username, avatar, sizeof(avatar));
        cJSON *user = UserObject(username, alias, remark, nickname, avatar);
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
    ContactContext list{store, data, {}, 0, limit, kind};
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

struct OneContext { Store *store; cJSON *object; int kind; };

bool OneRow(Wcdb *db, void *stmt, void *context) {
    auto *one = static_cast<OneContext *>(context);
    const char *username = WcdbText(db, stmt, 0);
    const char *alias = WcdbText(db, stmt, 1);
    const char *remark = WcdbText(db, stmt, 2);
    const char *nickname = WcdbText(db, stmt, 3);
    if (one->kind == 1) {
        char avatar[512];
        AvatarLocked(one->store, username, avatar, sizeof(avatar));
        one->object = UserObject(username, alias, remark, nickname, avatar);
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
    OneContext context{store, nullptr, kind};
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
                 "WHERE (type & 3) = 3 AND deleteFlag = 0 AND username NOT LIKE '%%@chatroom' AND username NOT LIKE 'gh_%%' "
                 "AND username != '%s' AND rowid > %lld ORDER BY rowid LIMIT %d", store->self_id, cursor, limit + 1);
    else
        snprintf(sql, sizeof(sql), "SELECT username, alias, conRemark, nickname, rowid FROM rcontact "
                 "WHERE (type & 3) = 3 AND deleteFlag = 0 AND username NOT LIKE '%%@chatroom' AND username NOT LIKE 'gh_%%' "
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
                 "WHERE username LIKE '%%@chatroom' AND deleteFlag = 0 AND rowid > %lld ORDER BY rowid LIMIT %d", cursor, limit + 1);
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

namespace {
constexpr int kMemberMax = 512; // WeChat group member ceiling
constexpr char kNameSeparator[] = "\xE3\x80\x81"; // U+3001, WeChat's member display-name joiner

// A chatroom row, copied out of the statement so the lock can be released before use.
struct Chatroom {
    char *memberlist;
    char *displayname;
    char *roomdata;
    int roomdata_size;
    char roomowner[80];
};

void FreeChatroom(Chatroom *room) {
    free(room->memberlist);
    free(room->displayname);
    free(room->roomdata);
    room->memberlist = nullptr;
    room->displayname = nullptr;
    room->roomdata = nullptr;
    room->roomdata_size = 0;
}

// Caller owns the copies inside *room and must call FreeChatroom. Locks internally.
bool LoadChatroom(Store *store, const char *guild_id, Chatroom *room) {
    room->memberlist = nullptr;
    room->displayname = nullptr;
    room->roomdata = nullptr;
    room->roomdata_size = 0;
    room->roomowner[0] = 0;
    if (!store || !store->db || !SafeSql(guild_id)) return false;
    pthread_mutex_lock(&store->mutex);
    char sql[400];
    snprintf(sql, sizeof(sql),
             "SELECT memberlist, displayname, roomowner, roomdata FROM chatroom WHERE chatroomname = '%s' LIMIT 1", guild_id);
    struct Context { Chatroom *room; int found; };
    Context context{room, 0};
    WcdbRow row = [](Wcdb *db, void *stmt, void *raw) -> bool {
        auto *ctx = static_cast<Context *>(raw);
        const char *members = WcdbText(db, stmt, 0);
        const char *names = WcdbText(db, stmt, 1);
        const char *owner = WcdbText(db, stmt, 2);
        int blob_size = 0;
        const void *blob = WcdbBlob(db, stmt, 3, &blob_size);
        if (members && *members) {
            const size_t size = strlen(members);
            ctx->room->memberlist = static_cast<char *>(malloc(size + 1));
            if (ctx->room->memberlist) memcpy(ctx->room->memberlist, members, size + 1);
        }
        if (names && *names) {
            const size_t size = strlen(names);
            ctx->room->displayname = static_cast<char *>(malloc(size + 1));
            if (ctx->room->displayname) memcpy(ctx->room->displayname, names, size + 1);
        }
        // The member flags live in this protobuf blob; keep a private copy because the
        // statement is stepped again before the caller gets to read it.
        if (blob && blob_size > 0) {
            ctx->room->roomdata = static_cast<char *>(malloc(size_t(blob_size)));
            if (ctx->room->roomdata) {
                memcpy(ctx->room->roomdata, blob, size_t(blob_size));
                ctx->room->roomdata_size = blob_size;
            }
        }
        if (owner) snprintf(ctx->room->roomowner, sizeof(ctx->room->roomowner), "%s", owner);
        ctx->found = 1;
        return false;
    };
    WcdbQuery(store->db, sql, row, &context);
    pthread_mutex_unlock(&store->mutex);
    if (!context.found) { FreeChatroom(room); return false; }
    return true;
}

// Splits in place and returns borrowed pointers into the mutated buffer.
int SplitMembers(char *list, const char **out, int max) {
    int count = 0;
    char *cursor = list;
    while (cursor && *cursor && count < max) {
        char *end = strchr(cursor, ';');
        if (end) *end = 0;
        out[count++] = cursor;
        cursor = end ? end + 1 : nullptr;
    }
    return count;
}

int SplitNames(char *names, const char **out, int max) {
    int count = 0;
    if (!names) return 0;
    char *cursor = names;
    while (*cursor && count < max) {
        char *end = strstr(cursor, kNameSeparator);
        if (end) *end = 0;
        out[count++] = cursor;
        if (!end) break;
        cursor = end + sizeof(kNameSeparator) - 1;
    }
    return count;
}

// ---- roomdata: the per-member flag bitfield -------------------------------------------
// WeChat caches each chatroom member's flags in the `chatroom.roomdata` protobuf:
//   ChatRoomData  { repeated ChatRoomMember member = 1; ... }
//   ChatRoomMember{ string userName = 1; ... int32 flag = 3; ... }
// Bit 2048 of `flag` is the group-admin bit the app itself tests. The blob is small and
// fully local, so a tiny hand-rolled reader keeps this out of the (unreadable) app code.

bool ProtoVarint(const unsigned char *bytes, size_t size, size_t *cursor, uint64_t *out) {
    uint64_t value = 0;
    int shift = 0;
    while (*cursor < size) {
        const unsigned char byte = bytes[(*cursor)++];
        value |= static_cast<uint64_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80)) { *out = value; return true; }
        shift += 7;
        if (shift > 63) return false;
    }
    return false;
}

// Advances past a field of the given wire type. False on malformed or truncated input.
bool ProtoSkip(const unsigned char *bytes, size_t size, size_t *cursor, unsigned wire) {
    uint64_t length = 0;
    switch (wire) {
        case 0: return ProtoVarint(bytes, size, cursor, &length);
        case 1: *cursor += 8; break;
        case 2:
            if (!ProtoVarint(bytes, size, cursor, &length) || *cursor > size ||
                length > size - *cursor) return false;
            *cursor += static_cast<size_t>(length);
            break;
        case 5: *cursor += 4; break;
        default: return false;
    }
    return *cursor <= size;
}

// One ChatRoomMember: true when it is `user_id`; `admin` then reflects bit 2048.
bool ProtoMember(const unsigned char *bytes, size_t size, const char *user_id, bool *admin) {
    const char *name = nullptr;
    size_t name_size = 0, cursor = 0;
    uint64_t flags = 0;
    bool has_flags = false;
    while (cursor < size) {
        uint64_t key = 0;
        if (!ProtoVarint(bytes, size, &cursor, &key)) return false;
        const unsigned field = static_cast<unsigned>(key >> 3), wire = static_cast<unsigned>(key & 7);
        if (field == 1 && wire == 2) {
            uint64_t length = 0;
            if (!ProtoVarint(bytes, size, &cursor, &length) || length > size - cursor) return false;
            name = reinterpret_cast<const char *>(bytes + cursor);
            name_size = static_cast<size_t>(length);
            cursor += name_size;
        } else if (field == 3 && wire == 0) {
            if (!ProtoVarint(bytes, size, &cursor, &flags)) return false;
            has_flags = true;
        } else if (!ProtoSkip(bytes, size, &cursor, wire)) {
            return false;
        }
    }
    if (!name || name_size != strlen(user_id) || memcmp(name, user_id, name_size)) return false;
    *admin = has_flags && (flags & 2048) != 0;
    return true;
}

// Whether `user_id` carries the admin bit in this roomdata blob. A member without an entry
// (or without flags) is simply not an admin.
bool RoomAdmin(const char *data, int size, const char *user_id) {
    if (!data || size <= 0 || !user_id || !*user_id) return false;
    const auto *bytes = reinterpret_cast<const unsigned char *>(data);
    const size_t total = static_cast<size_t>(size);
    size_t cursor = 0;
    while (cursor < total) {
        uint64_t key = 0;
        if (!ProtoVarint(bytes, total, &cursor, &key)) return false;
        const unsigned field = static_cast<unsigned>(key >> 3), wire = static_cast<unsigned>(key & 7);
        if (field == 1 && wire == 2) {
            uint64_t length = 0;
            if (!ProtoVarint(bytes, total, &cursor, &length) || length > total - cursor) return false;
            bool admin = false;
            if (ProtoMember(bytes + cursor, static_cast<size_t>(length), user_id, &admin)) return admin;
            cursor += static_cast<size_t>(length);
        } else if (!ProtoSkip(bytes, total, &cursor, wire)) {
            return false;
        }
    }
    return false;
}

// A Satori User for a member id; falls back to {"id": ...} when rcontact has no row.
cJSON *MemberUser(Store *store, const char *id) {
    cJSON *user = OneContact(store, id, 1);
    if (user) return user;
    user = cJSON_CreateObject();
    if (user) cJSON_AddStringToObject(user, "id", id ? id : "");
    return user;
}

cJSON *MemberObject(Store *store, const char *id, const char *group_name) {
    cJSON *member = cJSON_CreateObject();
    if (!member) return nullptr;
    cJSON *user = MemberUser(store, id);
    if (user) cJSON_AddItemToObject(member, "user", user);
    // The in-group alias is what the group actually shows; keep the contact name on `user`.
    if (group_name && *group_name) cJSON_AddStringToObject(member, "nick", group_name);
    return member;
}

cJSON *RoleObject(const char *id, const char *name) {
    cJSON *role = cJSON_CreateObject();
    if (role) {
        cJSON_AddStringToObject(role, "id", id);
        cJSON_AddStringToObject(role, "name", name);
    }
    return role;
}

cJSON *RoleList(const char *id, const char *name) {
    cJSON *result = cJSON_CreateObject();
    cJSON *data = cJSON_CreateArray();
    if (!result || !data) { cJSON_Delete(result); cJSON_Delete(data); return nullptr; }
    cJSON_AddItemToObject(result, "data", data);
    if (id) cJSON_AddItemToArray(data, RoleObject(id, name));
    return result;
}
} // namespace

cJSON *StoreGuildMemberList(Store *store, const char *guild_id, const char *next, int limit) {
    if (!store || !store->db) return nullptr;
    if (limit < 1) limit = 50;
    if (limit > kListMax) limit = kListMax;
    long long offset = 0;
    if (next && *next) {
        char *end = nullptr;
        offset = strtoll(next, &end, 10);
        if (!end || *end || offset < 0) return nullptr;
    }
    Chatroom room{};
    if (!LoadChatroom(store, guild_id, &room)) return nullptr;
    const char *ids[kMemberMax];
    const char *names[kMemberMax];
    const int total = SplitMembers(room.memberlist, ids, kMemberMax);
    const int named = SplitNames(room.displayname, names, kMemberMax);
    // The display-name list is positional; only trust it when it lines up with memberlist.
    const bool aligned = named == total;
    cJSON *result = cJSON_CreateObject();
    cJSON *data = cJSON_CreateArray();
    if (!result || !data) { cJSON_Delete(result); cJSON_Delete(data); FreeChatroom(&room); return nullptr; }
    cJSON_AddItemToObject(result, "data", data);
    int index = static_cast<int>(offset);
    int added = 0;
    for (; index < total && added < limit; ++index) {
        cJSON *member = MemberObject(store, ids[index], aligned ? names[index] : nullptr);
        if (!member) continue;
        cJSON_AddItemToArray(data, member);
        ++added;
    }
    if (index < total) {
        char buffer[32];
        snprintf(buffer, sizeof(buffer), "%d", index);
        cJSON_AddStringToObject(result, "next", buffer);
    }
    FreeChatroom(&room);
    return result;
}

cJSON *StoreGuildMemberGet(Store *store, const char *guild_id, const char *user_id) {
    if (!store || !store->db || !SafeSql(user_id)) return nullptr;
    Chatroom room{};
    if (!LoadChatroom(store, guild_id, &room)) return nullptr;
    const char *ids[kMemberMax];
    const char *names[kMemberMax];
    const int total = SplitMembers(room.memberlist, ids, kMemberMax);
    const int named = SplitNames(room.displayname, names, kMemberMax);
    const bool aligned = named == total;
    cJSON *member = nullptr;
    for (int i = 0; i < total; ++i) {
        if (!strcmp(ids[i], user_id)) {
            member = MemberObject(store, user_id, aligned ? names[i] : nullptr);
            break;
        }
    }
    FreeChatroom(&room);
    return member;
}

cJSON *StoreGuildRoleList(Store *store, const char *guild_id) {
    Chatroom room{};
    if (!LoadChatroom(store, guild_id, &room)) return nullptr;
    FreeChatroom(&room);
    cJSON *result = cJSON_CreateObject();
    cJSON *data = cJSON_CreateArray();
    if (!result || !data) { cJSON_Delete(result); cJSON_Delete(data); return nullptr; }
    cJSON_AddItemToObject(result, "data", data);
    cJSON_AddItemToArray(data, RoleObject("owner", "群主"));
    cJSON_AddItemToArray(data, RoleObject("admin", "管理员"));
    cJSON_AddItemToArray(data, RoleObject("member", "成员"));
    return result;
}

cJSON *StoreMemberRoleList(Store *store, const char *guild_id, const char *user_id) {
    if (!store || !store->db || !SafeSql(user_id)) return nullptr;
    Chatroom room{};
    if (!LoadChatroom(store, guild_id, &room)) return nullptr;
    const bool owner = !strcmp(room.roomowner, user_id);
    // Read the admin bit before splitting mutates the member list.
    const bool admin = !owner && RoomAdmin(room.roomdata, room.roomdata_size, user_id);
    const char *ids[kMemberMax];
    const int total = SplitMembers(room.memberlist, ids, kMemberMax);
    bool present = false;
    for (int i = 0; i < total; ++i) {
        if (!strcmp(ids[i], user_id)) { present = true; break; }
    }
    FreeChatroom(&room);
    if (!present) return RoleList(nullptr, nullptr);
    if (owner) return RoleList("owner", "群主");
    return admin ? RoleList("admin", "管理员") : RoleList("member", "成员");
}
} // namespace satori

// ==== messages ===============================================================================
// Rows are copied out of the statement (under the store lock) and only then decoded, because
// building a Satori message needs more lookups (contact, group member, quoted message) and the
// lock is not recursive. Nothing below may be called with the lock held.
namespace satori {
namespace {
constexpr char kRowColumns[] = "rowid, msgId, msgSvrId, type, isSend, createTime, talker, content, imgPath, lvbuffer";

struct Row {
    long long rowid, id, svr_id, create_time;
    int type, is_send;
    char *talker, *content, *img_path, *msg_source;
};

void FreeRow(Row &row) {
    free(row.talker); free(row.content); free(row.img_path); free(row.msg_source);
    row = Row{};
}

char *Dup(const char *text) { return strdup(text ? text : ""); }

// The <msgsource> XML WeChat keeps in `lvbuffer`, behind a few bytes of binary header.
char *ExtractMsgSource(const void *blob, int size) {
    if (!blob || size <= 0) return nullptr;
    const char *begin = static_cast<const char *>(memmem(blob, static_cast<size_t>(size), "<msgsource>", 11));
    if (!begin) return nullptr;
    const char *end_of_blob = static_cast<const char *>(blob) + size;
    const char *stop = static_cast<const char *>(memmem(begin, static_cast<size_t>(end_of_blob - begin), "</msgsource>", 12));
    if (!stop) return nullptr;
    stop += 12;
    char *copy = static_cast<char *>(malloc(static_cast<size_t>(stop - begin) + 1));
    if (!copy) return nullptr;
    memcpy(copy, begin, static_cast<size_t>(stop - begin));
    copy[stop - begin] = 0;
    return copy;
}

struct RowSink { Row *rows; int count, max; };

bool CopyRow(Wcdb *db, void *stmt, void *context) {
    auto *sink = static_cast<RowSink *>(context);
    if (sink->count >= sink->max) return false;
    Row &row = sink->rows[sink->count];
    row = Row{};
    row.rowid = WcdbInt(db, stmt, 0);
    row.id = WcdbInt(db, stmt, 1);
    row.svr_id = WcdbInt(db, stmt, 2);
    row.type = static_cast<int>(WcdbInt(db, stmt, 3));
    row.is_send = static_cast<int>(WcdbInt(db, stmt, 4));
    row.create_time = WcdbInt(db, stmt, 5);
    row.talker = Dup(WcdbText(db, stmt, 6));
    row.content = Dup(WcdbText(db, stmt, 7));
    row.img_path = Dup(WcdbText(db, stmt, 8));
    int size = 0;
    const void *blob = WcdbBlob(db, stmt, 9, &size);
    row.msg_source = ExtractMsgSource(blob, size);
    ++sink->count;
    return true;
}

// Copies up to `max` rows of `sql` (which must select kRowColumns). False on a query error, in
// which case the caller must not treat the (possibly empty) result as "nothing there".
bool FetchRows(Store *store, const char *sql, Row *rows, int max, int *count) {
    RowSink sink{rows, 0, max};
    pthread_mutex_lock(&store->mutex);
    const bool ok = WcdbQuery(store->db, sql, CopyRow, &sink);
    pthread_mutex_unlock(&store->mutex);
    *count = sink.count;
    return ok;
}

MessageRow View(const Row &row) {
    MessageRow view;
    view.id = row.id; view.svr_id = row.svr_id; view.type = row.type; view.is_send = row.is_send;
    view.create_time = row.create_time;
    view.talker = row.talker ? row.talker : "";
    view.content = row.content ? row.content : "";
    view.img_path = row.img_path ? row.img_path : "";
    view.msg_source = row.msg_source ? row.msg_source : "";
    return view;
}

// The local id of the message a reply quotes. WeChat records the pair in `MsgQuote`, but the
// row may land a moment after the message itself, so fall back to the quoted server id from
// the reply's own XML.
struct QuoteIds { long long local, server; };
bool QuoteRow(Wcdb *db, void *stmt, void *context) {
    auto *ids = static_cast<QuoteIds *>(context);
    ids->local = WcdbInt(db, stmt, 0);
    ids->server = WcdbInt(db, stmt, 1);
    return false;
}
bool LocalIdRow(Wcdb *db, void *stmt, void *context) {
    *static_cast<long long *>(context) = WcdbInt(db, stmt, 0);
    return false;
}
bool FindQuotedId(Store *store, long long reply_id, const char *server_id, char *out, size_t capacity) {
    out[0] = 0;
    long long local = 0;
    char sql[200];
    pthread_mutex_lock(&store->mutex);
    // The reply's own XML names the quoted server id, and message.msgSvrId is indexed.
    long long server = server_id && *server_id ? atoll(server_id) : 0;
    if (server > 0) {
        snprintf(sql, sizeof(sql), "SELECT msgId FROM message WHERE msgSvrId = %lld LIMIT 1", server);
        WcdbQuery(store->db, sql, LocalIdRow, &local);
    }
    if (local <= 0) {
        // WeChat's own pairing table (unindexed on msgId, but small); the row may land a moment
        // after the message itself, which is why it is the fallback rather than the first choice.
        QuoteIds ids{0, 0};
        snprintf(sql, sizeof(sql), "SELECT quotedMsgId, quotedMsgSvrId FROM MsgQuote WHERE msgId = %lld LIMIT 1", reply_id);
        WcdbQuery(store->db, sql, QuoteRow, &ids);
        local = ids.local;
        if (local <= 0 && ids.server > 0) {
            snprintf(sql, sizeof(sql), "SELECT msgId FROM message WHERE msgSvrId = %lld LIMIT 1", ids.server);
            WcdbQuery(store->db, sql, LocalIdRow, &local);
        }
    }
    pthread_mutex_unlock(&store->mutex);
    if (local <= 0) return false;
    snprintf(out, capacity, "%lld", local);
    return true;
}

struct Parts { cJSON *channel = nullptr, *guild = nullptr, *user = nullptr, *member = nullptr; };
void FreeParts(Parts &parts) {
    cJSON_Delete(parts.channel); cJSON_Delete(parts.guild); cJSON_Delete(parts.user); cJSON_Delete(parts.member);
    parts = Parts{};
}

// Who wrote the row, and where. Group senders come with their in-group nickname.
void BuildParts(Store *store, const Row &row, const Decoded &decoded, Parts *parts) {
    const char *talker = row.talker ? row.talker : "";
    const bool group = strstr(talker, "@chatroom") != nullptr;
    char sender[96];
    if (row.is_send && store->self_id[0]) snprintf(sender, sizeof(sender), "%s", store->self_id);
    else if (decoded.sender[0]) snprintf(sender, sizeof(sender), "%s", decoded.sender);
    else snprintf(sender, sizeof(sender), "%s", talker);
    if (group) {
        cJSON *member = StoreGuildMemberGet(store, talker, sender);
        if (member) {
            parts->user = cJSON_DetachItemFromObjectCaseSensitive(member, "user");
            parts->member = member;
        }
        parts->guild = OneContact(store, talker, 0);
        if (!parts->guild) {
            parts->guild = cJSON_CreateObject();
            if (parts->guild) cJSON_AddStringToObject(parts->guild, "id", talker);
        }
    }
    if (!parts->user) parts->user = MemberUser(store, sender);
    parts->channel = cJSON_CreateObject();
    if (parts->channel) {
        cJSON_AddStringToObject(parts->channel, "id", talker);
        cJSON_AddNumberToObject(parts->channel, "type", group ? 0 : 1);
        const cJSON *name = parts->guild ? cJSON_GetObjectItemCaseSensitive(parts->guild, "name") : nullptr;
        if (cJSON_IsString(name) && *name->valuestring) cJSON_AddStringToObject(parts->channel, "name", name->valuestring);
    }
}

// A reply's `<quote>` element and `message.quote`. With the quoted message's local id the
// element is a bare reference (what Satori clients resolve); without one it carries the
// quoted text itself so the reply still reads.
char *ComposeContent(Store *store, const Row &row, const Decoded &decoded, cJSON **quote) {
    *quote = nullptr;
    if (decoded.kind != MsgKind::Quote) return strdup(decoded.content ? decoded.content : "");
    char quoted_id[24];
    const bool known = FindQuotedId(store, row.id, decoded.refer_svr_id, quoted_id, sizeof(quoted_id));
    TextBuf escaped;
    escaped.Text(decoded.refer_text ? decoded.refer_text : "");
    TextBuf out;
    if (known) {
        out.Append("<quote id=\""); out.Attr(quoted_id); out.Append("\"/>");
    } else {
        out.Append("<quote>");
        if (decoded.refer_user[0]) {
            out.Append("<author user-id=\""); out.Attr(decoded.refer_user); out.Append("\"");
            if (decoded.refer_name[0]) { out.Append(" nickname=\""); out.Attr(decoded.refer_name); out.Append("\""); }
            out.Append("/>");
        }
        if (escaped.data) out.Append(escaped.data, escaped.size);
        out.Append("</quote>");
    }
    out.Append(decoded.content ? decoded.content : "");
    cJSON *object = cJSON_CreateObject();
    if (object) {
        if (known) cJSON_AddStringToObject(object, "id", quoted_id);
        cJSON_AddStringToObject(object, "content", escaped.data ? escaped.data : "");
        if (decoded.refer_user[0]) {
            cJSON *user = cJSON_CreateObject();
            if (user) {
                cJSON_AddStringToObject(user, "id", decoded.refer_user);
                if (decoded.refer_name[0]) cJSON_AddStringToObject(user, "name", decoded.refer_name);
                cJSON_AddItemToObject(object, "user", user);
            }
        }
        *quote = object;
    }
    return out.Take();
}

// The Satori Message for a decoded, deliverable row. `keep`, when given, receives the channel /
// guild / user / member the event wrapper needs (ownership moves to the caller); otherwise they
// are discarded.
cJSON *BuildMessage(Store *store, const Row &row, const Decoded &decoded, Parts *keep) {
    Parts parts;
    BuildParts(store, row, decoded, &parts);
    cJSON *quote = nullptr;
    char *content = ComposeContent(store, row, decoded, &quote);
    cJSON *message = cJSON_CreateObject();
    if (!message || !content || !parts.channel || !parts.user) {
        cJSON_Delete(message); cJSON_Delete(quote); free(content); FreeParts(parts);
        return nullptr;
    }
    char id[24];
    snprintf(id, sizeof(id), "%lld", row.id);
    cJSON_AddStringToObject(message, "id", id);
    cJSON_AddStringToObject(message, "content", content);
    cJSON_AddNumberToObject(message, "timestamp", static_cast<double>(row.create_time));
    free(content);
    cJSON_AddItemToObject(message, "channel", cJSON_Duplicate(parts.channel, true));
    cJSON_AddItemToObject(message, "user", cJSON_Duplicate(parts.user, true));
    if (parts.guild) cJSON_AddItemToObject(message, "guild", cJSON_Duplicate(parts.guild, true));
    if (parts.member) cJSON_AddItemToObject(message, "member", cJSON_Duplicate(parts.member, true));
    if (quote) cJSON_AddItemToObject(message, "quote", quote);
    if (keep) *keep = parts; else FreeParts(parts);
    return message;
}

// The event wrapper Satori wants around a message: the same resources, promoted to the top.
char *MessageEventJson(int login_sn, const char *type, cJSON *message, Parts &parts, double timestamp, bool manual_self = false) {
    cJSON *root = cJSON_CreateObject();
    cJSON *login = cJSON_CreateObject();
    if (!root || !login) { cJSON_Delete(root); cJSON_Delete(login); return nullptr; }
    cJSON_AddNumberToObject(login, "sn", login_sn);
    cJSON_AddItemToObject(root, "login", login);
    cJSON_AddStringToObject(root, "type", type);
    cJSON_AddNumberToObject(root, "timestamp", timestamp);
    if (parts.channel) { cJSON_AddItemToObject(root, "channel", parts.channel); parts.channel = nullptr; }
    if (parts.guild) { cJSON_AddItemToObject(root, "guild", parts.guild); parts.guild = nullptr; }
    if (parts.user) { cJSON_AddItemToObject(root, "user", parts.user); parts.user = nullptr; }
    if (parts.member) { cJSON_AddItemToObject(root, "member", parts.member); parts.member = nullptr; }
    cJSON_AddItemToObject(root, "message", message);
    if (manual_self) {
        // Same shape as satori-qq's extension: the account's own message, typed by its owner.
        cJSON *extension = cJSON_CreateObject();
        if (extension) {
            cJSON_AddBoolToObject(extension, "manual_self", 1);
            cJSON_AddItemToObject(root, "satori_wx", extension);
        }
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}

bool AllDigits(const char *text) {
    if (!text || !*text) return false;
    for (const char *p = text; *p; ++p) if (*p < '0' || *p > '9') return false;
    return strlen(text) < 19;
}

// Rows of a channel that can be chat messages at all; the decoder makes the final call.
constexpr char kNotSystem[] = "(type & 65535) NOT IN (10000, 10002)";

// A position in a channel's history. Pages are ordered by (createTime, msgId), which is what
// the (talker, createTime) index serves without a sort; a token is just the msgId, and its time
// is looked up. (Ordering by rowid alone would make SQLite collect and sort every row of a busy
// chat for each page.)
struct HistoryPos { long long time = 0, id = 0; };

struct Page {
    cJSON *messages[kListMax + 1];
    long long ids[kListMax + 1];
    long long times[kListMax + 1];
    int count = 0;
};

void FreePage(Page &page) {
    for (int i = 0; i < page.count; ++i) cJSON_Delete(page.messages[i]);
    page.count = 0;
}

bool TimeRow(Wcdb *db, void *stmt, void *context) {
    *static_cast<long long *>(context) = WcdbInt(db, stmt, 0);
    return false;
}
// The position of message `id`; false when the row is gone (a stale token).
bool CursorOf(Store *store, long long id, HistoryPos *out) {
    char sql[120];
    snprintf(sql, sizeof(sql), "SELECT createTime FROM message WHERE msgId = %lld", id);
    long long time = -1;
    pthread_mutex_lock(&store->mutex);
    const bool ok = WcdbQuery(store->db, sql, TimeRow, &time);
    pthread_mutex_unlock(&store->mutex);
    if (!ok || time < 0) return false;
    out->time = time;
    out->id = id;
    return true;
}

// Walks away from `bound` and gathers up to `want` deliverable messages (system rows and
// bookkeeping do not count, so a page is never short just because a tip sat in the way). The
// bound itself is excluded.
bool CollectPage(Store *store, const char *channel, bool ascending, bool bounded, HistoryPos bound, int want, Page *page) {
    Row rows[kBatch];
    while (page->count < want) {
        char sql[800];
        char condition[64] = "";
        if (bounded) snprintf(condition, sizeof(condition), " AND createTime %s %lld", ascending ? ">=" : "<=", bound.time);
        snprintf(sql, sizeof(sql),
                 "SELECT %s FROM message WHERE talker = '%s' AND %s%s ORDER BY createTime %s, msgId %s LIMIT %d",
                 kRowColumns, channel, kNotSystem, condition, ascending ? "ASC" : "DESC", ascending ? "ASC" : "DESC", kBatch);
        int count = 0;
        const bool ok = FetchRows(store, sql, rows, kBatch, &count);
        HistoryPos last = bound;
        for (int i = 0; i < count; ++i) {
            const Row &row = rows[i];
            last.time = row.create_time;
            last.id = row.id;
            // Rows that share the bound's timestamp are on the far side of it by id.
            const bool tied = bounded && row.create_time == bound.time && (ascending ? row.id <= bound.id : row.id >= bound.id);
            if (!tied && page->count < want) {
                Decoded decoded;
                DecodeMessage(View(row), store->self_id, &decoded);
                if (decoded.deliver) {
                    cJSON *message = BuildMessage(store, row, decoded, nullptr);
                    if (message) {
                        page->messages[page->count] = message;
                        page->ids[page->count] = row.id;
                        page->times[page->count] = row.create_time;
                        ++page->count;
                    }
                }
            }
        }
        for (int i = 0; i < count; ++i) FreeRow(rows[i]);
        if (!ok) return false;
        if (count < kBatch) break;
        bounded = true;
        bound = last;
    }
    return true;
}

bool ExistsRow(Wcdb *, void *, void *context) { *static_cast<bool *>(context) = true; return false; }
// Whether the channel has a chat row beyond `at` (before it, or after it) in the same order.
bool HasBeyond(Store *store, const char *channel, HistoryPos at, bool after) {
    char sql[500];
    snprintf(sql, sizeof(sql),
             "SELECT 1 FROM message WHERE talker = '%s' AND %s AND createTime %s %lld AND (createTime %s %lld OR msgId %s %lld) LIMIT 1",
             channel, kNotSystem, after ? ">=" : "<=", at.time, after ? ">" : "<", at.time, after ? ">" : "<", at.id);
    bool found = false;
    pthread_mutex_lock(&store->mutex);
    WcdbQuery(store->db, sql, ExistsRow, &found);
    pthread_mutex_unlock(&store->mutex);
    return found;
}

void ReversePage(Page &page) {
    for (int i = 0, j = page.count - 1; i < j; ++i, --j) {
        cJSON *message = page.messages[i]; page.messages[i] = page.messages[j]; page.messages[j] = message;
        const long long id = page.ids[i]; page.ids[i] = page.ids[j]; page.ids[j] = id;
        const long long time = page.times[i]; page.times[i] = page.times[j]; page.times[j] = time;
    }
}
} // namespace

long long StoreSkipped(Store *store) { return store ? store->skipped : 0; }

long long StorePoll(Store *store, long long since, int login_sn, bool (*emit)(void *context, const char *event), void *context, bool *more) {
    if (more) *more = false;
    if (!store || !store->db || !emit) return since;
    char sql[300];
    snprintf(sql, sizeof(sql), "SELECT %s FROM message WHERE rowid > %lld ORDER BY rowid ASC LIMIT %d", kRowColumns, since, kPollLimit);
    Row rows[kPollLimit];
    int count = 0;
    // A failed read must not move the watermark: WeChat may be mid-write, and the same rows are
    // simply read again on the next pass.
    if (!FetchRows(store, sql, rows, kPollLimit, &count)) {
        for (int i = 0; i < count; ++i) FreeRow(rows[i]);
        return since;
    }
    long long last = since;
    bool stalled = false;
    for (int i = 0; i < count; ++i) {
        if (!stalled) {
            Decoded decoded;
            DecodeMessage(View(rows[i]), store->self_id, &decoded);
            if (decoded.deliver) {
                Parts parts;
                cJSON *message = BuildMessage(store, rows[i], decoded, &parts);
                const char *author = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(parts.user, "id"));
                char author_copy[80] = {};
                if (author) snprintf(author_copy, sizeof(author_copy), "%s", author);
                const bool manual_self = rows[i].is_send && !SentByModule(store, rows[i].talker ? rows[i].talker : "", rows[i].id);
                char *text = message ? MessageEventJson(login_sn, "message-created", message, parts, static_cast<double>(rows[i].create_time), manual_self) : nullptr;
                FreeParts(parts);
                if (text && strlen(text) >= kEventSize) { free(text); text = nullptr; ++store->skipped; }
                if (text) {
                    const bool accepted = emit(context, text);
                    free(text);
                    if (accepted && author_copy[0]) {
                        auto &slot = store->authors[store->author_next++ % 512];
                        slot.id = rows[i].id;
                        snprintf(slot.user, sizeof(slot.user), "%s", author_copy);
                    }
                    // The bus is full: keep this row for the next pass rather than lose it.
                    if (!accepted) stalled = true;
                }
            }
            if (!stalled) last = rows[i].rowid;
        }
        FreeRow(rows[i]);
    }
    if (more) *more = !stalled && count == kPollLimit;
    return last;
}

cJSON *StoreMessageGet(Store *store, const char *channel_id, const char *message_id) {
    if (!store || !store->db || !SafeSql(channel_id) || !AllDigits(message_id)) return nullptr;
    const long long wanted = atoll(message_id);
    // The local id first (primary key), then WeChat's server id (indexed with the talker): two
    // cheap lookups rather than one OR that would scan the channel.
    char sql[600];
    Row rows[1];
    int count = 0;
    snprintf(sql, sizeof(sql), "SELECT %s FROM message WHERE msgId = %lld AND talker = '%s'", kRowColumns, wanted, channel_id);
    if (!FetchRows(store, sql, rows, 1, &count)) return nullptr;
    if (count < 1) {
        snprintf(sql, sizeof(sql), "SELECT %s FROM message WHERE talker = '%s' AND msgSvrId = %lld LIMIT 1", kRowColumns, channel_id, wanted);
        if (!FetchRows(store, sql, rows, 1, &count) || count < 1) return nullptr;
    }
    Decoded decoded;
    DecodeMessage(View(rows[0]), store->self_id, &decoded);
    cJSON *message = decoded.deliver ? BuildMessage(store, rows[0], decoded, nullptr) : nullptr;
    FreeRow(rows[0]);
    return message;
}

cJSON *StoreMessageList(Store *store, const char *channel_id, const char *next, const char *direction, int limit, const char *order) {
    if (!store || !store->db || !SafeSql(channel_id)) return nullptr;
    if (limit < 1) limit = kListMax;
    if (limit > kListMax) limit = kListMax;
    const bool has_cursor = next && *next;
    if (has_cursor && !AllDigits(next)) return nullptr;
    HistoryPos cursor;
    if (has_cursor && !CursorOf(store, atoll(next), &cursor)) return nullptr;  // a token for a row that no longer exists
    // Without a cursor the only sensible direction is "before the newest".
    const bool after = has_cursor && direction && !strcmp(direction, "after");
    const bool around = has_cursor && direction && !strcmp(direction, "around");
    const bool descending = order && !strcmp(order, "desc");

    Page older, newer;
    bool ok = true;
    if (around) {
        // The anchor itself is the first (newest) row of the "older" walk from just past it.
        HistoryPos past{cursor.time, cursor.id + 1};
        const int before = limit / 2 + 1 > limit ? limit : limit / 2 + 1;
        ok = CollectPage(store, channel_id, false, true, past, before, &older);
        ok = ok && CollectPage(store, channel_id, true, true, cursor, limit - older.count, &newer);
    } else if (after) {
        ok = CollectPage(store, channel_id, true, true, cursor, limit, &newer);
    } else {
        ok = CollectPage(store, channel_id, false, has_cursor, cursor, limit, &older);
    }
    if (!ok) { FreePage(older); FreePage(newer); return nullptr; }
    ReversePage(older);  // older pages are gathered newest-first; the result is oldest-first

    // At most `limit` in total by construction, so everything fits.
    Page all;
    for (int i = 0; i < older.count; ++i) { all.messages[all.count] = older.messages[i]; all.ids[all.count] = older.ids[i]; all.times[all.count++] = older.times[i]; }
    for (int i = 0; i < newer.count; ++i) { all.messages[all.count] = newer.messages[i]; all.ids[all.count] = newer.ids[i]; all.times[all.count++] = newer.times[i]; }
    older.count = newer.count = 0;

    HistoryPos oldest, newest;
    if (all.count) {
        oldest = HistoryPos{all.times[0], all.ids[0]};
        newest = HistoryPos{all.times[all.count - 1], all.ids[all.count - 1]};
    }
    if (descending) ReversePage(all);
    cJSON *result = cJSON_CreateObject();
    cJSON *data = cJSON_CreateArray();
    if (!result || !data) { cJSON_Delete(result); cJSON_Delete(data); FreePage(all); return nullptr; }
    cJSON_AddItemToObject(result, "data", data);
    for (int i = 0; i < all.count; ++i) cJSON_AddItemToArray(data, all.messages[i]);
    if (all.count) {
        char token[32];
        if (HasBeyond(store, channel_id, oldest, false)) { snprintf(token, sizeof(token), "%lld", oldest.id); cJSON_AddStringToObject(result, "prev", token); }
        if (HasBeyond(store, channel_id, newest, true)) { snprintf(token, sizeof(token), "%lld", newest.id); cJSON_AddStringToObject(result, "next", token); }
    }
    return result;
}

// ---- media files ------------------------------------------------------------------------------
namespace {
bool SafeName(const char *name) {
    const size_t n = name ? strlen(name) : 0;
    if (!n || n > 120 || strstr(name, "..")) return false;
    for (size_t i = 0; i < n; ++i) {
        const char c = name[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return false;
    }
    return true;
}

bool HexToken(const char *text, size_t length) {
    if (strlen(text) != length) return false;
    for (size_t i = 0; i < length; ++i) if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    return true;
}

// Reads the first bytes of a regular file and names what it is. "" means a WeChat-private
// image container (wxgf) no client can open; null means unreadable or not a regular file.
const char *SniffImage(const char *path) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return nullptr;
    struct stat info {};
    unsigned char head[16] = {};
    const bool regular = !fstat(fd, &info) && S_ISREG(info.st_mode) && info.st_size > 0;
    const ssize_t got = regular ? read(fd, head, sizeof(head)) : -1;
    close(fd);
    if (got < 4) return nullptr;
    if (head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) return "image/jpeg";
    if (head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') return "image/png";
    if (!memcmp(head, "GIF8", 4)) return "image/gif";
    if (got >= 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WEBP", 4)) return "image/webp";
    return "";
}

bool RegularFile(const char *path) {
    struct stat info {};
    return !stat(path, &info) && S_ISREG(info.st_mode) && info.st_size > 0;
}

// The extension decides the type of received files; anything unknown downloads as bytes.
const char *TypeByExtension(const char *path) {
    const char *dot = strrchr(path, '.');
    if (!dot) return "application/octet-stream";
    static const struct { const char *ext, *type; } table[] = {
        {".pdf", "application/pdf"}, {".zip", "application/zip"}, {".txt", "text/plain; charset=utf-8"},
        {".json", "application/json"}, {".mp4", "video/mp4"}, {".mp3", "audio/mpeg"}, {".png", "image/png"},
        {".jpg", "image/jpeg"}, {".jpeg", "image/jpeg"}, {".gif", "image/gif"}, {".webp", "image/webp"},
        {".doc", "application/msword"}, {".docx", "application/vnd.openxmlformats-officedocument.wordprocessingml.document"},
        {".xls", "application/vnd.ms-excel"}, {".xlsx", "application/vnd.openxmlformats-officedocument.spreadsheetml.sheet"},
        {".ppt", "application/vnd.ms-powerpoint"}, {".pptx", "application/vnd.openxmlformats-officedocument.presentationml.presentation"},
    };
    for (const auto &entry : table) if (!strcasecmp(dot, entry.ext)) return entry.type;
    return "application/octet-stream";
}

struct MediaRow {
    bool found;
    char img_path[256];
    char xml_md5[40];
    char full_path[1400];
};
bool MediaRowCallback(Wcdb *db, void *stmt, void *context) {
    auto *media = static_cast<MediaRow *>(context);
    media->found = true;
    const char *img_path = WcdbText(db, stmt, 0);
    snprintf(media->img_path, sizeof(media->img_path), "%s", img_path ? img_path : "");
    const char *content = WcdbText(db, stmt, 1);
    // The XML follows the optional "sender:" header; both emoji and image name their md5 there.
    const char *xml = content ? strstr(content, "<msg") : nullptr;
    if (xml) {
        char md5[40] = {};
        XmlSlice root = XmlDocument(xml);
        if (!XmlGetAttribute(root, "msg/emoji", "md5", md5, sizeof(md5))) XmlGetAttribute(root, "msg/img", "md5", md5, sizeof(md5));
        snprintf(media->xml_md5, sizeof(media->xml_md5), "%s", md5);
    }
    return false;
}
bool FilePathRow(Wcdb *db, void *stmt, void *context) {
    auto *media = static_cast<MediaRow *>(context);
    const char *path = WcdbText(db, stmt, 0);
    if (path) snprintf(media->full_path, sizeof(media->full_path), "%s", path);
    return false;
}

// "THUMBNAIL_DIRPATH://th_<md5>", "th_<md5>hd", "<md5>": the 32 hex digits inside.
bool ImageHash(const char *img_path, char *out) {
    for (const char *p = img_path; *p; ++p) {
        size_t n = 0;
        while (p[n] && ((p[n] >= '0' && p[n] <= '9') || (p[n] >= 'a' && p[n] <= 'f'))) ++n;
        if (n >= 32) { memcpy(out, p, 32); out[32] = 0; return true; }
        p += n;
        if (!*p) break;
    }
    return false;
}
} // namespace

bool StoreMediaFile(Store *store, const char *kind, long long msg_id, char *path, size_t path_capacity, char *content_type, size_t type_capacity) {
    if (!store || !store->db || !kind || msg_id <= 0 || !store->account_dir[0]) return false;
    char sql[200];
    MediaRow media{};
    pthread_mutex_lock(&store->mutex);
    snprintf(sql, sizeof(sql), "SELECT imgPath, content FROM message WHERE msgId = %lld LIMIT 1", msg_id);
    WcdbQuery(store->db, sql, MediaRowCallback, &media);
    if (media.found && !strcmp(kind, "file")) {
        snprintf(sql, sizeof(sql), "SELECT fileFullPath FROM appattach WHERE msgInfoId = %lld LIMIT 1", msg_id);
        WcdbQuery(store->db, sql, FilePathRow, &media);
    }
    pthread_mutex_unlock(&store->mutex);
    if (!media.found) return false;

    char candidate[1500];
    auto accept = [&](const char *file, const char *type) {
        if (strlen(file) >= path_capacity || strlen(type) >= type_capacity) return false;
        memcpy(path, file, strlen(file) + 1);
        memcpy(content_type, type, strlen(type) + 1);
        return true;
    };

    if (!strcmp(kind, "image") || !strcmp(kind, "emoji")) {
        char hashes[2][40] = {};
        int hash_count = 0;
        if (!strcmp(kind, "image") && ImageHash(media.img_path, hashes[hash_count])) ++hash_count;
        if (HexToken(media.xml_md5, 32)) { snprintf(hashes[hash_count], sizeof(hashes[0]), "%s", media.xml_md5); ++hash_count; }
        for (int h = 0; h < hash_count; ++h) {
            const char *hash = hashes[h];
            const char *names[5];
            char dirs[8], pattern[5][80];
            int names_count = 0;
            if (!strcmp(kind, "emoji")) {
                snprintf(pattern[0], sizeof(pattern[0]), "emoji/%s", hash);
                names[names_count++] = pattern[0];
            } else {
                snprintf(dirs, sizeof(dirs), "%.2s/%.2s", hash, hash + 2);
                snprintf(pattern[0], sizeof(pattern[0]), "image2/%s/%s", dirs, hash);
                snprintf(pattern[1], sizeof(pattern[1]), "image2/%s/%s.jpg", dirs, hash);
                snprintf(pattern[2], sizeof(pattern[2]), "image2/%s/th_%shd", dirs, hash);
                snprintf(pattern[3], sizeof(pattern[3]), "image2/%s/th_%s", dirs, hash);
                for (int i = 0; i < 4; ++i) names[names_count++] = pattern[i];
            }
            for (int i = 0; i < names_count; ++i) {
                snprintf(candidate, sizeof(candidate), "%s/%s", store->account_dir, names[i]);
                const char *type = SniffImage(candidate);
                if (type && *type) return accept(candidate, type);  // "" = wxgf: keep looking for a smaller, usable copy
            }
        }
        return false;
    }
    if (!strcmp(kind, "voice")) {
        if (!SafeName(media.img_path)) return false;
        char md5[33];
        Md5Hex(media.img_path, strlen(media.img_path), md5);
        snprintf(candidate, sizeof(candidate), "%s/voice2/%.2s/%.2s/msg_%s.amr", store->account_dir, md5, md5 + 2, media.img_path);
        return RegularFile(candidate) && accept(candidate, "audio/silk");
    }
    if (!strcmp(kind, "video") || !strcmp(kind, "videothumb")) {
        if (!SafeName(media.img_path)) return false;
        const bool thumb = !strcmp(kind, "videothumb");
        snprintf(candidate, sizeof(candidate), "%s/video/%s.%s", store->account_dir, media.img_path, thumb ? "jpg" : "mp4");
        return RegularFile(candidate) && accept(candidate, thumb ? "image/jpeg" : "video/mp4");
    }
    if (!strcmp(kind, "file")) {
        const char *file = media.full_path;
        if (*file != '/' || strstr(file, "..")) return false;
        // Only files WeChat itself keeps: its private data, or its folder on shared storage.
        char private_prefix[1110];
        snprintf(private_prefix, sizeof(private_prefix), "%s/", store->app_dir);
        const bool allowed = (store->app_dir[0] && !strncmp(file, private_prefix, strlen(private_prefix))) ||
                             !strncmp(file, "/storage/emulated/0/Android/data/com.tencent.mm/", 49) ||
                             !strncmp(file, "/sdcard/Android/data/com.tencent.mm/", 37);
        return allowed && RegularFile(file) && accept(file, TypeByExtension(file));
    }
    return false;
}
} // namespace satori

// ==== change feeds ============================================================================
namespace satori {
namespace {
struct StampSink { RoomStamp *out; int count, max; };
bool StampRow(Wcdb *db, void *stmt, void *context) {
    auto *sink = static_cast<StampSink *>(context);
    if (sink->count >= sink->max) return false;
    RoomStamp &stamp = sink->out[sink->count++];
    const char *name = WcdbText(db, stmt, 0);
    snprintf(stamp.name, sizeof(stamp.name), "%s", name ? name : "");
    stamp.modify_time = WcdbInt(db, stmt, 1);
    stamp.member_count = WcdbInt(db, stmt, 2);
    stamp.version = WcdbInt(db, stmt, 3);
    return true;
}
struct StringSink { char *text; bool ok; };
bool MembersRow(Wcdb *db, void *stmt, void *context) {
    auto *sink = static_cast<StringSink *>(context);
    const char *members = WcdbText(db, stmt, 0);
    sink->text = strdup(members ? members : "");
    sink->ok = sink->text != nullptr;
    return false;
}
struct RevokedSink { RevokedRow *out; int count, max; };
bool RevokedCallback(Wcdb *db, void *stmt, void *context) {
    auto *sink = static_cast<RevokedSink *>(context);
    if (sink->count >= sink->max) return false;
    RevokedRow &row = sink->out[sink->count++];
    row.id = WcdbInt(db, stmt, 0);
    const char *talker = WcdbText(db, stmt, 1);
    snprintf(row.talker, sizeof(row.talker), "%s", talker ? talker : "");
    row.create_time = WcdbInt(db, stmt, 2);
    row.is_send = static_cast<int>(WcdbInt(db, stmt, 3));
    return true;
}
struct IdSink { TextBuf *buffer; };
bool FriendIdRow(Wcdb *db, void *stmt, void *context) {
    auto *sink = static_cast<IdSink *>(context);
    const char *id = WcdbText(db, stmt, 0);
    if (id && *id) { sink->buffer->Append(id); sink->buffer->Append('\n'); }
    return true;
}
} // namespace

int StoreRoomStamps(Store *store, RoomStamp *out, int max) {
    if (!store || !store->db || !out) return -1;
    StampSink sink{out, 0, max};
    pthread_mutex_lock(&store->mutex);
    const bool ok = WcdbQuery(store->db, "SELECT chatroomname, modifytime, memberCount, chatroomVersion FROM chatroom", StampRow, &sink);
    pthread_mutex_unlock(&store->mutex);
    return ok ? sink.count : -1;
}

char *StoreRoomMembers(Store *store, const char *room) {
    if (!store || !store->db || !SafeSql(room)) return nullptr;
    char sql[200];
    snprintf(sql, sizeof(sql), "SELECT memberlist FROM chatroom WHERE chatroomname = '%s' LIMIT 1", room);
    StringSink sink{nullptr, false};
    pthread_mutex_lock(&store->mutex);
    const bool ok = WcdbQuery(store->db, sql, MembersRow, &sink);
    pthread_mutex_unlock(&store->mutex);
    if (!ok || !sink.ok) { free(sink.text); return nullptr; }
    return sink.text;
}

char *StoreFriendIds(Store *store) {
    if (!store || !store->db) return nullptr;
    TextBuf buffer;
    IdSink sink{&buffer};
    pthread_mutex_lock(&store->mutex);
    const bool ok = WcdbQuery(store->db,
        "SELECT username FROM rcontact WHERE (type & 3) = 3 AND deleteFlag = 0 AND username NOT LIKE '%@chatroom' "
        "AND username NOT LIKE 'gh\\_%' ESCAPE '\\'", FriendIdRow, &sink);
    pthread_mutex_unlock(&store->mutex);
    if (!ok) return nullptr;
    return buffer.Take();
}

int StoreRevoked(Store *store, long long since_ms, RevokedRow *out, int max) {
    if (!store || !store->db || !out) return -1;
    char sql[300];
    snprintf(sql, sizeof(sql),
             "SELECT msgId, talker, createTime, isSend FROM message WHERE (type IN (268445456, 285222674) OR (type & 65535) = 10002) "
             "AND createTime > %lld LIMIT %d", since_ms, max);
    RevokedSink sink{out, 0, max};
    pthread_mutex_lock(&store->mutex);
    const bool ok = WcdbQuery(store->db, sql, RevokedCallback, &sink);
    pthread_mutex_unlock(&store->mutex);
    // No ORDER BY in the query: sorting by msgId would make SQLite scan the whole table instead
    // of using the createTime index. The handful of rows are put in id order here.
    for (int i = 1; i < sink.count; ++i) {
        RevokedRow row = out[i];
        int j = i - 1;
        for (; j >= 0 && out[j].id > row.id; --j) out[j + 1] = out[j];
        out[j + 1] = row;
    }
    return ok ? sink.count : -1;
}

bool StoreAuthorOf(Store *store, long long msg_id, char *out, size_t capacity) {
    if (!store || !out || !capacity) return false;
    out[0] = 0;
    for (const auto &slot : store->authors) {
        if (slot.id == msg_id && slot.user[0]) { snprintf(out, capacity, "%s", slot.user); return true; }
    }
    return false;
}

bool StoreFindSentImage(Store *store, const char *talker, long long since, long long *local_id) {
    if (!store || !store->db || !SafeSql(talker) || !local_id) return false;
    // A real sent picture carries the CDN XML in content; a degenerate one (WeChat's own
    // pipeline rejects e.g. 1x1 images) is inserted with "<msg></msg>" and never advances
    // (status stays 5). Only a filled row counts as sent.
    char sql[300];
    snprintf(sql, sizeof(sql),
             "SELECT msgId FROM message WHERE talker = '%s' AND isSend = 1 AND type = 3 AND msgId > %lld"
             " AND length(content) > 20 ORDER BY msgId LIMIT 1",
             talker, since);
    long long found = 0;
    pthread_mutex_lock(&store->mutex);
    WcdbQuery(store->db, sql, LocalIdRow, &found);
    pthread_mutex_unlock(&store->mutex);
    if (found <= 0) return false;
    *local_id = found;
    return true;
}

bool StoreFindStalledImage(Store *store, const char *talker, long long since, long long *local_id) {
    if (!store || !store->db || !SafeSql(talker) || !local_id) return false;
    char sql[300];
    snprintf(sql, sizeof(sql),
             "SELECT msgId FROM message WHERE talker = '%s' AND isSend = 1 AND type = 3 AND msgId > %lld"
             " AND length(content) <= 20 ORDER BY msgId LIMIT 1",
             talker, since);
    long long found = 0;
    pthread_mutex_lock(&store->mutex);
    WcdbQuery(store->db, sql, LocalIdRow, &found);
    pthread_mutex_unlock(&store->mutex);
    if (found <= 0) return false;
    *local_id = found;
    return true;
}

bool StoreFindSentVideo(Store *store, const char *talker, long long since, long long *local_id) {
    if (!store || !store->db || !SafeSql(talker) || !local_id) return false;
    char sql[300];
    snprintf(sql, sizeof(sql),
             "SELECT msgId FROM message WHERE talker = '%s' AND isSend = 1 AND type = 43 AND msgId > %lld ORDER BY msgId LIMIT 1",
             talker, since);
    long long found = 0;
    pthread_mutex_lock(&store->mutex);
    WcdbQuery(store->db, sql, LocalIdRow, &found);
    pthread_mutex_unlock(&store->mutex);
    if (found <= 0) return false;
    *local_id = found;
    return true;
}

bool StoreSentStatus(Store *store, long long local_id, int *status) {
    if (!store || !store->db || !status || local_id <= 0) return false;
    char sql[200];
    snprintf(sql, sizeof(sql), "SELECT status + 1000 FROM message WHERE msgId = %lld AND isSend = 1", local_id);
    long long found = 0;
    pthread_mutex_lock(&store->mutex);
    WcdbQuery(store->db, sql, LocalIdRow, &found);
    pthread_mutex_unlock(&store->mutex);
    if (found < 1000) return false;   // no row (LocalIdRow leaves 0)
    *status = static_cast<int>(found - 1000);
    return true;
}

cJSON *StoreUserObject(Store *store, const char *id) { return MemberUser(store, id); }

cJSON *StoreGuildObject(Store *store, const char *id) {
    cJSON *guild = OneContact(store, id, 0);
    if (guild) return guild;
    guild = cJSON_CreateObject();
    if (guild) cJSON_AddStringToObject(guild, "id", id ? id : "");
    return guild;
}
} // namespace satori
