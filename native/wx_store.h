#pragma once
#include <stddef.h>
#include "vendor/cjson/cJSON.h"

// Read-only WeChat message store built on native/wcdb.
//
// It opens a WeChat database (EnMicroMsg.db) read-only with the account's cipher key and
// turns rows of the `message` table into Satori Message objects and `message-created`
// events. It never writes and never touches WeChat's own sqlite connections.
namespace satori {
struct Store;
// self_id is the account's own wxid (used as the author of sent messages).
Store *CreateStore(const char *path, const void *key, int key_size, int compatibility, const char *self_id);
// Same, with an explicit SQLCipher library (tests use a plain SQLite library).
Store *CreateStoreEx(const char *library, const char *path, const void *key, int key_size, int compatibility, const char *self_id);
void DestroyStore(Store *store);
bool StoreReady(Store *store);
const char *StoreError(Store *store);
// The account's own wxid, used as the author of outgoing messages. Never null.
const char *StoreSelfId(Store *store);
// Highest message rowid, or -1 when it cannot be read.
long long StoreWatermark(Store *store);
// Who wrote a row this account sent: this module (message.create) or a person typing in WeChat
// itself. WeChat's table has one flag (`isSend`) for both, so the module keeps its own account:
// a `message.create` to a talker is open from Begin to End (plus a short grace, for a poll that
// lags behind), and every local id it got back is remembered. Rows the poller reads from this
// account that match neither are announced with `satori_wx.manual_self`, the counterpart of
// satori-qq's marker, so a consumer can tell the owner's own messages from the echo of its
// own sends.
void StoreSendBegin(Store *store, const char *talker);
void StoreSendEnd(Store *store, const char *talker);
void StoreNoteSent(Store *store, long long local_id);
// Emits one `message-created` event per new chat message (rowid > since) and returns the new
// watermark: the last row that was handled. A row whose event `emit` refuses (the queue is
// full) is *not* passed, so it is offered again on the next call instead of being lost; rows
// that are not chat messages (system tips, call logs) are passed silently. `more` (optional)
// reports that a full batch was read and another call should follow immediately.
long long StorePoll(Store *store, long long since, int login_sn, bool (*emit)(void *context, const char *event), void *context,
                    bool *more = nullptr);
// Rows the poller had to drop because their event could never fit the bus.
long long StoreSkipped(Store *store);
// Satori BidiList of messages for a channel. `next` is a token from a previous result (empty:
// start from the newest message); `direction` is before|after|around relative to it; `order` is
// asc|desc for the returned page (the default, asc, is oldest first whatever the direction). The
// result carries `prev` (more older messages exist) and `next` (more newer ones do) tokens.
// Null on a malformed token or unknown channel id. Caller frees.
cJSON *StoreMessageList(Store *store, const char *channel_id, const char *next, const char *direction, int limit, const char *order);
// One message by id within a channel (the local id, or WeChat's server id). Caller frees.
cJSON *StoreMessageGet(Store *store, const char *channel_id, const char *message_id);
// Resolves a message-media kind (image|voice|video|videothumb|emoji|file) of local message
// `msg_id` to a file on disk plus a content type. Everything derived from the database is
// validated before it becomes part of a path.
bool StoreMediaFile(Store *store, const char *kind, long long msg_id, char *path, size_t path_capacity,
                    char *content_type, size_t type_capacity);
// Contacts from rcontact. Satori User / Friend / Guild / Channel shapes. Caller frees.
cJSON *StoreUserGet(Store *store, const char *user_id);
cJSON *StoreFriendList(Store *store, const char *next, int limit);
cJSON *StoreGuildList(Store *store, const char *next, int limit);
cJSON *StoreGuildGet(Store *store, const char *guild_id);
cJSON *StoreChannelGet(Store *store, const char *channel_id);
cJSON *StoreChannelList(Store *store, const char *guild_id, const char *next, int limit);
// Guild members from the `chatroom` table (memberlist + displayname + roomowner).
// `next` is a member offset. Satori GuildMember objects. Caller frees.
cJSON *StoreGuildMemberList(Store *store, const char *guild_id, const char *next, int limit);
cJSON *StoreGuildMemberGet(Store *store, const char *guild_id, const char *user_id);
// Roles are synthetic: WeChat has no custom guild roles, so `owner`, `admin` and `member`
// only. The admin bit comes from the chatroom's cached member flags (roomdata), so it tracks
// guild.member.role.set/unset as soon as WeChat refreshes that row.
cJSON *StoreGuildRoleList(Store *store, const char *guild_id);
cJSON *StoreMemberRoleList(Store *store, const char *guild_id, const char *user_id);

// ---- change feeds ---------------------------------------------------------------------------
// What the event scanner (wx_events) reads to notice things that are not new rows: recalled
// messages, group membership, friendships. Each returns -1 / null on a read error, which the
// caller must treat as "unknown", never as "nothing changed" or "everything is gone".
struct RoomStamp {
    char name[80];
    long long modify_time, member_count, version;  // cheap fingerprint (chatroomVersion is the server-side roster version)
};
int StoreRoomStamps(Store *store, RoomStamp *out, int max);
// The room's member ids, ';'-separated, in a malloc'd string ("" for an empty roster).
char *StoreRoomMembers(Store *store, const char *room);
// Every friend id, '\n'-separated, malloc'd.
char *StoreFriendIds(Store *store);
struct RevokedRow {
    long long id;
    char talker[80];
    long long create_time;  // of the original message
    int is_send;
};
// Recalled messages (WeChat rewrites the original row in place) created after `since_ms`.
int StoreRevoked(Store *store, long long since_ms, RevokedRow *out, int max);
// Who wrote a message the poller announced earlier in this process (recent ones only).
bool StoreAuthorOf(Store *store, long long msg_id, char *out, size_t capacity);
// The first picture message this account sent to `talker` after row `since` (a watermark taken
// before the send). True with its local id once WeChat's own pipeline has inserted the row with
// its CDN XML: the only reliable sign that an asynchronous image send really finished. A row
// with empty content (WeChat rejects degenerate pictures, e.g. 1x1) does not count.
bool StoreFindSentImage(Store *store, const char *talker, long long since, long long *local_id);
// The opposite case: an inserted picture row whose content never filled in — WeChat's pipeline
// accepted the request but stalled. Used to tell the caller exactly what happened.
bool StoreFindStalledImage(Store *store, const char *talker, long long since, long long *local_id);
// The first video message (type 43) this account sent to `talker` after row `since`. WeChat's video
// pipeline inserts the row at the start (status 1) and fills it in when the upload finishes.
bool StoreFindSentVideo(Store *store, const char *talker, long long since, long long *local_id);
// The send status of one of our own rows: 1 sending, 2 sent, 5 failed (other values as WeChat
// writes them). False when the row does not exist.
bool StoreSentStatus(Store *store, long long local_id, int *status);
// Satori User / Guild objects for event payloads; a bare {"id"} when the contact is unknown.
cJSON *StoreUserObject(Store *store, const char *id);
cJSON *StoreGuildObject(Store *store, const char *id);
} // namespace satori
