#include "wx_account.h"
#include "protocol.h"
#include "wx_capabilities.h"
#include "vendor/cjson/cJSON.h"
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

namespace satori {
namespace {
// Android SharedPreferences files are small; anything larger is not one we should read.
constexpr size_t kPrefsMax = 128 * 1024;
constexpr size_t kPrefsEntries = 160;
constexpr size_t kPrefsKey = 64;
constexpr size_t kPrefsValue = 512;

struct Pref {
    char key[kPrefsKey];
    char value[kPrefsValue];
};
struct Prefs {
    Pref entries[kPrefsEntries];
    size_t count;
    const char *Get(const char *key) const {
        for (size_t i = 0; i < count; ++i)
            if (!strcmp(entries[i].key, key)) return entries[i].value;
        return nullptr;
    }
    void Set(const char *key, const char *value) {
        if (count == kPrefsEntries) return;
        const size_t key_size = strlen(key);
        if (!key_size || key_size >= sizeof(entries[0].key)) return;
        if (Get(key)) return; // Android writes one value per key; keep the first.
        Put(key, value);
    }
    // MMKV is an append-only log: a later record for the same key replaces the earlier one.
    void Put(const char *key, const char *value) {
        const size_t key_size = strlen(key);
        if (!key_size || key_size >= sizeof(entries[0].key)) return;
        size_t slot = count;
        for (size_t i = 0; i < count; ++i)
            if (!strcmp(entries[i].key, key)) slot = i;
        if (slot == kPrefsEntries) return;
        strcpy(entries[slot].key, key);
        strncpy(entries[slot].value, value, sizeof(entries[0].value) - 1);
        entries[slot].value[sizeof(entries[0].value) - 1] = 0;
        if (slot == count) ++count;
    }
};

bool ReadFile(const char *path, char *buffer, size_t capacity, size_t *size) {
    const int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    size_t used = 0;
    for (;;) {
        const ssize_t n = read(fd, buffer + used, capacity - used);
        if (n < 0) {
            if (errno == EINTR) continue;
            close(fd);
            return false;
        }
        if (!n) break;
        used += static_cast<size_t>(n);
        if (used == capacity) {
            char extra;
            const ssize_t more = read(fd, &extra, 1);
            if (more != 0) {
                close(fd);
                return false;
            } // oversize or read error
            break;
        }
    }
    close(fd);
    buffer[used] = 0;
    *size = used;
    return true;
}

const char *SkipSpace(const char *p, const char *end) {
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r')) ++p;
    return p;
}

int EncodeUtf8(uint32_t cp, char *out) {
    if (!cp || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff)) return -1;
    if (cp < 0x80) {
        out[0] = static_cast<char>(cp);
        return 1;
    }
    if (cp < 0x800) {
        out[0] = static_cast<char>(0xc0 | (cp >> 6));
        out[1] = static_cast<char>(0x80 | (cp & 63));
        return 2;
    }
    if (cp < 0x10000) {
        out[0] = static_cast<char>(0xe0 | (cp >> 12));
        out[1] = static_cast<char>(0x80 | ((cp >> 6) & 63));
        out[2] = static_cast<char>(0x80 | (cp & 63));
        return 3;
    }
    out[0] = static_cast<char>(0xf0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 63));
    out[2] = static_cast<char>(0x80 | ((cp >> 6) & 63));
    out[3] = static_cast<char>(0x80 | (cp & 63));
    return 4;
}

// XML numeric character references, decimal or hexadecimal.
bool NumericEntity(const char *p, size_t size, uint32_t *out) {
    if (!size || size > 8) return false;
    const bool hex = *p == 'x' || *p == 'X';
    if (hex) {
        ++p;
        --size;
    }
    if (!size || size > 6) return false;
    uint32_t value = 0;
    for (size_t i = 0; i < size; ++i) {
        const char c = p[i];
        int digit;
        if (c >= '0' && c <= '9')
            digit = c - '0';
        else if (hex && c >= 'a' && c <= 'f')
            digit = c - 'a' + 10;
        else if (hex && c >= 'A' && c <= 'F')
            digit = c - 'A' + 10;
        else
            return false;
        value = value * (hex ? 16 : 10) + static_cast<uint32_t>(digit);
        if (value > 0x10ffff) return false;
    }
    *out = value;
    return true;
}

// Decodes the five XML entities Android emits plus numeric references.
// Unknown entities and a bare '&' are kept literally. Rejects invalid UTF-8.
bool DecodeEntities(const char *in, size_t size, char *out, size_t capacity) {
    if (!capacity) return false;
    size_t used = 0;
    for (size_t i = 0; i < size;) {
        const char ch = in[i];
        if (ch != '&') {
            if (used + 1 >= capacity) return false;
            out[used++] = ch;
            ++i;
            continue;
        }
        size_t semi = i + 1;
        while (semi < size && in[semi] != ';' && semi - i <= 12) ++semi;
        if (semi >= size || in[semi] != ';') {
            if (used + 1 >= capacity) return false;
            out[used++] = '&';
            ++i;
            continue;
        }
        const char *entity = in + i + 1;
        const size_t length = semi - i - 1;
        char decoded = 0;
        uint32_t code = 0;
        bool known = true;
        if (length == 3 && !memcmp(entity, "amp", 3))
            decoded = '&';
        else if (length == 2 && !memcmp(entity, "lt", 2))
            decoded = '<';
        else if (length == 2 && !memcmp(entity, "gt", 2))
            decoded = '>';
        else if (length == 4 && !memcmp(entity, "quot", 4))
            decoded = '"';
        else if (length == 4 && !memcmp(entity, "apos", 4))
            decoded = '\'';
        else if (length >= 2 && entity[0] == '#') {
            if (!NumericEntity(entity + 1, length - 1, &code)) known = false;
        } else
            known = false;
        if (!known) {
            if (used + (semi - i) + 1 >= capacity) return false;
            memcpy(out + used, in + i, semi - i + 1);
            used += semi - i + 1;
        } else if (decoded) {
            if (used + 1 >= capacity) return false;
            out[used++] = decoded;
        } else {
            char utf8[4];
            const int n = EncodeUtf8(code, utf8);
            if (n < 0 || used + static_cast<size_t>(n) >= capacity) return false;
            memcpy(out + used, utf8, static_cast<size_t>(n));
            used += static_cast<size_t>(n);
        }
        i = semi + 1;
    }
    out[used] = 0;
    return Utf8(out, used);
}

// Reads name="..." (or name='...') from an open tag body. Android always quotes.
bool Attribute(const char *start, const char *end, const char *name, char *out, size_t capacity) {
    const size_t target = strlen(name);
    for (const char *p = start; end - p >= static_cast<ptrdiff_t>(target + 3); ++p) {
        if (*p != ' ' && *p != '\t' && *p != '\n' && *p != '\r') continue;
        if (memcmp(p + 1, name, target) != 0) continue;
        const char *q = SkipSpace(p + 1 + target, end);
        if (q >= end || *q != '=') continue;
        q = SkipSpace(q + 1, end);
        if (q >= end || (*q != '"' && *q != '\'')) continue;
        const char quote = *q++;
        const char *value = q;
        while (q < end && *q != quote) ++q;
        if (q >= end) return false;
        return DecodeEntities(value, static_cast<size_t>(q - value), out, capacity);
    }
    return false;
}

// Minimal, bounded Android SharedPreferences XML reader. It intentionally ignores
// anything it does not recognise instead of trying to be a general XML parser.
void ParsePrefs(const char *data, size_t size, Prefs *prefs) {
    const char *end = data + size;
    const char *p = data;
    while (p < end) {
        const char *lt = static_cast<const char *>(memchr(p, '<', static_cast<size_t>(end - p)));
        if (!lt) break;
        p = lt + 1;
        if (p >= end) break;
        if (*p == '/' || *p == '?' || *p == '!') {
            const char *gt = static_cast<const char *>(memchr(p, '>', static_cast<size_t>(end - p)));
            if (!gt) break;
            p = gt + 1;
            continue;
        }
        const char *name = p;
        while (p < end && *p != ' ' && *p != '\t' && *p != '\n' && *p != '\r' && *p != '>' && *p != '/') ++p;
        const size_t name_size = static_cast<size_t>(p - name);
        const char *gt = static_cast<const char *>(memchr(p, '>', static_cast<size_t>(end - p)));
        if (!gt) break;
        const bool self_closing = gt > name && gt[-1] == '/';
        char key[kPrefsKey] = {};
        if (Attribute(p, gt, "name", key, sizeof(key))) {
            if (name_size == 6 && !memcmp(name, "string", 6)) {
                char value[kPrefsValue] = {};
                if (!self_closing) {
                    const char *close =
                        static_cast<const char *>(memchr(gt + 1, '<', static_cast<size_t>(end - gt - 1)));
                    if (close && DecodeEntities(gt + 1, static_cast<size_t>(close - gt - 1), value, sizeof(value)))
                        prefs->Set(key, value);
                } else if (Attribute(p, gt, "value", value, sizeof(value))) {
                    prefs->Set(key, value);
                }
            } else if ((name_size == 7 && !memcmp(name, "boolean", 7)) || (name_size == 3 && !memcmp(name, "int", 3)) ||
                       (name_size == 4 && !memcmp(name, "long", 4))) {
                char value[kPrefsValue] = {};
                if (Attribute(p, gt, "value", value, sizeof(value))) prefs->Set(key, value);
            }
        }
        p = gt + 1;
    }
}

bool LoadPrefs(const char *path, Prefs *prefs) {
    prefs->count = 0;
    char *data = static_cast<char *>(malloc(kPrefsMax + 1));
    if (!data) return false;
    size_t size = 0;
    const bool ok = ReadFile(path, data, kPrefsMax, &size);
    if (ok) ParsePrefs(data, size, prefs);
    free(data);
    return ok;
}

// ---- MMKV (Tencent's mmap key-value store) ------------------------------------------
//
// WeChat 8.0.78 keeps the last-login identity in files/mmkv/MMKV_Name_LastLoginInfo and may
// rewrite com.tencent.mm_preferences.xml without it. The file is unencrypted:
//   [uint32 header][varint size placeholder]{[varint key length][key][varint value length][value]}*
// where a string value is itself [varint length][UTF-8]. Records are appended, so the last one
// for a key wins. Since MMKV meta version 3 the valid length lives in the sibling .crc file
// (uint32 at offset 28); older files keep it in the 4-byte header. Read-only, bounded, and any
// record that does not decode cleanly ends the scan instead of guessing.
constexpr size_t kMmkvMax = 256 * 1024;

bool Varint(const unsigned char *&p, const unsigned char *end, uint32_t *out) {
    uint32_t value = 0;
    for (int shift = 0; shift < 35 && p < end; shift += 7) {
        const unsigned char byte = *p++;
        value |= static_cast<uint32_t>(byte & 0x7f) << shift;
        if (!(byte & 0x80)) {
            *out = value;
            return true;
        }
    }
    return false;
}

uint32_t Le32(const unsigned char *p) {
    return p[0] | (p[1] << 8) | (p[2] << 16) | (static_cast<uint32_t>(p[3]) << 24);
}

void ParseMmkv(const unsigned char *data, size_t size, size_t valid, Prefs *prefs) {
    if (size < 4) return;
    const unsigned char *p = data + 4;
    const unsigned char *end = data + 4 + (valid && valid <= size - 4 ? valid : size - 4);
    uint32_t placeholder;
    if (!Varint(p, end, &placeholder)) return;
    while (p < end) {
        uint32_t key_size, value_size;
        if (!Varint(p, end, &key_size) || !key_size || key_size >= kPrefsKey || key_size > static_cast<size_t>(end - p))
            return;
        char key[kPrefsKey];
        memcpy(key, p, key_size);
        key[key_size] = 0;
        p += key_size;
        if (!Varint(p, end, &value_size) || value_size > static_cast<size_t>(end - p)) return;
        const unsigned char *value = p;
        const unsigned char *value_end = p + value_size;
        p = value_end;
        if (!Utf8(key, key_size)) return;
        // Only string records are interesting; numbers WeChat stores here are strings too.
        uint32_t text_size;
        const unsigned char *text = value;
        if (!Varint(text, value_end, &text_size) || text + text_size != value_end || text_size >= kPrefsValue) continue;
        char decoded[kPrefsValue];
        memcpy(decoded, text, text_size);
        decoded[text_size] = 0;
        if (Utf8(decoded, text_size)) prefs->Put(key, decoded);
    }
}

bool LoadMmkv(const char *path, Prefs *prefs) {
    prefs->count = 0;
    char *data = static_cast<char *>(malloc(kMmkvMax + 1));
    if (!data) return false;
    size_t size = 0;
    const bool ok = ReadFile(path, data, kMmkvMax, &size);
    if (ok && size >= 4) {
        size_t valid = Le32(reinterpret_cast<const unsigned char *>(data));
        if (!valid) {
            // The .crc meta file is a whole page; only its first 32 bytes matter here.
            char meta_path[520];
            unsigned char meta[32];
            snprintf(meta_path, sizeof(meta_path), "%s.crc", path);
            const int fd = open(meta_path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            if (fd >= 0) {
                ssize_t n;
                do n = pread(fd, meta, sizeof(meta), 0);
                while (n < 0 && errno == EINTR);
                close(fd);
                if (n == static_cast<ssize_t>(sizeof(meta))) valid = Le32(meta + 28);
            }
        }
        ParseMmkv(reinterpret_cast<const unsigned char *>(data), size, valid, prefs);
    }
    free(data);
    return ok && prefs->count > 0;
}

void CopyField(char *destination, size_t capacity, const char *source) {
    if (!source || !*source || !capacity) return;
    const size_t size = strlen(source);
    const size_t copied = size < capacity - 1 ? size : capacity - 1;
    memcpy(destination, source, copied);
    destination[copied] = 0;
}
} // namespace

bool ReadAccount(const char *data_dir, Account *account) {
    if (!data_dir || !*data_dir || !account) return false;
    *account = Account{};
    static const char kMain[] = "/shared_prefs/com.tencent.mm_preferences.xml";
    static const char kAuth[] = "/shared_prefs/auth_info_key_prefs.xml";
    static const char kMmkv[] = "/files/mmkv/MMKV_Name_LastLoginInfo";
    char path[512];
    const size_t base = strlen(data_dir);
    if (base + sizeof(kMain) >= sizeof(path) || base + sizeof(kAuth) >= sizeof(path) ||
        base + sizeof(kMmkv) + 4 >= sizeof(path))
        return false;
    memcpy(path, data_dir, base);
    Prefs *xml = static_cast<Prefs *>(malloc(sizeof(Prefs)));
    Prefs *mmkv = static_cast<Prefs *>(malloc(sizeof(Prefs)));
    if (!xml || !mmkv) {
        free(xml);
        free(mmkv);
        return false;
    }
    strcpy(path + base, kMain);
    const bool xml_readable = LoadPrefs(path, xml);
    if (!xml_readable) xml->count = 0;
    strcpy(path + base, kMmkv);
    const bool mmkv_readable = LoadMmkv(path, mmkv);
    const bool readable = xml_readable || mmkv_readable;
    if (readable) {
        // The newer MMKV store wins; the XML preferences fill whatever it does not have.
        auto field = [&](const char *key) -> const char * {
            const char *value = mmkv_readable ? mmkv->Get(key) : nullptr;
            return value && *value ? value : xml->Get(key);
        };
        CopyField(account->wxid, sizeof(account->wxid), field("login_weixin_username"));
        CopyField(account->uin, sizeof(account->uin), field("last_login_uin"));
        CopyField(account->alias, sizeof(account->alias), field("last_login_alias"));
        CopyField(account->nickname, sizeof(account->nickname), field("last_login_nick_name"));
        CopyField(account->mobile, sizeof(account->mobile), field("last_login_bind_mobile"));
        if (!account->mobile[0]) CopyField(account->mobile, sizeof(account->mobile), field("login_user_name"));
        // isLogin only speaks for the account the XML itself names.
        const char *xml_wxid = xml->Get("login_weixin_username");
        const char *online = xml->Get("isLogin");
        const bool xml_says = online && xml_wxid && !strcmp(xml_wxid, account->wxid);
        account->online = xml_says && !strcmp(online, "true");
        strcpy(path + base, kAuth);
        const bool auth = LoadPrefs(path, xml);
        const char *auth_uin = auth ? xml->Get("_auth_uin") : nullptr;
        if (!account->uin[0]) CopyField(account->uin, sizeof(account->uin), auth_uin);
        // Without an isLogin flag, WeChat's own authenticated uin matching the identity means
        // the account is signed in (it is cleared or changed on logout / account switch).
        if (!xml_says)
            account->online = auth_uin && account->uin[0] && strcmp(auth_uin, "0") && !strcmp(auth_uin, account->uin);
    }
    free(xml);
    free(mmkv);
    if (!readable) return false;
    account->exists = account->wxid[0] && account->uin[0];
    if (!account->exists) account->online = false;
    return true;
}

bool SameIdentity(const Account &a, const Account &b) { return !strcmp(a.wxid, b.wxid) && !strcmp(a.uin, b.uin); }
bool SameLogin(const Account &a, const Account &b) {
    return SameIdentity(a, b) && a.online == b.online && !strcmp(a.alias, b.alias) && !strcmp(a.nickname, b.nickname) &&
           !strcmp(a.mobile, b.mobile);
}

char *AccountEvent(const char *type, const Account &account, int sn) {
    if (!type) return nullptr;
    const bool removed = !strcmp(type, "login-removed");
    cJSON *root = cJSON_CreateObject();
    if (!root) return nullptr;
    cJSON_AddStringToObject(root, "type", type);
    cJSON *login = cJSON_CreateObject();
    if (!login) {
        cJSON_Delete(root);
        return nullptr;
    }
    cJSON_AddItemToObject(root, "login", login);
    cJSON_AddNumberToObject(login, "sn", sn);
    cJSON_AddNumberToObject(login, "status", removed ? 0 : (account.online ? 1 : 0));
    cJSON_AddStringToObject(login, "adapter", "satori-wx");
    cJSON *features = cJSON_CreateArray();
    if (!features) {
        cJSON_Delete(root);
        return nullptr;
    }
    cJSON_AddItemToObject(login, "features", features);
    // Canonical list lives in wx_capabilities.cpp; it adds message.create when the sender
    // is enabled. wx_backend.cpp serves exactly this set.
    size_t feature_count = 0;
    const char *const *features_list = WeChatFeatures(&feature_count);
    for (size_t i = 0; i < feature_count; ++i) cJSON_AddItemToArray(features, cJSON_CreateString(features_list[i]));
    if (!removed) {
        cJSON_AddStringToObject(login, "platform", "wechat");
        cJSON *user = cJSON_CreateObject();
        if (!user) {
            cJSON_Delete(root);
            return nullptr;
        }
        cJSON_AddItemToObject(login, "user", user);
        // The platform user id is the wxid; the nickname and alias are cosmetic.
        cJSON_AddStringToObject(user, "id", account.wxid);
        if (account.nickname[0]) cJSON_AddStringToObject(user, "nick", account.nickname);
        if (account.alias[0]) cJSON_AddStringToObject(user, "name", account.alias);
    }
    char *text = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    return text;
}
} // namespace satori
