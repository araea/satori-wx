// Host tests for the read-only WeChat message store.
// A plaintext SQLite file stands in for EnMicroMsg.db; rows are mapped to Satori events.
#include "wx_store.h"
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
    satori::WcdbClose(writer);

    satori::Store *store = satori::CreateStoreEx(library, path, nullptr, 0, 0, "self_wxid");
    Check(store != nullptr, "open store");
    if (!store) { fprintf(stderr, "store error: %s\n", satori::StoreError(nullptr)); return 1; }
    Check(satori::StoreWatermark(store) == 5, "watermark before poll");
    Sink sink;
    const long long watermark = satori::StorePoll(store, 0, 7, Emit, &sink);
    Check(watermark == 5, "poll returns watermark");
    Check(sink.count == 4, "media row skipped");
    if (sink.count == 4) {
        cJSON *group = cJSON_Parse(sink.events[0]);
        Check(!strcmp(Str(group, "type"), "message-created"), "event type");
        Check(Num(group, "timestamp") == 1700000000000.0, "timestamp");
        Check(Num(Item(group, "login"), "sn") == 7, "login sn");
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
    Check(satori::StorePoll(store, watermark, 7, Emit, &again) == 5 && again.count == 0, "no replay");
    satori::DestroyStore(store);
    unlink(path);
    rmdir(directory);
    if (failures) { fprintf(stderr, "%d store test(s) failed\n", failures); return 1; }
    printf("store tests: PASS\n");
    return 0;
}
