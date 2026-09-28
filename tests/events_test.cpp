// Host tests for the event scanner: recalled messages, group roster changes and friendships,
// driven by mutating a plaintext SQLite file that has WeChat's column layout.
#include "wx_events.h"
#include "wcdb.h"
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
struct Sink {
    int count = 0;
    bool refuse = false;
    char *events[64] = {};
    void Clear() { for (int i = 0; i < count; ++i) { free(events[i]); events[i] = nullptr; } count = 0; }
    ~Sink() { Clear(); }
};
bool Emit(void *context, const char *event) {
    auto *sink = static_cast<Sink *>(context);
    if (sink->refuse || sink->count >= 64) return false;
    sink->events[sink->count++] = strdup(event);
    return true;
}
const cJSON *Item(const cJSON *o, const char *k) { return o ? cJSON_GetObjectItemCaseSensitive(o, k) : nullptr; }
const char *Str(const cJSON *o, const char *k) { const cJSON *v = Item(o, k); return cJSON_IsString(v) ? v->valuestring : ""; }
const char *Nested(const cJSON *o, const char *a, const char *b) { return Str(Item(o, a), b); }
bool Exec(satori::Wcdb *db, const char *sql) {
    if (satori::WcdbExec(db, sql)) return true;
    fprintf(stderr, "SQL failed: %s\n  %s\n", satori::WcdbError(db), sql);
    return false;
}
// Returns the parsed i-th event, or null.
cJSON *Event(const Sink &sink, int i) { return i < sink.count ? cJSON_Parse(sink.events[i]) : nullptr; }
// Runs `n` passes at increasing wall-clock times.
long long g_now = 1700000000000LL;
void Pass(satori::Scanner *scanner, Sink *sink, int n = 1) {
    for (int i = 0; i < n; ++i) { satori::ScannerStep(scanner, Emit, sink, g_now); g_now += 3000; }
}
// Friends are re-read on every 4th pass; this is the number of passes to see a change twice.
constexpr int kFriendPasses = 9;
} // namespace

int main() {
    const char *library = getenv("SATORI_WCDB_LIB");
    if (!library || !*library) { printf("events tests: SKIP (set SATORI_WCDB_LIB)\n"); return 0; }
    const char *base = getenv("SATORI_ACCOUNT_TMP");
    if (!base || !*base) base = getenv("TMPDIR");
    if (!base || !*base) base = ".";
    char directory[512], path[640];
    snprintf(directory, sizeof(directory), "%s/satori-events-XXXXXX", base);
    if (!mkdtemp(directory)) { fprintf(stderr, "mkdtemp failed\n"); return 2; }
    snprintf(path, sizeof(path), "%s/msg.db", directory);
    const int created = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (created >= 0) close(created);
    satori::Wcdb *db = satori::WcdbOpenEx(library, path, nullptr, 0, 0, 0, 0, 0);
    Check(db != nullptr, "open writer");
    if (!db) return 1;
    Exec(db, "CREATE TABLE message(msgId INTEGER PRIMARY KEY, msgSvrId INTEGER, type INT, status INT, isSend INT, isShowTimer INTEGER, "
             "createTime INTEGER, talker TEXT, content TEXT, imgPath TEXT, reserved TEXT, lvbuffer BLOB)");
    Exec(db, "CREATE TABLE chatroom(chatroomname TEXT PRIMARY KEY, memberlist TEXT, displayname TEXT, roomowner TEXT, memberCount INTEGER, roomdata BLOB, modifytime INTEGER, chatroomVersion INTEGER)");
    Exec(db, "CREATE TABLE rcontact(username TEXT PRIMARY KEY, alias TEXT, conRemark TEXT, nickname TEXT, type INTEGER, deleteFlag INTEGER)");
    Exec(db, "CREATE TABLE MsgQuote(msgId INTEGER, msgSvrId INTEGER, quotedMsgId INTEGER, quotedMsgSvrId INTEGER, status INTEGER, quotedMsgTalker TEXT)");
    Exec(db, "INSERT INTO rcontact VALUES('self_wxid','','','我',3,0), ('wxid_a','','','甲',3,0), ('wxid_b','','','乙',3,0), ('wxid_c','','','丙',3,0), "
             "('wxid_stranger','','','路人',4,0), ('wxid_starred','','','星标',67,0), ('gh_news','','','公众号',3,0), ('123@chatroom','','','群名',2,0)");
    Exec(db, "INSERT INTO chatroom VALUES('123@chatroom','self_wxid;wxid_a;wxid_b','','wxid_a',3,NULL,1000,0)");
    Exec(db, "INSERT INTO chatroom VALUES('456@chatroom','wxid_a;wxid_b','','wxid_a',2,NULL,1000,0)");
    // A message the poller will announce (so its author is remembered), one recalled *before*
    // the scanner starts, and a private one.
    Exec(db, "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(10,9010,1,0,1699999990000,'123@chatroom','wxid_a:\n收回我')");
    Exec(db, "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(11,9011,285222674,0,1699999991000,'123@chatroom','\"某人\" 撤回了一条消息')");
    Exec(db, "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(12,9012,1,0,1699999992000,'wxid_b','我说错了')");

    satori::Store *store = satori::CreateStoreEx(library, path, nullptr, 0, 0, "self_wxid");
    Check(store != nullptr, "open store");
    if (!store) return 1;
    // The poller announces the two live messages, which is what teaches the store their authors.
    struct Drain { static bool Take(void *, const char *) { return true; } };
    satori::StorePoll(store, 0, 1, Drain::Take, nullptr);
    satori::Scanner *scanner = satori::CreateScanner(store, 1);
    Check(scanner != nullptr, "create scanner");
    Sink sink;

    // ---- baseline: everything that already exists is silent ----------------------------------------
    Pass(scanner, &sink, 2);
    Check(sink.count == 0, "the state that exists at startup announces nothing (recall #11 included)");

    // ---- a recall: the row is rewritten in place -------------------------------------------------------
    Exec(db, "UPDATE message SET type = 268445456, content = '\"甲\" 撤回了一条消息' WHERE msgId = 10");
    Pass(scanner, &sink);
    Check(sink.count == 1, "one message-deleted for the recalled message");
    if (sink.count == 1) {
        cJSON *event = Event(sink, 0);
        Check(!strcmp(Str(event, "type"), "message-deleted"), "type");
        Check(!strcmp(Nested(event, "message", "id"), "10"), "message id is the local id the message was announced with");
        Check(!strcmp(Nested(event, "channel", "id"), "123@chatroom") && !strcmp(Nested(event, "guild", "id"), "123@chatroom"), "channel and guild");
        Check(!strcmp(Nested(event, "user", "id"), "wxid_a") && !strcmp(Nested(Item(event, "message"), "user", "id"), "wxid_a"), "the author is remembered from the announcement");
        Check(!strcmp(Nested(event, "user", "nick"), "甲"), "and described");
        Check(Item(Item(event, "login"), "sn") && Item(event, "timestamp"), "login and timestamp");
        cJSON_Delete(event);
    }
    sink.Clear();
    Pass(scanner, &sink, 2);
    Check(sink.count == 0, "a recall is announced once");
    // A private recall whose author we never saw: the talker is the only candidate.
    Exec(db, "UPDATE message SET type = 285222674, content = '对方撤回了一条消息' WHERE msgId = 12");
    Pass(scanner, &sink);
    if (sink.count == 1) {
        cJSON *event = Event(sink, 0);
        Check(!strcmp(Nested(event, "user", "id"), "wxid_b") && !Item(event, "guild"), "private recall: the talker, no guild");
        cJSON_Delete(event);
    } else Check(false, "private recall announced");
    sink.Clear();

    // ---- group roster ---------------------------------------------------------------------------------------
    Exec(db, "UPDATE chatroom SET memberlist = 'self_wxid;wxid_a;wxid_b;wxid_c', memberCount = 4, modifytime = 2000 WHERE chatroomname = '123@chatroom'");
    Pass(scanner, &sink);
    Check(sink.count == 0, "a roster change must be seen twice before it is believed");
    Pass(scanner, &sink);
    Check(sink.count == 1, "then it is announced");
    if (sink.count == 1) {
        cJSON *event = Event(sink, 0);
        Check(!strcmp(Str(event, "type"), "guild-member-added"), "member added");
        Check(!strcmp(Nested(event, "guild", "id"), "123@chatroom") && !strcmp(Nested(event, "guild", "name"), "群名"), "guild with its name");
        Check(!strcmp(Nested(event, "user", "id"), "wxid_c") && !strcmp(Nested(event, "user", "nick"), "丙"), "the new member");
        Check(!strcmp(Nested(Item(event, "member"), "user", "id"), "wxid_c"), "as a guild member too");
        cJSON_Delete(event);
    }
    sink.Clear();
    // A flicker that reverts (WeChat rewrites the row in steps during a sync) is never announced.
    Exec(db, "UPDATE chatroom SET memberlist = 'self_wxid;wxid_a', memberCount = 2, modifytime = 2500 WHERE chatroomname = '123@chatroom'");
    Pass(scanner, &sink);
    Exec(db, "UPDATE chatroom SET memberlist = 'self_wxid;wxid_a;wxid_b;wxid_c', memberCount = 4, modifytime = 2600 WHERE chatroomname = '123@chatroom'");
    Pass(scanner, &sink, 3);
    Check(sink.count == 0, "a transient roster that reverts announces nothing");
    // Reordering the same members is not a change.
    Exec(db, "UPDATE chatroom SET memberlist = 'wxid_c;self_wxid;wxid_b;wxid_a', modifytime = 2700 WHERE chatroomname = '123@chatroom'");
    Pass(scanner, &sink, 3);
    Check(sink.count == 0, "reordering is not a roster change");
    Exec(db, "UPDATE chatroom SET memberlist = 'wxid_c;self_wxid;wxid_a', memberCount = 3, modifytime = 3000 WHERE chatroomname = '123@chatroom'");
    Pass(scanner, &sink, 2);
    Check(sink.count == 1, "one member left");
    if (sink.count == 1) {
        cJSON *event = Event(sink, 0);
        Check(!strcmp(Str(event, "type"), "guild-member-removed") && !strcmp(Nested(event, "user", "id"), "wxid_b"), "member removed, described from the contact table");
        cJSON_Delete(event);
    }
    sink.Clear();
    // We are removed: that is the guild going away, not one more member leaving.
    Exec(db, "UPDATE chatroom SET memberlist = 'wxid_c;wxid_a', memberCount = 2, modifytime = 4000 WHERE chatroomname = '123@chatroom'");
    Pass(scanner, &sink, 2);
    Check(sink.count == 1, "we were removed");
    if (sink.count == 1) {
        cJSON *event = Event(sink, 0);
        Check(!strcmp(Str(event, "type"), "guild-removed") && !strcmp(Nested(event, "guild", "id"), "123@chatroom") && !Item(event, "user"), "guild-removed, no member");
        cJSON_Delete(event);
    }
    sink.Clear();
    // Joining a group: a room row that appears with us in it.
    Exec(db, "INSERT INTO chatroom VALUES('789@chatroom','self_wxid;wxid_a','','wxid_a',2,NULL,5000,0)");
    Pass(scanner, &sink);
    Check(sink.count == 1, "a new room that lists us");
    if (sink.count == 1) {
        cJSON *event = Event(sink, 0);
        Check(!strcmp(Str(event, "type"), "guild-added") && !strcmp(Nested(event, "guild", "id"), "789@chatroom"), "guild-added");
        cJSON_Delete(event);
    }
    sink.Clear();
    // A room we were never in appearing (or vanishing) is nobody's business.
    Exec(db, "INSERT INTO chatroom VALUES('999@chatroom','wxid_a;wxid_b','','wxid_a',2,NULL,5000,0)");
    Pass(scanner, &sink, 2);
    Exec(db, "DELETE FROM chatroom WHERE chatroomname = '456@chatroom'");
    Pass(scanner, &sink, 2);
    Check(sink.count == 0, "rooms without us are silent");
    Exec(db, "DELETE FROM chatroom WHERE chatroomname = '789@chatroom'");
    Pass(scanner, &sink);
    Check(sink.count == 1, "the room we were in was deleted");
    if (sink.count == 1) {
        cJSON *event = Event(sink, 0);
        Check(!strcmp(Str(event, "type"), "guild-removed") && !strcmp(Nested(event, "guild", "id"), "789@chatroom"), "guild-removed for a deleted room");
        cJSON_Delete(event);
    }
    sink.Clear();

    // ---- friends ---------------------------------------------------------------------------------------------------
    // The starred friend (type 67) is a friend; the stranger and the official account are not.
    Exec(db, "UPDATE rcontact SET type = 3 WHERE username = 'wxid_stranger'");
    Pass(scanner, &sink, kFriendPasses);
    Check(sink.count == 1, "a stranger became a friend");
    if (sink.count == 1) {
        cJSON *event = Event(sink, 0);
        Check(!strcmp(Str(event, "type"), "friend-added") && !strcmp(Nested(event, "user", "id"), "wxid_stranger") &&
                  !strcmp(Nested(Item(event, "friend"), "user", "id"), "wxid_stranger"), "friend-added");
        cJSON_Delete(event);
    }
    sink.Clear();
    Exec(db, "UPDATE rcontact SET type = 4 WHERE username = 'wxid_starred'");
    Pass(scanner, &sink, kFriendPasses);
    Check(sink.count == 1, "a friend was removed");
    if (sink.count == 1) {
        cJSON *event = Event(sink, 0);
        Check(!strcmp(Str(event, "type"), "friend-removed") && !strcmp(Nested(event, "user", "id"), "wxid_starred"), "friend-removed (a flagged friend counts as a friend)");
        cJSON_Delete(event);
    }
    sink.Clear();
    Exec(db, "UPDATE rcontact SET type = 67 WHERE username = 'wxid_starred'");
    Pass(scanner, &sink, kFriendPasses);
    Check(sink.count == 1 && !strcmp(Nested(cJSON_Parse(sink.events[0]), "user", "id"), "wxid_starred") &&
              !strcmp(Str(cJSON_Parse(sink.events[0]), "type"), "friend-added"), "the starred friend coming back is a friend-added");
    sink.Clear();
    // A contact that flickers (deleted, then restored) between two reads is not a friendship
    // changing. Four passes hold exactly one friend read, so the change is seen once.
    Exec(db, "UPDATE rcontact SET deleteFlag = 1 WHERE username = 'wxid_a'");
    Pass(scanner, &sink, 4);
    Exec(db, "UPDATE rcontact SET deleteFlag = 0 WHERE username = 'wxid_a'");
    Pass(scanner, &sink, kFriendPasses);
    Check(sink.count == 0, "flicker and reverts are not friendships changing");

    // ---- a refused event is not lost ----------------------------------------------------------------------------------
    Exec(db, "UPDATE message SET type = 268445456 WHERE msgId = 10");  // already recalled: nothing new
    Exec(db, "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(20,9020,285222674,0,1700000000000,'wxid_b','x')");
    Exec(db, "INSERT INTO message(msgId,msgSvrId,type,isSend,createTime,talker,content) VALUES(21,9021,285222674,0,1700000001000,'wxid_b','x')");
    sink.refuse = true;
    Pass(scanner, &sink);
    Check(sink.count == 0, "the bus refused them");
    sink.refuse = false;
    Pass(scanner, &sink);
    Check(sink.count == 2 && !strcmp(Nested(cJSON_Parse(sink.events[0]), "message", "id"), "20") &&
              !strcmp(Nested(cJSON_Parse(sink.events[1]), "message", "id"), "21"), "both are delivered next pass, in order");
    Check(satori::ScannerDropped(scanner) == 0, "nothing dropped");
    sink.Clear();
    Pass(scanner, &sink, 2);
    Check(sink.count == 0, "and not delivered twice");

    satori::DestroyScanner(scanner);
    satori::DestroyStore(store);
    satori::WcdbClose(db);
    char cleanup[600];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", directory);
    if (system(cleanup)) fprintf(stderr, "warning: could not remove %s\n", directory);
    if (failures) { fprintf(stderr, "%d events test(s) failed\n", failures); return 1; }
    printf("events tests: PASS\n");
    return 0;
}
