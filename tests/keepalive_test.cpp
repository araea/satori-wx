// Host-side tests for the resident keeper: what the notification says, the status block, the
// published state file, the automatic wake-lock hold, and (the point of the design) that the keeper
// thread does not poll. There is no JVM here: the keeper only touches Java once a VM has been
// handed to it, so everything but the JNI calls themselves can be checked without a device.
#include "protocol.h"
#include "server.h"
#include "wx_keepalive.h"
#include "wx_send.h"
#include "vendor/cjson/cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

namespace {
int failures = 0;

void Check(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

const cJSON *Field(const cJSON *keep, const char *name) { return cJSON_GetObjectItemCaseSensitive(keep, name); }

void CheckBool(const cJSON *keep, const char *name, bool expected) {
    const cJSON *field = Field(keep, name);
    Check(field && cJSON_IsBool(field) && (cJSON_IsTrue(field) == expected), name);
}

void CheckNumber(const cJSON *keep, const char *name, double expected) {
    const cJSON *field = Field(keep, name);
    Check(field && cJSON_IsNumber(field) && field->valuedouble == expected, name);
}

void SleepMs(long ms) {
    const timespec delay{ms / 1000, (ms % 1000) * 1000000};
    nanosleep(&delay, nullptr);
}

// The keepalive block as the status endpoint would build it; the caller frees `status`.
cJSON *Block(cJSON **status) {
    *status = cJSON_CreateObject();
    satori::KeepaliveStatus(*status);
    return cJSON_GetObjectItemCaseSensitive(*status, "keepalive");
}

double Number(const char *name) {
    cJSON *status;
    const cJSON *field = Field(Block(&status), name);
    const double value = field ? field->valuedouble : -1;
    cJSON_Delete(status);
    return value;
}

// Reads and parses the published file; null when it is not there yet.
cJSON *ReadState(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return nullptr;
    char buffer[2048];
    const size_t n = fread(buffer, 1, sizeof(buffer) - 1, file);
    fclose(file);
    buffer[n] = 0;
    return cJSON_Parse(buffer);
}

// Waits up to `ms` for the published file to satisfy `ok`; returns the parsed document (caller frees)
// or null.
template <typename Predicate> cJSON *WaitState(const char *path, long ms, Predicate ok) {
    for (long waited = 0; waited <= ms; waited += 10) {
        cJSON *doc = ReadState(path);
        if (doc && ok(doc)) return doc;
        cJSON_Delete(doc);
        SleepMs(10);
    }
    return nullptr;
}

int g_heard = 0;
void Heard() { ++g_heard; }

void TestRender() {
    setenv("TZ", "UTC", 1);
    tzset();
    satori::KeepaliveInputs in{};
    in.online = true;
    in.listening = true;
    in.port = 5601;
    in.send_state = satori::kSendReady;
    in.now_ms = 1760000000000LL; // 2025-10-09 08:53:20 UTC
    satori::KeepaliveView view;

    // What the resident notification says. The collapsed row is the title plus one line; the
    // expanded body repeats that line, then says when the service came up and what is wrong with
    // sending, if anything. It never repeats the title and says nothing about the wake lock.
    in.serving_since_ms = 0;
    satori::KeepaliveRender(in, &view);
    Check(!strcmp(view.title, "知言 · 运行中"), "notification: title while serving");
    Check(strstr(view.text, "等待客户端连接") != nullptr, "notification: waiting for a client");
    Check(!strstr(view.big, "知言"), "notification: the body does not repeat the title");
    Check(!strcmp(view.big, view.text) && !view.timer, "notification: with no start time the body is just the line");

    in.clients = 3;
    in.serving_since_ms = in.now_ms - (2 * 3600 + 5 * 60) * 1000LL; // earlier today, 06:48 UTC
    satori::KeepaliveRender(in, &view);
    Check(strstr(view.text, "已连接 3 个客户端") != nullptr, "notification: client count in the line");
    Check(!strstr(view.big, "运行中"), "notification: body has not the state again");
    Check(strstr(view.big, view.text) == view.big && strstr(view.big, "\n上线于 06:48") != nullptr,
          "notification: body is the line, then when it came up");
    Check(view.timer, "notification: the system chronometer shows the uptime");
    Check(!strstr(view.big, "已在线"), "notification: no hand-rolled uptime text left to redraw");
    Check(!strstr(view.big, "唤醒锁") && !strstr(view.big, "CPU") && !strstr(view.big, "Wi-Fi"),
          "notification: no wake-lock detail");
    Check(!strstr(view.big, "发送") && !strstr(view.text, "发送"),
          "notification: nothing about sending while it works");

    in.serving_since_ms = in.now_ms - 30 * 3600 * 1000LL; // yesterday
    satori::KeepaliveRender(in, &view);
    Check(strstr(view.big, "\n上线于 10-8 02:53") != nullptr, "notification: a start on another day carries its date");

    in.send_state = satori::kSendIncompatible;
    satori::KeepaliveRender(in, &view);
    Check(strstr(view.big, "发送与当前微信版本不兼容") != nullptr, "notification: says when sending cannot work");
    Check(strstr(view.text, "不兼容") == nullptr, "notification: ... in the body, not the collapsed row");
    in.send_state = satori::kSendWaiting;
    satori::KeepaliveRender(in, &view);
    Check(strstr(view.big, "发送") == nullptr, "notification: a sender that is only waiting is not an alarm");

    in.online = false;
    in.serving_since_ms = 0;
    satori::KeepaliveRender(in, &view);
    Check(!strcmp(view.title, "知言 · 等待登录") && !strstr(view.big, "\n") && !view.timer,
          "notification: waiting for login has no start time and no timer");
    in.online = true;
    in.listening = false;
    satori::KeepaliveRender(in, &view);
    Check(!strcmp(view.title, "知言 · 服务异常") && strstr(view.text, "未监听") != nullptr && !view.timer,
          "notification: port not listening");
}

void TestStatusShape() {
    // Defaults: nothing posted, no holds, no clients, no login. The user toggle is on: the locks
    // themselves cannot be held here (no JVM), only wanted.
    cJSON *status;
    const cJSON *keep = Block(&status);
    Check(keep && cJSON_IsObject(keep), "keepalive block exists");
    CheckBool(keep, "notification", false);
    CheckBool(keep, "notifications_enabled", false);
    CheckBool(keep, "channel", false);
    CheckBool(keep, "wakelock", true);
    CheckBool(keep, "wakelock_held", false);
    CheckBool(keep, "cpu_held", false);
    CheckBool(keep, "wifi_held", false);
    CheckBool(keep, "sustain_wifi", false);
    CheckNumber(keep, "auto", 0);
    CheckNumber(keep, "clients", 0);
    CheckNumber(keep, "uptime_ms", 0);
    CheckNumber(keep, "wakes", 0);
    CheckNumber(keep, "event_wakes", 0);
    Check(Field(keep, "service") && Field(keep, "notify") && Field(keep, "wchan") && Field(keep, "oom_score_adj") &&
              Field(keep, "heartbeat_age_ms") && Field(keep, "state_file"),
          "diagnostic fields present");
    cJSON_Delete(status);

    // The connection count is what the notification and the Wi-Fi sustain read.
    satori::g_client_count = 2;
    satori::g_server_ready = true;
    satori::g_login_count = 1;
    keep = Block(&status);
    CheckNumber(keep, "clients", 2);
    CheckNumber(keep, "uptime_ms", 0); // uptime starts when the keeper notices, not here
    cJSON_Delete(status);
    satori::g_client_count = 0;
    satori::g_server_ready = false;
    satori::g_login_count = 0;
}

// The hooks that wake the keeper: a login event changes the count and the primary login, and says so.
void TestHooks() {
    satori::SetStateListener(Heard);
    satori::Hub *hub = satori::CreateHub();
    Check(hub != nullptr, "hub created");
    char id[64] = "x", name[64] = "x";
    Check(!satori::PrimaryLogin(id, sizeof(id), name, sizeof(name)) && !*id && !*name, "no login at first");
    const char *added =
        "{\"type\":\"login-added\",\"login\":{\"sn\":1,\"platform\":\"wechat\",\"adapter\":\"satori-wx\",\"status\":1,"
        "\"user\":{\"id\":\"wxid_abc\",\"name\":\"abc\",\"nick\":\"Nick\"},\"features\":[]}}";
    g_heard = 0;
    char *signal = satori::Apply(hub, added, false);
    Check(signal != nullptr, "login-added applied");
    free(signal);
    Check(g_heard == 1, "a login event tells the listener");
    Check(satori::g_login_count == 1, "login count follows");
    Check(satori::PrimaryLogin(id, sizeof(id), name, sizeof(name)) && !strcmp(id, "wxid_abc") && !strcmp(name, "Nick"),
          "the primary login is the first online one, shown by nick");
    const char *removed =
        "{\"type\":\"login-removed\",\"login\":{\"sn\":1,\"platform\":\"wechat\",\"adapter\":\"satori-wx\",\"status\":0,"
        "\"user\":{\"id\":\"wxid_abc\"},\"features\":[]}}";
    signal = satori::Apply(hub, removed, false);
    free(signal);
    Check(g_heard == 2 && satori::g_login_count == 0 && !satori::PrimaryLogin(id, sizeof(id), name, sizeof(name)),
          "a removed login clears the primary one and tells the listener");
    satori::DestroyHub(hub);
    satori::SetStateListener(nullptr);
    satori::g_login_count = 0;
}

void TestKeeper() {
    char dir[256];
    snprintf(dir, sizeof(dir), "%s/keepalive-test-%d", getenv("SATORI_TMPROOT") ? getenv("SATORI_TMPROOT") : "/tmp",
             static_cast<int>(getpid()));
    mkdir(dir, 0700);
    char state[320], tmp[330];
    snprintf(state, sizeof(state), "%s/files/satori-wx/state.json", dir);
    snprintf(tmp, sizeof(tmp), "%s.tmp", state);
    char files[300];
    snprintf(files, sizeof(files), "%s/files", dir);
    mkdir(files, 0700);

    satori::Config config;
    config.port = 5601;
    snprintf(config.token, sizeof(config.token), "token");
    satori::KeepaliveStart(nullptr, config, dir);

    // The first pass publishes the state as it is, with no event from anyone.
    cJSON *doc = WaitState(state, 2000, [](const cJSON *) { return true; });
    Check(doc != nullptr, "state file: published at start");
    if (doc) {
        CheckNumber(doc, "schema", 1);
        CheckBool(doc, "listening", false);
        CheckNumber(Field(doc, "login"), "online", 0);
        CheckNumber(doc, "serving_since", 0);
        Check(Field(doc, "changed_at") && Field(doc, "version") && Field(doc, "pid") && Field(doc, "send"),
              "state file: carries version, pid, send and when it changed");
        CheckNumber(doc, "pid", static_cast<double>(getpid()));
    }
    cJSON_Delete(doc);
    Check(access(tmp, F_OK) != 0, "state file: no temp file left behind");
    struct stat info{};
    Check(stat(state, &info) == 0 && (info.st_mode & 077) == 0, "state file: private to the app's uid");

    // The point of the design: with nothing happening, the keeper does nothing. No timer, no tick.
    SleepMs(150);
    const double idle = Number("wakes");
    SleepMs(700);
    Check(Number("wakes") == idle, "idle: the keeper thread does not wake up on its own");

    // A change wakes it, once, and the file follows. Several changes in a burst are one pass.
    const double before = Number("wakes");
    satori::g_login_count = 1;
    satori::g_server_ready = true;
    satori::g_client_count = 1;
    for (int i = 0; i < 20; ++i) satori::NotifyStateChanged();
    doc = WaitState(state, 2000, [](const cJSON *d) { return cJSON_IsTrue(Field(d, "listening")); });
    Check(doc != nullptr, "state file: follows a change");
    if (doc) {
        CheckNumber(Field(doc, "login"), "online", 1);
        CheckNumber(doc, "clients", 1);
        Check(Field(doc, "serving_since") && Field(doc, "serving_since")->valuedouble > 0,
              "state file: a serving service has a start time");
    }
    cJSON_Delete(doc);
    SleepMs(100);
    const double after = Number("wakes");
    Check(after > before && after - before <= 3, "a burst of changes is a pass or two, not twenty");
    Check(Number("event_wakes") >= 1, "the wake is counted as an event");
    Check(Number("uptime_ms") >= 0, "uptime is reported once serving");
    SleepMs(300);
    const double settled = Number("wakes");
    SleepMs(500);
    Check(Number("wakes") == settled, "idle again after the change: still no wakes");

    // The same change twice writes nothing new (the document did not change).
    struct stat first{};
    stat(state, &first);
    SleepMs(30);
    satori::NotifyStateChanged();
    SleepMs(200);
    struct stat second{};
    stat(state, &second);
    Check(first.st_ino == second.st_ino && first.st_mtime == second.st_mtime &&
              first.st_mtim.tv_nsec == second.st_mtim.tv_nsec,
          "state file: an unchanged document is not rewritten");

    // The user's wake-lock choice is part of the view; flipping it wakes the keeper and is published.
    bool held = true;
    satori::KeepaliveWakelock(0, &held);
    Check(!held, "wake lock: switched off");
    doc = WaitState(state, 2000, [](const cJSON *d) {
        const cJSON *keep = Field(d, "keepalive");
        return keep && cJSON_IsFalse(Field(keep, "wakelock"));
    });
    Check(doc != nullptr, "state file: carries the wake-lock intent");
    cJSON_Delete(doc);
    satori::KeepaliveWakelock(1, &held);
    Check(held, "wake lock: switched back on");

    // The automatic hold is ref-counted and symmetric even with no JVM attached.
    satori::KeepaliveWakelockBegin();
    satori::KeepaliveWakelockBegin();
    cJSON *status;
    CheckNumber(Block(&status), "auto", 2);
    cJSON_Delete(status);
    satori::KeepaliveWakelockEnd();
    CheckNumber(Block(&status), "auto", 1);
    cJSON_Delete(status);
    satori::KeepaliveWakelockEnd();
    satori::KeepaliveWakelockEnd(); // an unmatched end must not underflow
    CheckNumber(Block(&status), "auto", 0);
    cJSON_Delete(status);

    // The document builder: the timestamp is the caller's to add, so the keeper can compare without it.
    char *plain = satori::KeepaliveStateJson(-1);
    char *stamped = satori::KeepaliveStateJson(123456);
    Check(plain && !strstr(plain, "changed_at"), "state json: no timestamp when asked for none");
    Check(stamped && strstr(stamped, "\"changed_at\":123456") != nullptr, "state json: the timestamp when given");
    free(plain);
    free(stamped);

    satori::g_client_count = 0;
    satori::g_server_ready = false;
    satori::g_login_count = 0;
    unlink(state);
    unlink(tmp);
    char sub[330];
    snprintf(sub, sizeof(sub), "%s/files/satori-wx", dir);
    rmdir(sub);
    rmdir(files);
    rmdir(dir);
}
} // namespace

int main() {
    TestRender();
    TestStatusShape();
    TestHooks();
    TestKeeper();
    if (failures) {
        fprintf(stderr, "keepalive tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("keepalive tests: PASS\n");
    return 0;
}
