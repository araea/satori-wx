// Host-side tests for the resident-keeper status block and the automatic wake-lock hold.
// There is no JVM here: the keeper only touches Java once a VM has been handed to it, so the
// ref-counting and the reported shape can be checked without a device.
#include "protocol.h"
#include "wx_keepalive.h"
#include "vendor/cjson/cJSON.h"
#include <stdio.h>
#include <string.h>

namespace {
int failures = 0;

void Check(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

const cJSON *Field(const cJSON *keep, const char *name) {
    return cJSON_GetObjectItemCaseSensitive(keep, name);
}

void CheckBool(const cJSON *keep, const char *name, bool expected) {
    const cJSON *field = Field(keep, name);
    Check(field && cJSON_IsBool(field) && (cJSON_IsTrue(field) == expected), name);
}

void CheckNumber(const cJSON *keep, const char *name, double expected) {
    const cJSON *field = Field(keep, name);
    Check(field && cJSON_IsNumber(field) && field->valuedouble == expected, name);
}
} // namespace

int main() {
    // Defaults: nothing posted, no holds, no clients, no login.
    cJSON *status = cJSON_CreateObject();
    satori::KeepaliveStatus(status);
    const cJSON *keep = Field(status, "keepalive");
    Check(keep && cJSON_IsObject(keep), "keepalive block exists");
    CheckBool(keep, "notification", false);
    CheckBool(keep, "notifications_enabled", false);
    CheckBool(keep, "channel", false);
    CheckBool(keep, "wakelock", false);
    CheckBool(keep, "wakelock_held", false);
    CheckBool(keep, "cpu_held", false);
    CheckBool(keep, "wifi_held", false);
    CheckBool(keep, "sustain_wifi", false);
    CheckNumber(keep, "auto", 0);
    CheckNumber(keep, "clients", 0);
    CheckNumber(keep, "uptime_ms", 0);
    Check(Field(keep, "service") && Field(keep, "notify") && Field(keep, "wchan") &&
          Field(keep, "oom_score_adj"), "diagnostic fields present");
    cJSON_Delete(status);

    // The connection count is what the notification and the Wi-Fi sustain read.
    satori::g_client_count = 2;
    satori::g_server_ready = true;
    satori::g_login_count = 1;
    status = cJSON_CreateObject();
    satori::KeepaliveStatus(status);
    keep = Field(status, "keepalive");
    CheckNumber(keep, "clients", 2);
    CheckNumber(keep, "uptime_ms", 0); // uptime starts on the keeper tick, not here
    cJSON_Delete(status);

    // What the resident notification says. The collapsed row is the title plus one line; the
    // expanded body repeats that line and adds the uptime, but never the title a second time, and
    // says nothing about the wake lock (the button's own label does) or about sending (always on).
    {
        char title[96], text[160], big[320];
        satori::g_login_count = 1; satori::g_server_ready = true; satori::g_client_count = 0;
        satori::KeepaliveRender(0, title, sizeof(title), text, sizeof(text), big, sizeof(big));
        Check(!strcmp(title, "知言 · 运行中"), "notification: title while serving");
        Check(strstr(text, "等待客户端连接") != nullptr, "notification: waiting for a client");
        Check(!strstr(big, "知言"), "notification: the body does not repeat the title");
        Check(!strcmp(big, text), "notification: with no uptime the body is just the line");

        satori::g_client_count = 3;
        satori::KeepaliveRender((2 * 3600 + 5 * 60) * 1000LL + 999, title, sizeof(title), text, sizeof(text), big, sizeof(big));
        Check(strstr(text, "已连接 3 个客户端") != nullptr, "notification: client count in the line");
        Check(!strstr(big, "知言") && !strstr(big, "运行中"), "notification: body has neither the app name nor the state again");
        Check(strstr(big, text) == big && strstr(big, "\n已在线 2 小时 5 分") != nullptr, "notification: body is the line, then the uptime");
        Check(!strstr(big, "唤醒锁") && !strstr(big, "CPU") && !strstr(big, "Wi-Fi"), "notification: no wake-lock detail");
        Check(!strstr(big, "发送") && !strstr(text, "发送"), "notification: no send-switch line");

        satori::KeepaliveRender(30 * 1000LL, title, sizeof(title), text, sizeof(text), big, sizeof(big));
        Check(strstr(big, "\n刚刚上线") != nullptr, "notification: under a minute reads as just online");
        satori::KeepaliveRender(59 * 60 * 1000LL, title, sizeof(title), text, sizeof(text), big, sizeof(big));
        Check(strstr(big, "\n已在线 59 分") != nullptr, "notification: minutes");
        satori::KeepaliveRender(49 * 3600 * 1000LL, title, sizeof(title), text, sizeof(text), big, sizeof(big));
        Check(strstr(big, "\n已在线 2 天 1 小时") != nullptr, "notification: days");

        satori::g_login_count = 0;
        satori::KeepaliveRender(0, title, sizeof(title), text, sizeof(text), big, sizeof(big));
        Check(!strcmp(title, "知言 · 等待登录") && !strstr(big, "\n"), "notification: waiting for login has no uptime line");
        satori::g_login_count = 1; satori::g_server_ready = false;
        satori::KeepaliveRender(0, title, sizeof(title), text, sizeof(text), big, sizeof(big));
        Check(!strcmp(title, "知言 · 服务异常") && strstr(text, "未监听") != nullptr, "notification: port not listening");
        satori::g_server_ready = true; satori::g_client_count = 2;
    }

    // The automatic hold is ref-counted and symmetric even with no JVM attached.
    satori::KeepaliveWakelockBegin();
    satori::KeepaliveWakelockBegin();
    status = cJSON_CreateObject();
    satori::KeepaliveStatus(status);
    CheckNumber(Field(status, "keepalive"), "auto", 2);
    cJSON_Delete(status);
    satori::KeepaliveWakelockEnd();
    status = cJSON_CreateObject();
    satori::KeepaliveStatus(status);
    CheckNumber(Field(status, "keepalive"), "auto", 1);
    cJSON_Delete(status);
    satori::KeepaliveWakelockEnd();
    satori::KeepaliveWakelockEnd(); // an unmatched end must not underflow
    status = cJSON_CreateObject();
    satori::KeepaliveStatus(status);
    CheckNumber(Field(status, "keepalive"), "auto", 0);
    cJSON_Delete(status);

    satori::g_client_count = 0;
    satori::g_server_ready = false;
    satori::g_login_count = 0;
    if (failures) { fprintf(stderr, "keepalive tests: %d failure(s)\n", failures); return 1; }
    printf("keepalive tests: PASS\n");
    return 0;
}
