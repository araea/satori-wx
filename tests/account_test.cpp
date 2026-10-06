// Host-side tests for the read-only WeChat account identity source.
// They build fixture SharedPreferences files and never touch a real device.
#include "wx_account.h"
#include "wx_adapter.h"
#include "protocol.h"
#include "vendor/cjson/cJSON.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
int failures = 0;

void Check(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

bool WriteFile(const char *path, const char *data) {
    const int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return false;
    const size_t size = strlen(data);
    const ssize_t written = write(fd, data, size);
    close(fd);
    return written == static_cast<ssize_t>(size);
}

struct Fixture {
    char dir[256];
    explicit Fixture() {
        const char *base = getenv("SATORI_ACCOUNT_TMP");
        if (!base || !*base) base = getenv("TMPDIR");
        if (!base || !*base) base = ".";
        char pattern[512];
        snprintf(pattern, sizeof(pattern), "%s/satori-account-XXXXXX", base);
        const char *made = mkdtemp(pattern);
        if (!made) {
            fprintf(stderr, "mkdtemp failed in %s\n", base);
            exit(2);
        }
        snprintf(dir, sizeof(dir), "%s", made);
        char path[512];
        snprintf(path, sizeof(path), "%s/shared_prefs", dir);
        if (mkdir(path, 0700)) {
            fprintf(stderr, "mkdir failed\n");
            exit(2);
        }
    }
    ~Fixture() {
        char path[512];
        Path(path, sizeof(path), "com.tencent.mm_preferences.xml");
        remove(path);
        Path(path, sizeof(path), "auth_info_key_prefs.xml");
        remove(path);
        snprintf(path, sizeof(path), "%s/files/mmkv/MMKV_Name_LastLoginInfo", dir);
        remove(path);
        snprintf(path, sizeof(path), "%s/files/mmkv/MMKV_Name_LastLoginInfo.crc", dir);
        remove(path);
        snprintf(path, sizeof(path), "%s/files/mmkv", dir);
        rmdir(path);
        snprintf(path, sizeof(path), "%s/files", dir);
        rmdir(path);
        snprintf(path, sizeof(path), "%s/shared_prefs", dir);
        rmdir(path);
        rmdir(dir);
    }
    void Path(char *out, size_t capacity, const char *name) const {
        snprintf(out, capacity, "%s/shared_prefs/%s", dir, name);
    }
    bool Main(const char *body) const {
        char path[512], xml[8192];
        Path(path, sizeof(path), "com.tencent.mm_preferences.xml");
        snprintf(xml, sizeof(xml), "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n%s</map>\n", body);
        return WriteFile(path, xml);
    }
    bool Auth(const char *body) const {
        char path[512], xml[2048];
        Path(path, sizeof(path), "auth_info_key_prefs.xml");
        snprintf(xml, sizeof(xml), "<?xml version='1.0' encoding='utf-8' standalone='yes' ?>\n<map>\n%s</map>\n", body);
        return WriteFile(path, xml);
    }
};

// Builds an MMKV file the way WeChat 8.0.78 writes MMKV_Name_LastLoginInfo: a zero header,
// a size placeholder varint, then [key][string value] records; the valid length goes into the
// .crc meta file at offset 28 (meta version 3+) unless header_size asks for the old layout.
struct Mmkv {
    unsigned char data[4096] = {};
    size_t used = 9; // 4-byte header + 5-byte placeholder varint (0xffffff07-style holder)
    Mmkv() {
        data[4] = 0xff;
        data[5] = 0xff;
        data[6] = 0xff;
        data[7] = 0xff;
        data[8] = 0x07;
    }
    void Varint(uint32_t v) {
        while (v >= 0x80) {
            data[used++] = static_cast<unsigned char>(v | 0x80);
            v >>= 7;
        }
        data[used++] = static_cast<unsigned char>(v);
    }
    Mmkv &Put(const char *key, const char *value) {
        const size_t k = strlen(key), v = strlen(value);
        Varint(static_cast<uint32_t>(k));
        memcpy(data + used, key, k);
        used += k;
        Varint(static_cast<uint32_t>(v + (v < 0x80 ? 1 : 2)));
        Varint(static_cast<uint32_t>(v));
        memcpy(data + used, value, v);
        used += v;
        return *this;
    }
    bool Write(const Fixture &fixture, bool header_size, size_t valid = 0) const {
        char path[512];
        snprintf(path, sizeof(path), "%s/files", fixture.dir);
        mkdir(path, 0700);
        snprintf(path, sizeof(path), "%s/files/mmkv", fixture.dir);
        mkdir(path, 0700);
        const uint32_t size = static_cast<uint32_t>(valid ? valid : used - 4);
        unsigned char copy[4096];
        memcpy(copy, data, sizeof(copy));
        if (header_size) memcpy(copy, &size, 4);
        snprintf(path, sizeof(path), "%s/files/mmkv/MMKV_Name_LastLoginInfo", fixture.dir);
        int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0 || write(fd, copy, sizeof(copy)) != static_cast<ssize_t>(sizeof(copy))) {
            if (fd >= 0) close(fd);
            return false;
        }
        close(fd);
        unsigned char meta[4096] = {};
        const uint32_t version = 5;
        memcpy(meta + 4, &version, 4);
        if (!header_size) memcpy(meta + 28, &size, 4);
        snprintf(path, sizeof(path), "%s/files/mmkv/MMKV_Name_LastLoginInfo.crc", fixture.dir);
        fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
        if (fd < 0 || write(fd, meta, sizeof(meta)) != static_cast<ssize_t>(sizeof(meta))) {
            if (fd >= 0) close(fd);
            return false;
        }
        close(fd);
        return true;
    }
};

const char *String(const cJSON *object, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) ? item->valuestring : "";
}
double Number(const cJSON *object, const char *key) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valuedouble : -1;
}
const char *Nested(const cJSON *object, const char *first, const char *second) {
    const cJSON *child = cJSON_GetObjectItemCaseSensitive(object, first);
    return String(child, second);
}

const char *kMain = "<string name=\"login_weixin_username\">wxid_8zxjsghrk8vz41</string>\n"
                    "<string name=\"last_login_uin\">1114861342</string>\n"
                    "<boolean name=\"isLogin\" value=\"true\" />\n"
                    "<boolean name=\"init_success\" value=\"true\" />\n"
                    "<string name=\"last_login_alias\">nawyjx</string>\n"
                    "<string name=\"last_login_nick_name\">知言</string>\n"
                    "<string name=\"last_login_bind_mobile\">19558338697</string>\n";

void TestOnlineAccount() {
    Fixture fixture;
    Check(fixture.Main(kMain), "write main prefs");
    satori::Account account;
    Check(satori::ReadAccount(fixture.dir, &account), "read account");
    Check(account.exists, "account exists");
    Check(account.online, "account online");
    Check(!strcmp(account.wxid, "wxid_8zxjsghrk8vz41"), "wxid");
    Check(!strcmp(account.uin, "1114861342"), "uin");
    Check(!strcmp(account.alias, "nawyjx"), "alias");
    Check(!strcmp(account.nickname, "知言"), "nickname");
    Check(!strcmp(account.mobile, "19558338697"), "mobile");
    char *json = satori::AccountEvent("login-added", account, 7);
    Check(json != nullptr, "event printed");
    if (json) {
        cJSON *root = cJSON_Parse(json);
        Check(root != nullptr, "event is json");
        if (root) {
            Check(!strcmp(String(root, "type"), "login-added"), "event type");
            const cJSON *login = cJSON_GetObjectItemCaseSensitive(root, "login");
            Check(login != nullptr, "login object");
            Check(Number(login, "sn") == 7, "login sn");
            Check(Number(login, "status") == 1, "status online");
            Check(!strcmp(String(login, "adapter"), "satori-wx"), "adapter name");
            Check(!strcmp(String(login, "platform"), "wechat"), "platform");
            Check(!strcmp(Nested(login, "user", "id"), "wxid_8zxjsghrk8vz41"), "user id");
            Check(!strcmp(Nested(login, "user", "nick"), "知言"), "user nick");
            Check(!strcmp(Nested(login, "user", "name"), "nawyjx"), "user name");
            const cJSON *features = cJSON_GetObjectItemCaseSensitive(login, "features");
            Check(cJSON_IsArray(features) && cJSON_GetArraySize(features) == 22,
                  "features listed (writes are always published)");
            Check(cJSON_IsArray(features) && cJSON_GetArrayItem(features, 0) &&
                      !strcmp(cJSON_GetArrayItem(features, 0)->valuestring, "message.get"),
                  "feature message.get");
            cJSON_Delete(root);
        }
        free(json);
    }
}

void TestOfflineAndRemoval() {
    Fixture fixture;
    Check(fixture.Main("<string name=\"login_weixin_username\">wxid_a</string>\n"
                       "<string name=\"last_login_uin\">42</string>\n"
                       "<boolean name=\"isLogin\" value=\"false\" />\n"),
          "write offline prefs");
    satori::Account account;
    Check(satori::ReadAccount(fixture.dir, &account), "read offline");
    Check(account.exists && !account.online, "offline login still exists");
    char *json = satori::AccountEvent("login-updated", account, 1);
    Check(json && strstr(json, "\"status\":0"), "offline status is 0");
    free(json);
    json = satori::AccountEvent("login-removed", account, 1);
    Check(json && strstr(json, "\"status\":0") && !strstr(json, "\"user\""), "removed event has no user");
    free(json);
}

void TestAuthFallback() {
    Fixture fixture;
    Check(fixture.Main("<string name=\"login_weixin_username\">wxid_b</string>\n"
                       "<boolean name=\"isLogin\" value=\"true\" />\n"),
          "main without uin");
    Check(fixture.Auth("<int name=\"_auth_uin\" value=\"12345\" />\n"), "auth prefs");
    satori::Account account;
    Check(satori::ReadAccount(fixture.dir, &account), "read with fallback");
    Check(!strcmp(account.uin, "12345"), "uin from auth prefs");
    Check(account.exists, "exists via fallback");
}

void TestEntitiesAndInvalidUtf8() {
    Fixture fixture;
    Check(fixture.Main("<string name=\"login_weixin_username\">wxid_c</string>\n"
                       "<string name=\"last_login_uin\">7</string>\n"
                       "<boolean name=\"isLogin\" value=\"true\" />\n"
                       "<string name=\"last_login_nick_name\">A&amp;B&lt;C&gt;D&#x4e2d;&#25991;</string>\n"),
          "entity prefs");
    satori::Account account;
    Check(satori::ReadAccount(fixture.dir, &account), "read entities");
    Check(!strcmp(account.nickname, "A&B<C>D中文"), "entities decoded");
    Check(fixture.Main("<string name=\"login_weixin_username\">wxid_c</string>\n"
                       "<string name=\"last_login_uin\">7</string>\n"
                       "<boolean name=\"isLogin\" value=\"true\" />\n"
                       "<string name=\"last_login_nick_name\">bad\xff"
                       "byte</string>\n"),
          "invalid utf8 prefs");
    Check(satori::ReadAccount(fixture.dir, &account), "read invalid utf8");
    Check(account.nickname[0] == 0, "invalid utf8 field dropped");
    Check(account.exists, "identity kept when a cosmetic field is corrupt");
}

void TestMissingAndMalformed() {
    Fixture fixture;
    satori::Account account;
    Check(!satori::ReadAccount(fixture.dir, &account), "missing prefs reported unreadable");
    Check(fixture.Main(""), "empty map");
    Check(satori::ReadAccount(fixture.dir, &account), "empty map readable");
    Check(!account.exists, "empty map has no account");
    // Truncated XML must not crash and must not invent an account.
    Check(fixture.Main("<string name=\"login_weixin_username\">wxid_d"), "truncated prefs");
    Check(satori::ReadAccount(fixture.dir, &account), "truncated readable");
    Check(!account.exists, "truncated has no complete identity");
    Check(fixture.Main("<string name=\"login_weixin_username\">wxid_d</string>\n"
                       "<string name=\"last_login_uin\">8</string>\n"
                       "<string name=\"long_value\">"),
          "unterminated string");
    Check(satori::ReadAccount(fixture.dir, &account), "unterminated readable");
    Check(!account.exists || account.online == false, "no online claim without isLogin");
}

// Copies the next queued event into `out` (the bus now hands out heap strings).
bool TakeEvent(satori::EventBus *bus, char *out) {
    bool meta = false;
    char *event = satori::Take(bus, &meta);
    if (!event) return false;
    snprintf(out, satori::kEventSize, "%s", event);
    free(event);
    return true;
}

void TestAdapterTransitions() {
    Fixture fixture;
    Check(fixture.Main(kMain), "write main prefs");
    satori::EventBus *bus = satori::CreateBus();
    Check(bus != nullptr, "create bus");
    satori::Adapter *adapter = satori::CreateAdapter(fixture.dir, bus);
    Check(adapter != nullptr, "create adapter");
    static char event[satori::kEventSize];
    Check(satori::AdapterRefresh(adapter), "first refresh");
    Check(TakeEvent(bus, event), "added event published");
    cJSON *root = cJSON_Parse(event);
    Check(root && !strcmp(String(root, "type"), "login-added"), "first event is login-added");
    cJSON_Delete(root);
    Check(!TakeEvent(bus, event), "no duplicate while unchanged");
    Check(satori::AdapterRefresh(adapter), "no-op refresh");
    Check(!TakeEvent(bus, event), "unchanged publishes nothing");
    // Cosmetic change -> update on the same sn.
    Check(fixture.Main("<string name=\"login_weixin_username\">wxid_8zxjsghrk8vz41</string>\n"
                       "<string name=\"last_login_uin\">1114861342</string>\n"
                       "<boolean name=\"isLogin\" value=\"true\" />\n"
                       "<string name=\"last_login_nick_name\">改名</string>\n"),
          "rename");
    Check(satori::AdapterRefresh(adapter), "refresh after rename");
    Check(TakeEvent(bus, event), "update event published");
    root = cJSON_Parse(event);
    Check(root && !strcmp(String(root, "type"), "login-updated"), "second event is login-updated");
    cJSON_Delete(root);
    // Identity change -> remove then add with a new sn.
    Check(fixture.Main("<string name=\"login_weixin_username\">wxid_other</string>\n"
                       "<string name=\"last_login_uin\">99</string>\n"
                       "<boolean name=\"isLogin\" value=\"true\" />\n"),
          "switch account");
    Check(satori::AdapterRefresh(adapter), "refresh after switch");
    Check(TakeEvent(bus, event), "removal published");
    root = cJSON_Parse(event);
    Check(root && !strcmp(String(root, "type"), "login-removed"), "switch removes old identity");
    cJSON_Delete(root);
    Check(satori::AdapterRefresh(adapter), "refresh adds new identity");
    Check(TakeEvent(bus, event), "addition published");
    root = cJSON_Parse(event);
    Check(root && !strcmp(String(root, "type"), "login-added"), "switch adds new identity");
    Check(root && Number(cJSON_GetObjectItemCaseSensitive(root, "login"), "sn") == 2, "new sn allocated");
    cJSON_Delete(root);
    // Logout clears the identity -> removal.
    Check(fixture.Main("<boolean name=\"isLogin\" value=\"false\" />\n"), "logout");
    Check(satori::AdapterRefresh(adapter), "refresh after logout");
    Check(TakeEvent(bus, event), "logout removal published");
    root = cJSON_Parse(event);
    Check(root && !strcmp(String(root, "type"), "login-removed"), "logout removes login");
    cJSON_Delete(root);
    satori::DestroyAdapter(adapter);
    satori::DestroyBus(bus);
}
void TestHubIntegration() {
    Fixture fixture;
    Check(fixture.Main(kMain), "write main prefs");
    satori::Account account;
    Check(satori::ReadAccount(fixture.dir, &account), "read for hub");
    satori::Hub *hub = satori::CreateHub();
    Check(hub != nullptr, "create hub");
    char *added = satori::AccountEvent("login-added", account, 1);
    Check(added != nullptr, "added json");
    char *signal = satori::Apply(hub, added, false);
    Check(signal != nullptr, "apply accepts the adapter's added event");
    free(signal);
    free(added);
    Check(satori::FindLogin(hub, "wechat", "wxid_8zxjsghrk8vz41") != nullptr, "login now in meta");
    account.online = false;
    char *updated = satori::AccountEvent("login-updated", account, 1);
    Check(updated != nullptr, "updated json");
    signal = satori::Apply(hub, updated, false);
    Check(signal != nullptr, "apply accepts the adapter's updated event");
    free(signal);
    free(updated);
    const cJSON *login = satori::FindLogin(hub, "wechat", "wxid_8zxjsghrk8vz41");
    Check(login != nullptr && Number(login, "status") == 0, "offline update reflected in meta");
    char *removed = satori::AccountEvent("login-removed", account, 1);
    Check(removed != nullptr, "removed json");
    signal = satori::Apply(hub, removed, false);
    Check(signal != nullptr, "apply accepts the adapter's removed event");
    free(signal);
    free(removed);
    Check(satori::FindLogin(hub, "wechat", "wxid_8zxjsghrk8vz41") == nullptr, "login removed from meta");
    satori::DestroyHub(hub);
}
} // namespace

// WeChat 8.0.78 rewrote com.tencent.mm_preferences.xml without the login keys; identity now
// lives only in MMKV. This is the exact shape seen on the device on 2026-09-28.
void TestMmkvIdentity() {
    Fixture fixture;
    Check(fixture.Main("<boolean name=\"Main_need_read_top_margin\" value=\"false\" />\n"
                       "<int name=\"heavy_user_session_cnt\" value=\"31\" />\n"),
          "trimmed main prefs");
    Check(fixture.Auth("<int name=\"_auth_uin\" value=\"1114861342\" />\n"), "auth prefs");
    Mmkv mmkv;
    mmkv.Put("last_login_use_voice", "58368")
        .Put("last_login_uin", "1114861342")
        .Put("login_weixin_username", "wxid_8zxjsghrk8vz41")
        .Put("last_login_nick_name", "旧昵称")
        .Put("last_login_alias", "nawyjx")
        .Put("last_login_bind_mobile", "19558338697")
        .Put("last_login_nick_name", "知言"); // appended later: the last record wins
    Check(mmkv.Write(fixture, false), "write mmkv (size in .crc)");
    satori::Account account;
    Check(satori::ReadAccount(fixture.dir, &account), "read mmkv account");
    Check(account.exists && account.online, "mmkv identity exists and is online by auth uin");
    Check(!strcmp(account.wxid, "wxid_8zxjsghrk8vz41"), "mmkv wxid");
    Check(!strcmp(account.uin, "1114861342"), "mmkv uin");
    Check(!strcmp(account.nickname, "知言"), "last mmkv record wins");
    Check(!strcmp(account.alias, "nawyjx") && !strcmp(account.mobile, "19558338697"), "mmkv alias and mobile");

    // Another account authenticated (or the auth record cleared): not online.
    Check(fixture.Auth("<int name=\"_auth_uin\" value=\"999\" />\n"), "auth prefs other uin");
    Check(satori::ReadAccount(fixture.dir, &account) && account.exists && !account.online, "auth mismatch is offline");
    Check(fixture.Auth("<int name=\"_auth_uin\" value=\"0\" />\n"), "auth prefs zero");
    Check(satori::ReadAccount(fixture.dir, &account) && !account.online, "zero auth uin is offline");
}

void TestMmkvBounds() {
    Fixture fixture;
    Check(fixture.Auth("<int name=\"_auth_uin\" value=\"42\" />\n"), "auth prefs");
    Mmkv mmkv;
    mmkv.Put("login_weixin_username", "wxid_valid").Put("last_login_uin", "42");
    const size_t valid = mmkv.used - 4;
    mmkv.Put("login_weixin_username", "wxid_stale_beyond_valid_length");
    Check(mmkv.Write(fixture, false, valid), "write mmkv with stale tail");
    satori::Account account;
    Check(satori::ReadAccount(fixture.dir, &account), "read without main prefs");
    Check(!strcmp(account.wxid, "wxid_valid"), "records past the valid length are ignored");
    Check(account.online, "online via auth uin");

    Mmkv old;
    old.Put("login_weixin_username", "wxid_header").Put("last_login_uin", "42");
    Check(old.Write(fixture, true), "write old-layout mmkv");
    Check(satori::ReadAccount(fixture.dir, &account) && !strcmp(account.wxid, "wxid_header"),
          "size from the 4-byte header");

    Mmkv broken;
    broken.Put("login_weixin_username", "wxid_before_break").Put("last_login_uin", "42");
    broken.data[broken.used++] = 0x7f; // key length 127 that runs past the data
    Check(broken.Write(fixture, false), "write truncated mmkv");
    Check(satori::ReadAccount(fixture.dir, &account) && !strcmp(account.wxid, "wxid_before_break"),
          "a broken record ends the scan and keeps what came before");

    // The legacy XML still wins nothing over MMKV but fills fields MMKV lacks.
    Check(fixture.Main("<string name=\"last_login_nick_name\">来自 XML</string>\n"), "main prefs with nickname only");
    Check(satori::ReadAccount(fixture.dir, &account) && !strcmp(account.nickname, "来自 XML"),
          "xml fills missing fields");
}

int main() {
    TestOnlineAccount();
    TestOfflineAndRemoval();
    TestAuthFallback();
    TestMmkvIdentity();
    TestMmkvBounds();
    TestEntitiesAndInvalidUtf8();
    TestMissingAndMalformed();
    TestAdapterTransitions();
    TestHubIntegration();
    if (failures) {
        fprintf(stderr, "%d account test(s) failed\n", failures);
        return 1;
    }
    printf("account tests: PASS\n");
    return 0;
}
