// Host tests for the read-only WeChat message store.
// A plaintext SQLite file stands in for EnMicroMsg.db; rows are mapped to Satori events.
#include "wx_store.h"
#include "wcdb.h"
#include "protocol.h"
#include "vendor/cjson/cJSON.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace {
int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}
struct Sink { int count = 0; char events[8][4096] = {}; };
bool Emit(void *context, const char *event) {
    auto *sink = static_cast<Sink *>(context);
    if (sink->count >= 8) return false;
    snprintf(sink->events[sink->count], sizeof(sink->events[0]), "%s", event);
    ++sink->count;
    return true;
}
const cJSON *Item(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : nullptr; }
const char *Str(const cJSON *o, const char *k) { const cJSON *v = Item(o, k); return cJSON_IsString(v) ? v->valuestring : ""; }
const char *Nested(const cJSON *o, const char *a, const char *b) { return Str(Item(o, a), b); }
double Num(const cJSON *o, const char *k) { const cJSON *v = Item(o, k); return cJSON_IsNumber(v) ? v->valuedouble : -1; }
} // namespace

int main() {
    const char *library = getenv("SATORI_WCDB_LIB");
    if (!library || !*library) { printf("store tests: SKIP (set SATORI_WCDB_LIB)\n"); return 0; }
    const char *base = getenv("SATORI_ACCOUNT_TMP");
    if (!base || !*base) base = getenv("TMPDIR");
    if (!base || !*base) base = ".";
    char directory[512];
    snprintf(directory, sizeof(directory), "%s/satori-store-XXXXXX", base);
    if (!mkdtemp(directory)) { fprintf(stderr, "mkdtemp failed\n"); return 2; }
    char path[640];
    snprintf(path, sizeof(path), "%s/msg.db", directory);
    const int created = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (created >= 0) close(created);

    satori::Wcdb *writer = satori::WcdbOpenEx(library, path, nullptr, 0, 0, 0, 0, 0);
    Check(writer != nullptr, "open writer");
    if (!writer) return 1;
    Check(satori::WcdbExec(writer, "CREATE TABLE message(msgId INTEGER, type INTEGER, isSend INTEGER, createTime INTEGER, talker TEXT, content TEXT)"), "create");
    Check(satori::WcdbExec(writer, "INSERT INTO message VALUES(1,1,0,1700000000000,'123@chatroom','wxid_abc:\n你好')"), "insert group");
    Check(satori::WcdbExec(writer, "INSERT INTO message VALUES(2,1,0,1700000001000,'wxid_xyz','hi')"), "insert private");
    Check(satori::WcdbExec(writer, "INSERT INTO message VALUES(3,1,1,1700000002000,'123@chatroom','self says')"), "insert sent");
    Check(satori::WcdbExec(writer, "INSERT INTO message VALUES(4,3,0,1700000003000,'wxid_xyz','')"), "insert image");
    Check(satori::WcdbExec(writer, "INSERT INTO message VALUES(5,1,0,1700000004000,'wxid_xyz','a<b>&c')"), "insert markup");
    Check(satori::WcdbExec(writer, "CREATE TABLE rcontact(username TEXT PRIMARY KEY, alias TEXT, conRemark TEXT, nickname TEXT, type INTEGER, deleteFlag INTEGER)"), "create rcontact");
    Check(satori::WcdbExec(writer, "INSERT INTO rcontact VALUES('wxid_friend','fri','好友备注','昵称',3,0)"), "insert friend");
    Check(satori::WcdbExec(writer, "INSERT INTO rcontact VALUES('123@chatroom','','群备注','群名',2,0)"), "insert group contact");
    Check(satori::WcdbExec(writer, "INSERT INTO rcontact VALUES('gh_abc','','','公众号',1,0)"), "insert service");
    Check(satori::WcdbExec(writer, "CREATE TABLE chatroom(chatroomname TEXT PRIMARY KEY, memberlist TEXT, displayname TEXT, roomowner TEXT, memberCount INTEGER, roomdata BLOB)"), "create chatroom");
    // roomdata is WeChat's cached member protobuf: member{userName=1, flag=3}; bit 2048 is
    // the group-admin bit. wxid_abc carries the plain member flag, wxid_friend is an admin.
    Check(satori::WcdbExec(writer, "INSERT INTO chatroom VALUES('123@chatroom','wxid_abc;wxid_friend','甲、好友备注','wxid_abc',2,"
                                    "X'0A0C0A08777869645F61626318010A100A0B777869645F667269656E64188110')"), "insert chatroom");
    // A room whose cache has no roomdata at all still reports plain members.
    Check(satori::WcdbExec(writer, "INSERT INTO chatroom VALUES('456@chatroom','wxid_plain',NULL,'wxid_owner2',1,NULL)"), "insert chatroom without roomdata");
    satori::WcdbClose(writer);

    satori::Store *store = satori::CreateStoreEx(library, path, nullptr, 0, 0, "self_wxid");
    Check(store != nullptr, "open store");
    if (!store) { fprintf(stderr, "store error: %s\n", satori::StoreError(nullptr)); return 1; }
    Check(satori::StoreWatermark(store) == 5, "watermark before poll");
    Sink sink;
    const long long watermark = satori::StorePoll(store, 0, 1, Emit, &sink);
    Check(watermark == 5, "poll returns watermark");
    Check(sink.count == 4, "media row skipped");
    if (sink.count == 4) {
        cJSON *group = cJSON_Parse(sink.events[0]);
        Check(!strcmp(Str(group, "type"), "message-created"), "event type");
        Check(Num(group, "timestamp") == 1700000000000.0, "timestamp");
        Check(Num(Item(group, "login"), "sn") == 1, "login sn");
        Check(!strcmp(Nested(group, "user", "id"), "wxid_abc"), "group sender from prefix");
        Check(!strcmp(Nested(group, "message", "content"), "你好"), "group text without prefix");
        Check(!strcmp(Nested(group, "channel", "id"), "123@chatroom"), "group channel id");
        Check(Num(Item(group, "channel"), "type") == 0, "group channel type");
        Check(!strcmp(Nested(Item(group, "message"), "channel", "id"), "123@chatroom"), "message carries channel");
        cJSON_Delete(group);
        cJSON *priv = cJSON_Parse(sink.events[1]);
        Check(!strcmp(Nested(priv, "user", "id"), "wxid_xyz"), "private sender");
        Check(Num(Item(priv, "channel"), "type") == 1, "direct channel type");
        cJSON_Delete(priv);
        cJSON *sent = cJSON_Parse(sink.events[2]);
        Check(!strcmp(Nested(sent, "user", "id"), "self_wxid"), "sent author is self");
        cJSON_Delete(sent);
        cJSON *markup = cJSON_Parse(sink.events[3]);
        Check(!strcmp(Nested(markup, "message", "content"), "a&lt;b&gt;&amp;c"), "markup escaped");
        cJSON_Delete(markup);
    }
    Sink again;
    Check(satori::StorePoll(store, watermark, 1, Emit, &again) == 5 && again.count == 0, "no replay");
    // The hub must accept the generated events once the login is known.
    satori::Hub *hub = satori::CreateHub();
    char *login = satori::Apply(hub, "{\"type\":\"login-added\",\"login\":{\"sn\":1,\"status\":1,\"adapter\":\"t\",\"platform\":\"wechat\",\"user\":{\"id\":\"self_wxid\"},\"features\":[]}}", false);
    Check(login != nullptr, "login applied");
    free(login);
    for (int i = 0; i < sink.count; ++i) {
        char *applied = satori::Apply(hub, sink.events[i], false);
        Check(applied != nullptr, "store event accepted by hub");
        free(applied);
    }
    Check(satori::Latest(hub) > 0, "hub sequence advanced");
    satori::DestroyHub(hub);
    // message.list returns the channel's text messages, newest first.
    cJSON *list = satori::StoreMessageList(store, "123@chatroom", nullptr, 20);
    Check(list != nullptr && cJSON_GetArraySize(Item(list, "data")) == 2, "message.list group");
    cJSON_Delete(list);
    cJSON *page = satori::StoreMessageList(store, "123@chatroom", nullptr, 1);
    Check(page && cJSON_GetArraySize(Item(page, "data")) == 1 && Item(page, "next"), "message.list page 1");
    const char *cursor = Str(page, "next");
    char cursor_copy[32];
    snprintf(cursor_copy, sizeof(cursor_copy), "%s", cursor);
    cJSON_Delete(page);
    cJSON *page2 = satori::StoreMessageList(store, "123@chatroom", cursor_copy, 1);
    Check(page2 && cJSON_GetArraySize(Item(page2, "data")) == 1, "message.list page 2");
    cJSON_Delete(page2);
    // message.get by platform message id.
    cJSON *one = satori::StoreMessageGet(store, "123@chatroom", "3");
    Check(one != nullptr, "message.get");
    Check(one && !strcmp(Nested(one, "user", "id"), "self_wxid"), "message.get author");
    cJSON_Delete(one);
    Check(satori::StoreMessageGet(store, "123@chatroom", "99999") == nullptr, "message.get missing");
    // Contacts: friends exclude groups and services; groups map to guild/channel.
    cJSON *user = satori::StoreUserGet(store, "wxid_friend");
    Check(user && !strcmp(Str(user, "id"), "wxid_friend") && !strcmp(Str(user, "name"), "好友备注"), "user.get");
    cJSON_Delete(user);
    cJSON *friends = satori::StoreFriendList(store, nullptr, 50);
    Check(friends && cJSON_GetArraySize(Item(friends, "data")) == 1, "friend.list excludes groups/services");
    cJSON_Delete(friends);
    cJSON *guilds = satori::StoreGuildList(store, nullptr, 50);
    Check(guilds && cJSON_GetArraySize(Item(guilds, "data")) == 1, "guild.list");
    cJSON *guild = guilds ? cJSON_GetArrayItem(Item(guilds, "data"), 0) : nullptr;
    Check(guild && !strcmp(Str(guild, "id"), "123@chatroom") && !strcmp(Str(guild, "name"), "群备注"), "guild name");
    cJSON_Delete(guilds);
    cJSON *group_channel = satori::StoreChannelGet(store, "123@chatroom");
    Check(group_channel && Num(group_channel, "type") == 0, "group channel type");
    cJSON_Delete(group_channel);
    cJSON *direct = satori::StoreChannelGet(store, "wxid_friend");
    Check(direct && Num(direct, "type") == 1, "direct channel type");
    cJSON_Delete(direct);
    cJSON *channels = satori::StoreChannelList(store, "123@chatroom", nullptr, 50);
    Check(channels && cJSON_GetArraySize(Item(channels, "data")) == 1, "channel.list");
    cJSON_Delete(channels);

    // Guild members come from the chatroom table: memberlist + displayname + roomowner.
    cJSON *members = satori::StoreGuildMemberList(store, "123@chatroom", nullptr, 50);
    Check(members && cJSON_GetArraySize(Item(members, "data")) == 2, "guild.member.list count");
    const cJSON *first_member = members ? cJSON_GetArrayItem(Item(members, "data"), 0) : nullptr;
    Check(first_member && !strcmp(Str(first_member, "nick"), "甲"), "member group display name");
    Check(first_member && !strcmp(Nested(first_member, "user", "id"), "wxid_abc"), "member user id");
    cJSON_Delete(members);
    cJSON *member_page = satori::StoreGuildMemberList(store, "123@chatroom", nullptr, 1);
    Check(member_page && cJSON_GetArraySize(Item(member_page, "data")) == 1 && *Str(member_page, "next"), "member page cursor");
    cJSON_Delete(member_page);
    cJSON *one_member = satori::StoreGuildMemberGet(store, "123@chatroom", "wxid_friend");
    Check(one_member && !strcmp(Str(one_member, "nick"), "好友备注"), "guild.member.get display name");
    Check(one_member && !strcmp(Nested(one_member, "user", "name"), "好友备注"), "member contact name");
    cJSON_Delete(one_member);
    Check(satori::StoreGuildMemberGet(store, "123@chatroom", "nobody") == nullptr, "member.get missing");
    Check(satori::StoreGuildMemberList(store, "999@chatroom", nullptr, 50) == nullptr, "member.list unknown guild");

    cJSON *roles = satori::StoreGuildRoleList(store, "123@chatroom");
    Check(roles && cJSON_GetArraySize(Item(roles, "data")) == 3, "guild.role.list");
    cJSON_Delete(roles);
    Check(satori::StoreGuildRoleList(store, "999@chatroom") == nullptr, "role.list unknown guild");
    cJSON *owner_roles = satori::StoreMemberRoleList(store, "123@chatroom", "wxid_abc");
    Check(owner_roles && !strcmp(Str(cJSON_GetArrayItem(Item(owner_roles, "data"), 0), "id"), "owner"), "owner role");
    cJSON_Delete(owner_roles);
    cJSON *member_roles = satori::StoreMemberRoleList(store, "123@chatroom", "wxid_friend");
    Check(member_roles && !strcmp(Str(cJSON_GetArrayItem(Item(member_roles, "data"), 0), "id"), "admin"), "admin role from roomdata");
    cJSON_Delete(member_roles);
    // Without a roomdata cache the admin bit is unknown, so the member stays a plain member.
    cJSON *plain_roles = satori::StoreMemberRoleList(store, "456@chatroom", "wxid_plain");
    Check(plain_roles && !strcmp(Str(cJSON_GetArrayItem(Item(plain_roles, "data"), 0), "id"), "member"), "plain member without roomdata");
    cJSON_Delete(plain_roles);
    cJSON *none_roles = satori::StoreMemberRoleList(store, "123@chatroom", "nobody");
    Check(none_roles && cJSON_GetArraySize(Item(none_roles, "data")) == 0, "non-member has no roles");
    cJSON_Delete(none_roles);

    satori::DestroyStore(store);
    unlink(path);
    rmdir(directory);
    if (failures) { fprintf(stderr, "%d store test(s) failed\n", failures); return 1; }
    printf("store tests: PASS\n");
    return 0;
}
