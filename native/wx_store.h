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
// Emits one JSON event per new message (rowid > since). Returns the new watermark.
long long StorePoll(Store *store, long long since, int login_sn, bool (*emit)(void *context, const char *event), void *context);
// Satori List of messages for a channel, newest first. `next` is a rowid cursor from a
// previous call or null. Caller frees the result.
cJSON *StoreMessageList(Store *store, const char *channel_id, const char *next, int limit);
// One message by platform id within a channel. Caller frees.
cJSON *StoreMessageGet(Store *store, const char *channel_id, const char *message_id);
// Contacts from rcontact. Satori User / Friend / Guild / Channel shapes. Caller frees.
cJSON *StoreUserGet(Store *store, const char *user_id);
cJSON *StoreFriendList(Store *store, const char *next, int limit);
cJSON *StoreGuildList(Store *store, const char *next, int limit);
cJSON *StoreGuildGet(Store *store, const char *guild_id);
cJSON *StoreChannelGet(Store *store, const char *channel_id);
cJSON *StoreChannelList(Store *store, const char *guild_id, const char *next, int limit);
} // namespace satori
