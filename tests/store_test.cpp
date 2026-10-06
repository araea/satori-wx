// Host tests for the read-only WeChat message store.
// A plaintext SQLite file with the real WeChat column layout stands in for EnMicroMsg.db; rows
// are mapped to Satori events, messages, lists and media files.
#include "wx_store.h"
#include "wx_live.h"
#include "wx_media.h"
#include "wcdb.h"
#include "media.h"
#include "protocol.h"
#include "vendor/cjson/cJSON.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

// The resolver reaches the store through the live-store accessor; the test supplies its own.
namespace satori {
Store *g_test_store = nullptr;
Store *LiveStore() { return g_test_store; }
} // namespace satori

namespace {
int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
struct Sink {
    int count = 0, accept = 1 << 30; // `accept` events are taken, then the bus is "full"
    char *events[512] = {};
    ~Sink() {
        for (int i = 0; i < count; ++i) free(events[i]);
    }
};
bool Emit(void *context, const char *event) {
    auto *sink = static_cast<Sink *>(context);
    if (sink->count >= 512 || sink->count >= sink->accept) return false;
    sink->events[sink->count++] = strdup(event);
    return true;
}
const cJSON *Item(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : nullptr; }
const char *Str(const cJSON *o, const char *k) {
    const cJSON *v = Item(o, k);
    return cJSON_IsString(v) ? v->valuestring : "";
}
const char *Nested(const cJSON *o, const char *a, const char *b) { return Str(Item(o, a), b); }
double Num(const cJSON *o, const char *k) {
    const cJSON *v = Item(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : -1;
}
bool Exec(satori::Wcdb *db, const char *sql) {
    if (satori::WcdbExec(db, sql)) return true;
    fprintf(stderr, "SQL failed: %s\n  %s\n", satori::WcdbError(db), sql);
    return false;
}
void WriteFile(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) {
        fprintf(stderr, "cannot write %s\n", path);
        return;
    }
    fwrite(data, 1, size, file);
    fclose(file);
}
void MakeDir(const char *path) { mkdir(path, 0700); }
bool Contains(const char *text, const char *part) { return text && strstr(text, part); }
// Polls to exhaustion the way the live loop does and returns the final watermark.
long long Drain(satori::Store *store, long long since, Sink *sink) {
    for (int guard = 0; guard < 100; ++guard) {
        bool more = false;
        const long long next = satori::StorePoll(store, since, 1, Emit, sink, &more);
        if (next == since && !more) return since;
        since = next;
        if (!more) return since;
    }
    return since;
}
} // namespace

int main() {
    const char *library = getenv("SATORI_WCDB_LIB");
    if (!library || !*library) {
        printf("store tests: SKIP (set SATORI_WCDB_LIB)\n");
        return 0;
    }
    satori::MediaSetSecret("token-for-the-store-tests-aaaaaaaaaaaaaaaa");
    const char *base = getenv("SATORI_ACCOUNT_TMP");
    if (!base || !*base) base = getenv("TMPDIR");
    if (!base || !*base) base = ".";
    // <base>/satori-store-XXXXXX/app/MicroMsg/acct/EnMicroMsg.db: the layout the store derives
    // its media directory from.
    char directory[512], account[600], path[700], step[800];
    snprintf(directory, sizeof(directory), "%s/satori-store-XXXXXX", base);
    if (!mkdtemp(directory)) {
        fprintf(stderr, "mkdtemp failed\n");
        return 2;
    }
    snprintf(step, sizeof(step), "%s/app", directory);
    MakeDir(step);
    snprintf(step, sizeof(step), "%s/app/MicroMsg", directory);
    MakeDir(step);
    snprintf(account, sizeof(account), "%s/app/MicroMsg/acct", directory);
    MakeDir(account);
    snprintf(path, sizeof(path), "%s/EnMicroMsg.db", account);
    const int created = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (created >= 0) close(created);

    satori::Wcdb *writer = satori::WcdbOpenEx(library, path, nullptr, 0, 0, 0, 0, 0);
    Check(writer != nullptr, "open writer");
    if (!writer) return 1;
    // The real column set of WeChat 8.0.78's message table (the ones the store reads); msgId is
    // the rowid alias, exactly as on a device.
    Check(
        Exec(
            writer,
            "CREATE TABLE message(msgId INTEGER PRIMARY KEY, msgSvrId INTEGER, type INT, status INT, isSend INT, isShowTimer INTEGER, "
            "createTime INTEGER, talker TEXT, content TEXT, imgPath TEXT, reserved TEXT, lvbuffer BLOB)"),
        "create");
    Check(
        Exec(
            writer,
            "CREATE TABLE MsgQuote(msgId INTEGER, msgSvrId INTEGER, quotedMsgId INTEGER, quotedMsgSvrId INTEGER, status INTEGER, quotedMsgTalker TEXT)"),
        "create MsgQuote");
    Check(
        Exec(
            writer,
            "CREATE TABLE img_flag(username TEXT PRIMARY KEY, imgflag INTEGER, lastupdatetime INTEGER, reserved1 TEXT, reserved2 TEXT)"),
        "create img_flag");
    Check(Exec(writer, "CREATE TABLE appattach(msgInfoId INTEGER, fileFullPath TEXT)"), "create appattach");
    Check(
        Exec(
            writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(1,1001,1,0,1700000000000,'123@chatroom','wxid_abc:\n你好')"),
        "insert group");
    Check(
        Exec(
            writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(2,1002,1,0,1700000001000,'wxid_xyz','hi')"),
        "insert private");
    Check(
        Exec(
            writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(3,1003,1,1,1700000002000,'123@chatroom','self says')"),
        "insert sent");
    Check(
        Exec(
            writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content,imgPath) VALUES(4,1004,3,0,1700000003000,'wxid_xyz',"
            "'<msg><img md5=\"ab\"/></msg>','THUMBNAIL_DIRPATH://th_0123456789abcdef0123456789abcdef')"),
        "insert image");
    Check(
        Exec(
            writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(5,1005,1,0,1700000004000,'wxid_xyz','a<b>&c')"),
        "insert markup");
    // A system tip and a recalled message: neither is a chat message.
    Check(
        Exec(
            writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(6,1006,10000,0,1700000005000,'wxid_xyz','你已添加了某人')"),
        "insert system tip");
    Check(
        Exec(
            writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(7,1007,268445456,0,1700000006000,'123@chatroom','\"某人\" 撤回了一条消息')"),
        "insert revoke");
    // A group mention: the list is in lvbuffer's <msgsource> (after a binary header), the text
    // carries "@name" + U+2005.
    Check(
        Exec(
            writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content,lvbuffer) VALUES(8,1008,1,0,1700000007000,'123@chatroom',"
            "'wxid_friend:\n@好友备注' || char(8197) || '看这里',"
            "X'7B00000000000001' || CAST('<msgsource><atuserlist><![CDATA[wxid_friend]]></atuserlist></msgsource>' AS BLOB) || X'0000')"),
        "insert mention");
    // A reply, with WeChat's own MsgQuote row pointing at message 1; and one whose MsgQuote row has
    // not landed yet (resolved from the server id in the XML instead).
    Check(
        Exec(
            writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(9,1009,822083633,0,1700000008000,'123@chatroom',"
            "'wxid_friend:\n<msg><appmsg><title>同意</title><type>57</type><refermsg><type>1</type><svrid>1001</svrid><fromusr>123@chatroom</fromusr>"
            "<chatusr>wxid_abc</chatusr><displayname>甲</displayname><content>你好</content></refermsg></appmsg></msg>')"),
        "insert reply");
    Check(Exec(writer, "INSERT INTO MsgQuote VALUES(9,1009,1,1001,0,'123@chatroom')"), "insert MsgQuote");
    Check(
        Exec(
            writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(10,1010,822083633,0,1700000009000,'123@chatroom',"
            "'wxid_friend:\n<msg><appmsg><title>再回</title><type>57</type><refermsg><type>1</type><svrid>1003</svrid><fromusr>123@chatroom</fromusr>"
            "<chatusr>self_wxid</chatusr><displayname>我</displayname><content>self says</content></refermsg></appmsg></msg>')"),
        "insert late reply");
    // A reply to something the database no longer holds.
    Check(
        Exec(
            writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(11,1011,822083633,0,1700000010000,'wxid_xyz',"
            "'<msg><appmsg><title>好的</title><type>57</type><refermsg><type>1</type><svrid>424242</svrid><fromusr>wxid_xyz</fromusr>"
            "<displayname>乙</displayname><content>很久以前的话</content></refermsg></appmsg></msg>')"),
        "insert orphan reply");
    Check(
        Exec(
            writer,
            "CREATE TABLE rcontact(username TEXT PRIMARY KEY, alias TEXT, conRemark TEXT, nickname TEXT, type INTEGER, deleteFlag INTEGER)"),
        "create rcontact");
    Check(Exec(writer, "INSERT INTO rcontact VALUES('wxid_friend','fri','好友备注','昵称',3,0)"), "insert friend");
    Check(Exec(writer, "INSERT INTO rcontact VALUES('123@chatroom','','群备注','群名',2,0)"), "insert group contact");
    Check(Exec(writer, "INSERT INTO rcontact VALUES('gh_abc','','','公众号',1,0)"), "insert service");
    Check(
        Exec(
            writer,
            "INSERT INTO img_flag VALUES('wxid_friend',3,0,'https://wx.qlogo.cn/mmhead/small','https://wx.qlogo.cn/mmhead/big')"),
        "insert avatar");
    Check(
        Exec(
            writer,
            "CREATE TABLE chatroom(chatroomname TEXT PRIMARY KEY, memberlist TEXT, displayname TEXT, roomowner TEXT, memberCount INTEGER, roomdata BLOB)"),
        "create chatroom");
    // roomdata is WeChat's cached member protobuf: member{userName=1, flag=3}; bit 2048 is
    // the group-admin bit. wxid_abc carries the plain member flag, wxid_friend is an admin.
    Check(Exec(writer, "INSERT INTO chatroom VALUES('123@chatroom','wxid_abc;wxid_friend','甲、好友备注','wxid_abc',2,"
                       "X'0A0C0A08777869645F61626318010A100A0B777869645F667269656E64188110')"),
          "insert chatroom");
    // A room whose cache has no roomdata at all still reports plain members.
    Check(Exec(writer, "INSERT INTO chatroom VALUES('456@chatroom','wxid_plain',NULL,'wxid_owner2',1,NULL)"),
          "insert chatroom without roomdata");
    satori::WcdbClose(writer);

    satori::Store *store = satori::CreateStoreEx(library, path, nullptr, 0, 0, "self_wxid");
    Check(store != nullptr, "open store");
    if (!store) {
        fprintf(stderr, "store error: %s\n", satori::StoreError(nullptr));
        return 1;
    }
    Check(satori::StoreWatermark(store) == 11, "watermark before poll");

    // ---- live events ------------------------------------------------------------------------
    Sink sink;
    const long long watermark = Drain(store, 0, &sink);
    Check(watermark == 11, "poll consumes every row, including the ones that are not messages");
    // 1,2,3 text; 4 image; 5 markup; 6 and 7 are not messages; 8 mention; 9,10,11 replies.
    Check(sink.count == 9, "system tips and revoke markers are not delivered");
    if (sink.count == 9) {
        cJSON *group = cJSON_Parse(sink.events[0]);
        Check(!strcmp(Str(group, "type"), "message-created"), "event type");
        Check(Num(group, "timestamp") == 1700000000000.0, "timestamp");
        Check(Num(Item(group, "message"), "created_at") == 1700000000000.0 &&
                  !Item(Item(group, "message"), "timestamp"),
              "message time is created_at in ms");
        Check(Num(Item(group, "login"), "sn") == 1, "login sn");
        Check(!strcmp(Nested(group, "user", "id"), "wxid_abc"), "group sender from prefix");
        Check(!strcmp(Nested(group, "message", "content"), "你好"), "group text without prefix");
        Check(!strcmp(Nested(group, "channel", "id"), "123@chatroom"), "group channel id");
        Check(Num(Item(group, "channel"), "type") == 0, "group channel type");
        // Resource promotion: nothing the event carries at the top is repeated inside `message`.
        Check(!Item(Item(group, "message"), "channel") && !Item(Item(group, "message"), "user") &&
                  !Item(Item(group, "message"), "guild") && !Item(Item(group, "message"), "member"),
              "message carries no promoted resources");
        // Satori wants the guild and the member promoted next to the message in a group.
        Check(!strcmp(Nested(group, "guild", "id"), "123@chatroom") &&
                  !strcmp(Nested(group, "guild", "name"), "群备注"),
              "group event carries the guild");
        Check(!strcmp(Nested(group, "member", "nick"), "甲"), "group event carries the in-group nickname");
        Check(!strcmp(Nested(group, "channel", "name"), "群备注"), "group channel is named after the group");
        cJSON_Delete(group);
        cJSON *priv = cJSON_Parse(sink.events[1]);
        Check(!strcmp(Nested(priv, "user", "id"), "wxid_xyz"), "private sender");
        Check(Num(Item(priv, "channel"), "type") == 1, "direct channel type");
        Check(!Item(priv, "guild") && !Item(priv, "member"), "a private chat has no guild or member");
        cJSON_Delete(priv);
        cJSON *sent = cJSON_Parse(sink.events[2]);
        Check(!strcmp(Nested(sent, "user", "id"), "self_wxid"), "sent author is self");
        Check(Item(Item(sent, "satori_wx"), "manual_self") &&
                  cJSON_IsTrue(Item(Item(sent, "satori_wx"), "manual_self")),
              "the owner's own row is marked manual_self");
        cJSON_Delete(sent);
        cJSON *image = cJSON_Parse(sink.events[3]);
        Check(Contains(Nested(image, "message", "content"), "<img src=\"internal:wechat/self_wxid/_msg/image/4/"),
              "image is a signed link, not dropped");
        cJSON_Delete(image);
        cJSON *markup = cJSON_Parse(sink.events[4]);
        Check(!strcmp(Nested(markup, "message", "content"), "a&lt;b&gt;&amp;c"), "markup escaped");
        cJSON_Delete(markup);
        cJSON *mention = cJSON_Parse(sink.events[5]);
        Check(!strcmp(Nested(mention, "message", "content"), "<at id=\"wxid_friend\" name=\"好友备注\"/> 看这里"),
              "mention from lvbuffer becomes <at>");
        Check(!strcmp(Nested(mention, "user", "id"), "wxid_friend") &&
                  !strcmp(Nested(mention, "user", "avatar"), "https://wx.qlogo.cn/mmhead/big"),
              "sender carries the cached avatar");
        cJSON_Delete(mention);
        cJSON *reply = cJSON_Parse(sink.events[6]);
        Check(!strcmp(Nested(reply, "message", "content"), "<quote id=\"1\"/>同意"),
              "reply references the quoted message by local id (MsgQuote)");
        Check(!strcmp(Nested(Item(reply, "message"), "quote", "id"), "1") &&
                  !strcmp(Nested(Item(Item(reply, "message"), "quote"), "user", "id"), "wxid_abc"),
              "message.quote describes the original");
        Check(!strcmp(Str(Item(Item(reply, "message"), "quote"), "content"), "你好"), "quoted text");
        cJSON_Delete(reply);
        cJSON *late = cJSON_Parse(sink.events[7]);
        Check(!strcmp(Nested(late, "message", "content"), "<quote id=\"3\"/>再回"),
              "without a MsgQuote row the quoted server id is looked up");
        cJSON_Delete(late);
        cJSON *orphan = cJSON_Parse(sink.events[8]);
        Check(!strcmp(Nested(orphan, "message", "content"),
                      "<quote><author user-id=\"wxid_xyz\" nickname=\"乙\"/>很久以前的话</quote>好的"),
              "an unresolvable reply carries the quoted text inline");
        Check(!Item(Item(orphan, "message"), "quote") || !Item(Item(Item(orphan, "message"), "quote"), "id"),
              "and has no quote id");
        cJSON_Delete(orphan);
    }
    Sink again;
    Check(satori::StorePoll(store, watermark, 1, Emit, &again) == 11 && again.count == 0, "no replay");
    // The hub must accept the generated events once the login is known.
    satori::Hub *hub = satori::CreateHub();
    char *login = satori::Apply(
        hub,
        "{\"type\":\"login-added\",\"login\":{\"sn\":1,\"status\":1,\"adapter\":\"t\",\"platform\":\"wechat\",\"user\":{\"id\":\"self_wxid\"},\"features\":[]}}",
        false);
    Check(login != nullptr, "login applied");
    free(login);
    for (int i = 0; i < sink.count; ++i) {
        char *applied = satori::Apply(hub, sink.events[i], false);
        Check(applied != nullptr, "store event accepted by hub");
        free(applied);
    }
    Check(satori::Latest(hub) > 0, "hub sequence advanced");
    satori::DestroyHub(hub);

    // ---- bursts and back-pressure: nothing may be lost ------------------------------------------
    {
        satori::Wcdb *more_writer = satori::WcdbOpenEx(library, path, nullptr, 0, 0, 0, 0, 0);
        char sql[300];
        for (int i = 0; i < 130; ++i) {
            snprintf(
                sql, sizeof(sql),
                "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(%d,%d,1,0,%lld,'wxid_burst','burst %d')",
                100 + i, 5000 + i, 1700001000000LL + i, i);
            Exec(more_writer, sql);
        }
        satori::WcdbClose(more_writer);
        Sink burst;
        const long long after = Drain(store, 11, &burst);
        Check(after == 229 && burst.count == 130, "a burst larger than one batch is delivered in full");
        bool ordered = true;
        for (int i = 0; i < burst.count; ++i) {
            cJSON *event = cJSON_Parse(burst.events[i]);
            char expected[32];
            snprintf(expected, sizeof(expected), "burst %d", i);
            if (strcmp(Nested(event, "message", "content"), expected)) ordered = false;
            cJSON_Delete(event);
        }
        Check(ordered, "burst order preserved");
        // A full bus: the refused row stays, and is offered again first.
        satori::Wcdb *w2 = satori::WcdbOpenEx(library, path, nullptr, 0, 0, 0, 0, 0);
        for (int i = 0; i < 6; ++i) {
            snprintf(
                sql, sizeof(sql),
                "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(%d,%d,1,0,%lld,'wxid_full','full %d')",
                300 + i, 6000 + i, 1700002000000LL + i, i);
            Exec(w2, sql);
        }
        satori::WcdbClose(w2);
        Sink limited;
        limited.accept = 2;
        bool more = true;
        const long long held = satori::StorePoll(store, 229, 1, Emit, &limited, &more);
        Check(limited.count == 2 && held == 301 && !more, "a full bus stops the poll at the last accepted row");
        Sink resumed;
        const long long done = Drain(store, held, &resumed);
        cJSON *first = resumed.count ? cJSON_Parse(resumed.events[0]) : nullptr;
        Check(done == 305 && resumed.count == 4 && !strcmp(Nested(first, "message", "content"), "full 2"),
              "the refused row is redelivered, nothing skipped");
        cJSON_Delete(first);
        // An event that could never fit is counted and passed over, not retried forever.
        satori::Wcdb *w3 = satori::WcdbOpenEx(library, path, nullptr, 0, 0, 0, 0, 0);
        static char huge[200000];
        memset(huge, 'x', sizeof(huge) - 1);
        huge[sizeof(huge) - 1] = 0;
        char *big_sql = static_cast<char *>(malloc(sizeof(huge) + 300));
        snprintf(
            big_sql, sizeof(huge) + 300,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(400,7000,1,0,1700003000000,'wxid_big','%s')",
            huge);
        Exec(w3, big_sql);
        free(big_sql);
        Exec(
            w3,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(401,7001,1,0,1700003001000,'wxid_big','after the big one')");
        satori::WcdbClose(w3);
        Sink oversize;
        const long long past = Drain(store, 305, &oversize);
        Check(past == 401 && oversize.count == 1 && satori::StoreSkipped(store) == 1,
              "an oversized message is skipped and counted, the next one still arrives");
    }

    // ---- message.list: Satori's bidirectional paging ----------------------------------------------
    // Channel 123@chatroom holds chat messages 1, 3, 8, 9, 10 (7 is a revoke marker, not shown).
    cJSON *list = satori::StoreMessageList(store, "123@chatroom", nullptr, nullptr, 50, nullptr);
    Check(list != nullptr && cJSON_GetArraySize(Item(list, "data")) == 5, "message.list group");
    if (list) {
        const cJSON *data = Item(list, "data");
        Check(!strcmp(Str(cJSON_GetArrayItem(data, 0), "id"), "1") &&
                  !strcmp(Str(cJSON_GetArrayItem(data, 4), "id"), "10"),
              "default order is oldest first");
        Check(!Item(list, "prev") && !Item(list, "next"), "a complete history has neither cursor");
    }
    cJSON_Delete(list);
    cJSON *newest = satori::StoreMessageList(store, "123@chatroom", nullptr, "before", 2, "asc");
    Check(newest && cJSON_GetArraySize(Item(newest, "data")) == 2 &&
              !strcmp(Str(cJSON_GetArrayItem(Item(newest, "data"), 0), "id"), "9") &&
              !strcmp(Str(cJSON_GetArrayItem(Item(newest, "data"), 1), "id"), "10"),
          "the newest page, oldest first within it");
    Check(newest && !strcmp(Str(newest, "prev"), "9") && !Item(newest, "next"),
          "prev points at older messages, next is absent at the newest end");
    char token[32];
    snprintf(token, sizeof(token), "%s", Str(newest, "prev"));
    cJSON_Delete(newest);
    cJSON *older = satori::StoreMessageList(store, "123@chatroom", token, "before", 2, "asc");
    Check(older && cJSON_GetArraySize(Item(older, "data")) == 2 &&
              !strcmp(Str(cJSON_GetArrayItem(Item(older, "data"), 0), "id"), "3") &&
              !strcmp(Str(cJSON_GetArrayItem(Item(older, "data"), 1), "id"), "8"),
          "before a token: the messages older than it");
    Check(older && !strcmp(Str(older, "prev"), "3") && !strcmp(Str(older, "next"), "8"),
          "both cursors once we are in the middle");
    cJSON_Delete(older);
    cJSON *forward = satori::StoreMessageList(store, "123@chatroom", "3", "after", 2, "asc");
    Check(forward && cJSON_GetArraySize(Item(forward, "data")) == 2 &&
              !strcmp(Str(cJSON_GetArrayItem(Item(forward, "data"), 0), "id"), "8"),
          "after a token: the newer messages");
    Check(forward && !strcmp(Str(forward, "next"), "9") && !strcmp(Str(forward, "prev"), "8"),
          "cursors after paging forward");
    cJSON_Delete(forward);
    cJSON *desc = satori::StoreMessageList(store, "123@chatroom", nullptr, "before", 3, "desc");
    Check(desc && !strcmp(Str(cJSON_GetArrayItem(Item(desc, "data"), 0), "id"), "10") &&
              !strcmp(Str(cJSON_GetArrayItem(Item(desc, "data"), 2), "id"), "8"),
          "order=desc reverses the page");
    cJSON_Delete(desc);
    cJSON *around = satori::StoreMessageList(store, "123@chatroom", "8", "around", 3, "asc");
    Check(around && cJSON_GetArraySize(Item(around, "data")) == 3 &&
              !strcmp(Str(cJSON_GetArrayItem(Item(around, "data"), 0), "id"), "3") &&
              !strcmp(Str(cJSON_GetArrayItem(Item(around, "data"), 1), "id"), "8") &&
              !strcmp(Str(cJSON_GetArrayItem(Item(around, "data"), 2), "id"), "9"),
          "around a token: it and its neighbours");
    cJSON_Delete(around);
    Check(satori::StoreMessageList(store, "123@chatroom", "not-a-number", nullptr, 5, nullptr) == nullptr,
          "a malformed token is refused");
    cJSON *empty = satori::StoreMessageList(store, "nobody@chatroom", nullptr, nullptr, 5, nullptr);
    Check(empty && cJSON_GetArraySize(Item(empty, "data")) == 0 && !Item(empty, "prev"),
          "an empty channel is an empty page");
    cJSON_Delete(empty);
    // Paging is by (createTime, msgId): rows that share a timestamp, and a row that arrived late with
    // an older timestamp (an offline sync), must each appear exactly once whichever way we walk.
    {
        satori::Wcdb *order_writer = satori::WcdbOpenEx(library, path, nullptr, 0, 0, 0, 0, 0);
        const struct {
            int id;
            long long time;
        } rows[] = {
            {600, 1700005000000LL}, {601, 1700005001000LL}, {602, 1700005001000LL}, {603, 1700005001000LL},
            {604, 1700005002000LL}, {605, 1700005000500LL}, // arrived last, belongs between 600 and 601
            {606, 1700005003000LL},
        };
        char sql[300];
        for (const auto &row : rows) {
            snprintf(
                sql, sizeof(sql),
                "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(%d,%d,1,0,%lld,'wxid_order','m%d')",
                row.id, 9000 + row.id, row.time, row.id);
            Exec(order_writer, sql);
        }
        satori::WcdbClose(order_writer);
        const char *expected[] = {"600", "605", "601", "602", "603", "604", "606"};
        // Backwards, two at a time, following prev.
        char collected[16][8];
        int collected_count = 0;
        char token[32] = "";
        for (int guard = 0; guard < 10; ++guard) {
            cJSON *page = satori::StoreMessageList(store, "wxid_order", *token ? token : nullptr, "before", 2, "asc");
            if (!page) {
                Check(false, "history page");
                break;
            }
            const cJSON *data = Item(page, "data");
            // Each page is oldest-first; pages arrive newest-first, so fill from the back.
            for (int i = cJSON_GetArraySize(data) - 1; i >= 0 && collected_count < 16; --i)
                snprintf(collected[collected_count++], sizeof(collected[0]), "%s",
                         Str(cJSON_GetArrayItem(data, i), "id"));
            const char *prev = Str(page, "prev");
            if (!*prev) {
                cJSON_Delete(page);
                break;
            }
            snprintf(token, sizeof(token), "%s", prev);
            cJSON_Delete(page);
        }
        bool backwards_ok = collected_count == 7;
        for (int i = 0; backwards_ok && i < 7; ++i) backwards_ok = !strcmp(collected[i], expected[6 - i]);
        Check(backwards_ok, "walking back 2 at a time visits every message once, in (time, id) order");
        // Forwards from the oldest, following next.
        cJSON *first = satori::StoreMessageList(store, "wxid_order", "600", "after", 3, "asc");
        Check(first && cJSON_GetArraySize(Item(first, "data")) == 3 &&
                  !strcmp(Str(cJSON_GetArrayItem(Item(first, "data"), 0), "id"), "605") &&
                  !strcmp(Str(cJSON_GetArrayItem(Item(first, "data"), 2), "id"), "602"),
              "after 600: the late arrival comes next, then the tied pair in id order");
        Check(first && !strcmp(Str(first, "next"), "602"), "next token is the newest of the page");
        cJSON *second = satori::StoreMessageList(store, "wxid_order", Str(first, "next"), "after", 10, "asc");
        Check(second && cJSON_GetArraySize(Item(second, "data")) == 3 &&
                  !strcmp(Str(cJSON_GetArrayItem(Item(second, "data"), 0), "id"), "603") && !Item(second, "next"),
              "the remainder, and no next at the newest end");
        cJSON_Delete(first);
        cJSON_Delete(second);
        cJSON *middle = satori::StoreMessageList(store, "wxid_order", "602", "around", 3, "asc");
        Check(middle && cJSON_GetArraySize(Item(middle, "data")) == 3 &&
                  !strcmp(Str(cJSON_GetArrayItem(Item(middle, "data"), 0), "id"), "601") &&
                  !strcmp(Str(cJSON_GetArrayItem(Item(middle, "data"), 1), "id"), "602") &&
                  !strcmp(Str(cJSON_GetArrayItem(Item(middle, "data"), 2), "id"), "603"),
              "around a tied row");
        cJSON_Delete(middle);
        Check(satori::StoreMessageList(store, "wxid_order", "424242", "before", 5, "asc") == nullptr,
              "a token for a row that is gone is refused");
    }
    // Messages carry the same resources as events, so a history page is as usable as a live one.
    cJSON *listed = satori::StoreMessageList(store, "wxid_xyz", nullptr, nullptr, 50, nullptr);
    Check(listed && cJSON_GetArraySize(Item(listed, "data")) == 4, "message.list skips the system tip");
    cJSON_Delete(listed);
    // message.get by local id, or by WeChat's server id.
    cJSON *one = satori::StoreMessageGet(store, "123@chatroom", "3");
    Check(one != nullptr, "message.get");
    Check(one && !strcmp(Nested(one, "user", "id"), "self_wxid"), "message.get author");
    cJSON_Delete(one);
    cJSON *by_server = satori::StoreMessageGet(store, "123@chatroom", "1003");
    Check(by_server && !strcmp(Str(by_server, "id"), "3"), "message.get accepts the server id");
    cJSON_Delete(by_server);
    Check(satori::StoreMessageGet(store, "123@chatroom", "99999") == nullptr, "message.get missing");
    Check(satori::StoreMessageGet(store, "123@chatroom", "7") == nullptr, "a revoke marker is not a message");
    Check(satori::StoreMessageGet(store, "123@chatroom", "6") == nullptr, "and neither is a tip in another channel");
    Check(satori::StoreMessageGet(store, "123@chatroom", "1' OR '1'='1") == nullptr, "message.get takes digits only");

    satori::g_test_store = store;
    // ---- media files ---------------------------------------------------------------------------
    char pathbuf[1400], type[64];
    // Image 4: th_<md5> under image2/<2>/<2>/; a usable hd copy wins over the small one.
    snprintf(step, sizeof(step), "%s/image2", account);
    MakeDir(step);
    snprintf(step, sizeof(step), "%s/image2/01", account);
    MakeDir(step);
    snprintf(step, sizeof(step), "%s/image2/01/23", account);
    MakeDir(step);
    const unsigned char jpeg[] = {0xFF, 0xD8, 0xFF, 0xE0, 0, 0x10, 'J', 'F', 'I', 'F'};
    snprintf(step, sizeof(step), "%s/image2/01/23/th_0123456789abcdef0123456789abcdef", account);
    WriteFile(step, jpeg, sizeof(jpeg));
    Check(satori::StoreMediaFile(store, "image", 4, pathbuf, sizeof(pathbuf), type, sizeof(type)) &&
              !strcmp(type, "image/jpeg") && Contains(pathbuf, "/th_0123456789abcdef0123456789abcdef"),
          "image falls back to the thumbnail");
    snprintf(step, sizeof(step), "%s/image2/01/23/th_0123456789abcdef0123456789abcdefhd", account);
    WriteFile(step, jpeg, sizeof(jpeg));
    Check(satori::StoreMediaFile(store, "image", 4, pathbuf, sizeof(pathbuf), type, sizeof(type)) &&
              Contains(pathbuf, "hd"),
          "the hd copy is preferred");
    // The original is WeChat's wxgf container: unusable, so it must be passed over, not served.
    snprintf(step, sizeof(step), "%s/image2/01/23/0123456789abcdef0123456789abcdef.jpg", account);
    WriteFile(step, "wxgf\x01\x02\x03\x04\x05", 9);
    Check(satori::StoreMediaFile(store, "image", 4, pathbuf, sizeof(pathbuf), type, sizeof(type)) &&
              Contains(pathbuf, "hd"),
          "a wxgf original is skipped");
    Check(!satori::StoreMediaFile(store, "image", 5, pathbuf, sizeof(pathbuf), type, sizeof(type)),
          "no image path: nothing");
    Check(!satori::StoreMediaFile(store, "nonsense", 4, pathbuf, sizeof(pathbuf), type, sizeof(type)), "unknown kind");
    Check(!satori::StoreMediaFile(store, "image", 99999, pathbuf, sizeof(pathbuf), type, sizeof(type)),
          "unknown message");
    // Voice and video use imgPath as a file name; a hostile one is never turned into a path.
    satori::Wcdb *media_writer = satori::WcdbOpenEx(library, path, nullptr, 0, 0, 0, 0, 0);
    Exec(
        media_writer,
        "INSERT INTO message(msgId,type,isSend,createTime,talker,content,imgPath) VALUES(500,34,0,1,'wxid_v','<msg/>','amr_44083309252650814af57ce101')");
    Exec(
        media_writer,
        "INSERT INTO message(msgId,type,isSend,createTime,talker,content,imgPath) VALUES(501,43,0,1,'wxid_v','wxid_v:7:0','2609280159429751')");
    Exec(
        media_writer,
        "INSERT INTO message(msgId,type,isSend,createTime,talker,content,imgPath) VALUES(502,34,0,1,'wxid_v','<msg/>','../../../etc/passwd')");
    Exec(
        media_writer,
        "INSERT INTO message(msgId,type,isSend,createTime,talker,content,imgPath) VALUES(503,6,0,1,'wxid_v','<msg/>','')");
    snprintf(step, sizeof(step), "INSERT INTO appattach VALUES(503,'%s/app/files/report.pdf')", directory);
    Exec(media_writer, step);
    Exec(
        media_writer,
        "INSERT INTO message(msgId,type,isSend,createTime,talker,content,imgPath) VALUES(504,6,0,1,'wxid_v','<msg/>','')");
    Exec(media_writer, "INSERT INTO appattach VALUES(504,'/etc/passwd')");
    satori::WcdbClose(media_writer);
    snprintf(step, sizeof(step), "%s/voice2", account);
    MakeDir(step);
    snprintf(step, sizeof(step), "%s/voice2/30", account);
    MakeDir(step);
    snprintf(step, sizeof(step), "%s/voice2/30/96", account);
    MakeDir(step); // md5("amr_4408...") starts 3096
    snprintf(step, sizeof(step), "%s/voice2/30/96/msg_amr_44083309252650814af57ce101.amr", account);
    WriteFile(step, "\x02#!SILK_V3", 10);
    Check(satori::StoreMediaFile(store, "voice", 500, pathbuf, sizeof(pathbuf), type, sizeof(type)) &&
              !strcmp(type, "audio/silk"),
          "voice resolves through md5 of its name");
    snprintf(step, sizeof(step), "%s/video", account);
    MakeDir(step);
    snprintf(step, sizeof(step), "%s/video/2609280159429751.mp4", account);
    WriteFile(step,
              "\0\0\0\x20"
              "ftypisom",
              12);
    snprintf(step, sizeof(step), "%s/video/2609280159429751.jpg", account);
    WriteFile(step, jpeg, sizeof(jpeg));
    Check(satori::StoreMediaFile(store, "video", 501, pathbuf, sizeof(pathbuf), type, sizeof(type)) &&
              !strcmp(type, "video/mp4"),
          "video");
    Check(satori::StoreMediaFile(store, "videothumb", 501, pathbuf, sizeof(pathbuf), type, sizeof(type)) &&
              !strcmp(type, "image/jpeg"),
          "video thumbnail");
    Check(!satori::StoreMediaFile(store, "voice", 502, pathbuf, sizeof(pathbuf), type, sizeof(type)),
          "a path-traversal file name is refused");
    snprintf(step, sizeof(step), "%s/app/files", directory);
    MakeDir(step);
    snprintf(step, sizeof(step), "%s/app/files/report.pdf", directory);
    WriteFile(step, "%PDF-1.4", 8);
    Check(satori::StoreMediaFile(store, "file", 503, pathbuf, sizeof(pathbuf), type, sizeof(type)) &&
              !strcmp(type, "application/pdf"),
          "a file inside the app's own data");
    Check(!satori::StoreMediaFile(store, "file", 504, pathbuf, sizeof(pathbuf), type, sizeof(type)),
          "a path outside WeChat's folders is never served");

    // ---- the proxy route: only links this module signed resolve ----------------------------------
    {
        satori::MediaFile file;
        char signature[satori::kMediaSigSize], route[200];
        satori::MediaSign("self_wxid", "image", "4", signature);
        snprintf(route, sizeof(route), "_msg/image/4/%s", signature);
        Check(satori::WeChatMediaResolver("self_wxid", route, &file) && !strcmp(file.content_type, "image/jpeg") &&
                  Contains(file.path, "hd"),
              "a signed link resolves to its file");
        Check(!satori::WeChatMediaResolver("someone_else", route, &file), "the link is bound to the login");
        snprintf(route, sizeof(route), "_msg/video/4/%s", signature);
        Check(!satori::WeChatMediaResolver("self_wxid", route, &file), "the kind is part of the signature");
        snprintf(route, sizeof(route), "_msg/image/5/%s", signature);
        Check(!satori::WeChatMediaResolver("self_wxid", route, &file),
              "and so is the id: enumerating ids gets nothing");
        Check(!satori::WeChatMediaResolver("self_wxid", "_msg/image/4/0000000000000000", &file), "a forged signature");
        Check(!satori::WeChatMediaResolver("self_wxid", "_msg/image/4", &file), "a link without a signature");
        Check(!satori::WeChatMediaResolver("self_wxid", "_msg/image/4x/abcdabcdabcdabcd", &file), "a non-numeric id");
        Check(!satori::WeChatMediaResolver("self_wxid", "_tmp/whatever", &file), "other routes are not ours");
        satori::MediaSign("self_wxid", "voice", "500", signature);
        snprintf(route, sizeof(route), "_msg/voice/500/%s", signature);
        Check(satori::WeChatMediaResolver("self_wxid", route, &file) && !strcmp(file.content_type, "audio/silk"),
              "a signed voice link");
        satori::g_test_store = nullptr;
        satori::MediaSign("self_wxid", "image", "4", signature);
        snprintf(route, sizeof(route), "_msg/image/4/%s", signature);
        Check(!satori::WeChatMediaResolver("self_wxid", route, &file), "no store yet: nothing resolves");
        satori::g_test_store = store;
    }

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
    Check(member_page && cJSON_GetArraySize(Item(member_page, "data")) == 1 && *Str(member_page, "next"),
          "member page cursor");
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
    Check(member_roles && !strcmp(Str(cJSON_GetArrayItem(Item(member_roles, "data"), 0), "id"), "admin"),
          "admin role from roomdata");
    cJSON_Delete(member_roles);
    // Without a roomdata cache the admin bit is unknown, so the member stays a plain member.
    cJSON *plain_roles = satori::StoreMemberRoleList(store, "456@chatroom", "wxid_plain");
    Check(plain_roles && !strcmp(Str(cJSON_GetArrayItem(Item(plain_roles, "data"), 0), "id"), "member"),
          "plain member without roomdata");
    cJSON_Delete(plain_roles);
    cJSON *none_roles = satori::StoreMemberRoleList(store, "123@chatroom", "nobody");
    Check(none_roles && cJSON_GetArraySize(Item(none_roles, "data")) == 0, "non-member has no roles");
    cJSON_Delete(none_roles);

    // ---- the owner's own rows versus the module's own sends ---------------------------------------
    {
        const long long base_mark = satori::StoreWatermark(store);
        satori::Wcdb *own_writer = satori::WcdbOpenEx(library, path, nullptr, 0, 0, 0, 0, 0);
        // 600: typed by the owner. 601: created by message.create (known by local id). 602: written
        // while a message.create to that talker is open. 603: after it closed, still in the grace.
        // 604: another talker, no send open. 605: an incoming row is never marked.
        Exec(
            own_writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(9600,8000,1,1,1700004000000,'wxid_own','https://example.com/typed')");
        Exec(
            own_writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(9601,8001,1,1,1700004001000,'wxid_bot','sent by id')");
        Exec(
            own_writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(9602,8002,1,1,1700004002000,'wxid_win','sent in window')");
        Exec(
            own_writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(9603,8003,1,1,1700004003000,'wxid_win','sent in grace')");
        Exec(
            own_writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(9604,8004,1,1,1700004004000,'wxid_elsewhere','typed elsewhere')");
        Exec(
            own_writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(9605,8005,1,0,1700004005000,'wxid_own','incoming')");
        satori::WcdbClose(own_writer);
        satori::StoreNoteSent(store, 9601);
        satori::StoreSendBegin(store, "wxid_win");
        Sink own;
        Drain(store, base_mark, &own);
        satori::StoreSendEnd(store, "wxid_win");
        Check(own.count == 6, "every row of the scenario is delivered");
        bool marked[6] = {};
        for (int i = 0; i < own.count && i < 6; ++i) {
            cJSON *event = cJSON_Parse(own.events[i]);
            marked[i] = cJSON_IsTrue(Item(Item(event, "satori_wx"), "manual_self"));
            cJSON_Delete(event);
        }
        Check(marked[0], "an owner-typed row is manual_self");
        Check(!marked[1], "a row the module created is not (local id)");
        Check(!marked[2] && !marked[3], "rows written while the module was sending to that talker are not (window)");
        Check(marked[4], "another talker's own row is still manual_self");
        Check(!marked[5], "an incoming row is never marked");
        // After the window closes the grace still covers a late poll.
        satori::Wcdb *late_writer = satori::WcdbOpenEx(library, path, nullptr, 0, 0, 0, 0, 0);
        Exec(
            late_writer,
            "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(9606,8006,1,1,1700004006000,'wxid_win','late poll')");
        satori::WcdbClose(late_writer);
        Sink late;
        Drain(store, 9605, &late);
        cJSON *late_event = late.count ? cJSON_Parse(late.events[0]) : nullptr;
        Check(late.count == 1 && !Item(late_event, "satori_wx"),
              "a poll that lags a moment behind the send still recognises it");
        cJSON_Delete(late_event);
    }

    satori::DestroyStore(store);
    char cleanup[600];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", directory);
    if (system(cleanup)) fprintf(stderr, "warning: could not remove %s\n", directory);
    if (failures) {
        fprintf(stderr, "%d store test(s) failed\n", failures);
        return 1;
    }
    printf("store tests: PASS\n");
    return 0;
}
