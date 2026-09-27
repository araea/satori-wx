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
