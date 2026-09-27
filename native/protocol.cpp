#include "protocol.h"
#include <errno.h>
#include <math.h>
#include <pthread.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>

namespace satori {
volatile int g_login_count = 0;
bool Utf8(const char *data, size_t size) {
    for (size_t i = 0; i < size;) {
        uint32_t c = static_cast<unsigned char>(data[i++]);
        if (c < 128) continue;
        unsigned n; uint32_t min;
        if (c >= 0xc2 && c <= 0xdf) { n = 1; min = 0x80; c &= 31; }
        else if (c >= 0xe0 && c <= 0xef) { n = 2; min = 0x800; c &= 15; }
        else if (c >= 0xf0 && c <= 0xf4) { n = 3; min = 0x10000; c &= 7; }
        else return false;
        if (size - i < n) return false;
        while (n--) {
            const unsigned char next = data[i++];
            if ((next & 0xc0) != 0x80) return false;
            c = (c << 6) | (next & 63);
        }
        if (c < min || c > 0x10ffff || (c >= 0xd800 && c <= 0xdfff)) return false;
    }
    return true;
}
bool UniqueKeys(const cJSON *node) {
    for (const cJSON *child = node->child; child; child = child->next) {
        if (child->string) {
            for (const cJSON *p = node->child; p != child; p = p->next)
                if (p->string && strcmp(child->string, p->string) == 0) return false;
        }
        if (!UniqueKeys(child)) return false;
    }
    return true;
}
// cJSON intentionally accepts some non-JSON number spellings/control characters.
// Validate lexical rules first; cJSON still owns structural/Unicode-escape parsing.
bool JsonLexical(const char *data, size_t size) {
    for (size_t i = 0; i < size; ++i) {
        unsigned char ch = data[i];
        if (ch < 32 && ch != ' ' && ch != '\r' && ch != '\n' && ch != '\t') return false;
        if (ch == '"') {
            bool closed = false;
            while (++i < size) {
                ch = data[i];
                if (ch < 32) return false;
                if (ch == '"') { closed = true; break; }
                if (ch == '\\') { if (++i >= size) return false; }
            }
            if (!closed) return false;
        } else if (ch == '-' || (ch >= '0' && ch <= '9')) {
            if (ch == '-' && ++i >= size) return false;
            if (data[i] == '0') ++i;
            else {
                if (data[i] < '1' || data[i] > '9') return false;
                while (i < size && data[i] >= '0' && data[i] <= '9') ++i;
            }
            if (i < size && data[i] == '.') {
                ++i;
                const size_t start = i;
                while (i < size && data[i] >= '0' && data[i] <= '9') ++i;
                if (i == start) return false;
            }
            if (i < size && (data[i] == 'e' || data[i] == 'E')) {
                ++i;
                if (i < size && (data[i] == '+' || data[i] == '-')) ++i;
                const size_t start = i;
                while (i < size && data[i] >= '0' && data[i] <= '9') ++i;
                if (i == start) return false;
            }
            if (i < size && !strchr(" ,]}\r\n\t", data[i])) return false;
            --i;
        }
    }
    return true;
}
cJSON *Json(const char *data, size_t size) {
    if (!size || memchr(data, 0, size) || !Utf8(data, size) || !JsonLexical(data, size)) return nullptr;
    // cJSON strings are NUL-terminated; disallow escaped NUL to avoid truncated keys/tokens.
    for (size_t i = 0; i + 5 < size; ++i)
        if (memcmp(data + i, "\\u0000", 6) == 0) return nullptr;
    const char *end = nullptr;
    cJSON *value = cJSON_ParseWithLengthOpts(data, size, &end, false);
    if (!value) return nullptr;
    while (end < data + size && (*end == ' ' || *end == '\r' || *end == '\n' || *end == '\t')) ++end;
    if (end != data + size || !cJSON_IsObject(value) || !UniqueKeys(value)) {
        cJSON_Delete(value); return nullptr;
    }
    return value;
}
// Field grammar: name:type, '?' means optional; s string, o object, b bool, n nonnegative integer.
const Method kMethods[] = {
    {"channel.get", "channel_id:s guild_id:?s", false},
    {"channel.list", "guild_id:s next:?s", false},
    {"channel.create", "guild_id:s data:o", false},
    {"channel.update", "channel_id:s data:o", false},
    {"channel.delete", "channel_id:s", false},
    {"channel.mute", "channel_id:s guild_id:?s enable:?b", false},
    {"message.create", "channel_id:s content:s referrer:?o", false},
    {"message.update", "channel_id:s message_id:s content:s", false},
    {"message.delete", "channel_id:s message_id:s", false},
    {"message.get", "channel_id:s message_id:s", false},
    {"message.list", "channel_id:s next:?s direction:?s limit:?n order:?s", false},
    {"reaction.create", "channel_id:s message_id:s emoji_id:s", false},
    {"reaction.delete", "channel_id:s message_id:s emoji_id:s user_id:?s", false},
    {"reaction.clear", "channel_id:s message_id:s emoji_id:?s", false},
    {"reaction.list", "channel_id:s message_id:s emoji_id:s next:?s", false},
    {"upload.create", "", true},
    {"guild.get", "guild_id:s", false},
    {"guild.list", "next:?s", false},
    {"guild.member.get", "guild_id:s user_id:s", false},
    {"guild.member.list", "guild_id:s next:?s", false},
    {"guild.member.kick", "guild_id:s user_id:s permanent:?b", false},
    {"guild.member.mute", "guild_id:s user_id:s duration:n reason:?s", false},
    {"guild.member.role.set", "guild_id:s user_id:s role_id:s", false},
    {"guild.member.role.unset", "guild_id:s user_id:s role_id:s", false},
    {"guild.member.role.list", "guild_id:s user_id:s next:?s", false},
    {"guild.role.list", "guild_id:s next:?s", false},
    {"guild.role.create", "guild_id:s data:o", false},
    {"guild.role.update", "guild_id:s role_id:s data:o", false},
    {"guild.role.delete", "guild_id:s role_id:s", false},
    {"login.get", "", false},
    {"user.get", "user_id:s", false},
    {"user.channel.create", "user_id:s guild_id:?s", false},
    {"friend.list", "next:?s", false},
    {"friend.delete", "user_id:s", false},
    {"friend.approve", "message_id:s approve:b comment:?s", false},
    {"guild.approve", "message_id:s approve:b comment:?s", false},
    {"guild.member.approve", "message_id:s approve:b comment:?s", false},
};
const size_t kMethodCount = sizeof(kMethods) / sizeof(kMethods[0]);
const Method *FindMethod(const char *name) {
    for (const auto &method : kMethods) if (!strcmp(name, method.name)) return &method;
    return nullptr;
}
static const cJSON *Item(const cJSON *obj, const char *name) { return cJSON_GetObjectItemCaseSensitive(obj, name); }
static bool Integer(const cJSON *obj) {
    return cJSON_IsNumber(obj) && isfinite(obj->valuedouble) && obj->valuedouble >= 0 &&
           obj->valuedouble <= 9007199254740991.0 && floor(obj->valuedouble) == obj->valuedouble;
}
bool ValidateParams(const Method &method, const cJSON *body) {
    if (!cJSON_IsObject(body)) return false;
    for (const char *p = method.fields; *p;) {
        char name[64]; size_t i = 0;
        while (*p && *p != ':' && i + 1 < sizeof(name)) name[i++] = *p++;
        name[i] = 0;
        if (*p++ != ':') return false;
        bool optional = *p == '?'; if (optional) ++p;
        const char type = *p++;
        if (*p == ' ') ++p;
        const cJSON *value = Item(body, name);
        if (!value || cJSON_IsNull(value)) { if (optional) continue; return false; }
        if ((type == 's' && !cJSON_IsString(value)) || (type == 'o' && !cJSON_IsObject(value)) ||
            (type == 'b' && !cJSON_IsBool(value)) || (type == 'n' && !Integer(value))) return false;
        if (!strcmp(name, "direction") && strcmp(value->valuestring, "before") &&
            strcmp(value->valuestring, "after") && strcmp(value->valuestring, "around")) return false;
        if (!strcmp(name, "order") && strcmp(value->valuestring, "asc") && strcmp(value->valuestring, "desc")) return false;
        if (!strcmp(name, "limit") && value->valuedouble == 0) return false;
    }
    return true;
}
bool EscapeText(const char *text, char *out, size_t capacity) {
    if (!capacity) return false;
    size_t used = 0;
    for (; *text; ++text) {
        const char *replacement = *text == '&' ? "&amp;" : *text == '<' ? "&lt;" : *text == '>' ? "&gt;" : nullptr;
        const size_t n = replacement ? strlen(replacement) : 1;
        if (n >= capacity - used) return false;
        memcpy(out + used, replacement ? replacement : text, n); used += n;
    }
    if (!capacity) return false;
    out[used] = 0; return true;
}

size_t PlainText(const char *content, char *out, size_t capacity) {
    if (!out || capacity == 0) return 0;
    size_t used = 0;
    // Every element in a Satori content string is self-closing or wraps text. Quote,
    // at, emoji and media only carry ids, so writing them out would leak numbers into
    // the chat; <br/> is the one tag that means something to a text-only client.
    auto keep = [&](char c) { if (used + 1 < capacity) out[used++] = c; };
    for (const char *p = content; p && *p;) {
        if (*p == '<') {
            const char *q = p + 1;
            char quote = 0;
            for (; *q; ++q) {
                if (quote) { if (*q == quote) quote = 0; continue; }
                if (*q == '"' || *q == '\'') { quote = *q; continue; }
                if (*q == '>') break;
            }
            if (!*q) break;  // Unterminated tag: nothing trustworthy follows.
            const char *name = p + 1;
            const bool closing = *name == '/';
            if (closing) ++name;
            const char *end = name;
            while (end < q && ((*end >= 'a' && *end <= 'z') || (*end >= 'A' && *end <= 'Z') ||
                               (*end >= '0' && *end <= '9') || *end == '-' || *end == '_' || *end == ':')) ++end;
            if (!closing && end - name == 2 &&
                (name[0] == 'b' || name[0] == 'B') && (name[1] == 'r' || name[1] == 'R')) keep('\n');
            p = q + 1;
            continue;
        }
        if (*p == '&') {
            static const struct { const char *name; char value; } entities[] = {
                {"&lt;", '<'}, {"&gt;", '>'}, {"&quot;", '"'},
                {"&#39;", '\''}, {"&apos;", '\''}, {"&amp;", '&'},
            };
            bool matched = false;
            for (const auto &entity : entities) {
                const size_t n = strlen(entity.name);
                if (!strncmp(p, entity.name, n)) { keep(entity.value); p += n; matched = true; break; }
            }
            if (matched) continue;
        }
        keep(*p++);
    }
    out[used] = 0;
    return used;
}
struct EventBus {
    pthread_mutex_t mutex;
    int fd;
    unsigned head, count;
    struct { bool meta; char json[kEventSize]; } entries[32];
};
EventBus *CreateBus() {
    auto *bus = static_cast<EventBus *>(calloc(1, sizeof(EventBus)));
    if (!bus) return nullptr;
    bus->fd = eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    if (bus->fd < 0) { free(bus); return nullptr; }
    if (pthread_mutex_init(&bus->mutex, nullptr)) { close(bus->fd); free(bus); return nullptr; }
    return bus;
}
void DestroyBus(EventBus *bus) {
    if (!bus) return;
    close(bus->fd); pthread_mutex_destroy(&bus->mutex); free(bus);
}
int BusFd(EventBus *bus) { return bus ? bus->fd : -1; }
bool Publish(EventBus *bus, const char *json, bool meta) {
    if (!bus || !json || strlen(json) >= kEventSize) return false;
    pthread_mutex_lock(&bus->mutex);
    if (bus->count == 32) { pthread_mutex_unlock(&bus->mutex); return false; }
    auto &entry = bus->entries[(bus->head + bus->count++) % 32];
    entry.meta = meta; strcpy(entry.json, json);
    pthread_mutex_unlock(&bus->mutex);
    uint64_t one = 1;
    while (write(bus->fd, &one, sizeof(one)) < 0 && errno == EINTR) {}
    return true;
}
void DrainWake(EventBus *bus) {
    if (!bus) return;
    uint64_t value;
    while (read(bus->fd, &value, sizeof(value)) < 0 && errno == EINTR) {}
}
bool Take(EventBus *bus, char out[kEventSize], bool *meta) {
    if (!bus) return false;
    pthread_mutex_lock(&bus->mutex);
    const bool found = bus->count != 0;
    if (found) {
        const auto &entry = bus->entries[bus->head];
        strcpy(out, entry.json); *meta = entry.meta;
        bus->head = (bus->head + 1) % 32; --bus->count;
    }
    pthread_mutex_unlock(&bus->mutex);
    return found;
}
struct Hub {
    cJSON *meta;
    uint64_t epoch, latest, floor, lost_through;
    unsigned head, count;
    struct { uint64_t sn; bool login_event; char json[kEventSize]; } history[kHistory];
};
Hub *CreateHub() {
    auto *hub = static_cast<Hub *>(calloc(1, sizeof(Hub)));
    if (!hub) return nullptr;
    hub->meta = cJSON_Parse("{\"logins\":[],\"proxy_urls\":[]}");
    if (!hub->meta) { free(hub); return nullptr; }
    timespec ts{}; clock_gettime(CLOCK_REALTIME, &ts);
    // Disjoint practical sequence ranges across process restarts; still a JS-safe integer.
    hub->epoch = static_cast<uint64_t>(ts.tv_sec) * 1000000 + ts.tv_nsec / 1000;
    hub->latest = hub->floor = hub->lost_through = hub->epoch;
    return hub;
}
void DestroyHub(Hub *hub) { if (hub) { cJSON_Delete(hub->meta); free(hub); } }
const cJSON *Meta(Hub *hub) { return hub->meta; }
const cJSON *FindLogin(Hub *hub, const char *platform, const char *user) {
    for (const cJSON *p = Item(hub->meta, "logins")->child; p; p = p->next) {
        const cJSON *pf = Item(p, "platform"), *id = Item(Item(p, "user"), "id");
        if (cJSON_IsString(pf) && cJSON_IsString(id) && !strcmp(pf->valuestring, platform) && !strcmp(id->valuestring, user)) return p;
    }
    return nullptr;
}
uint64_t Latest(Hub *hub) { return hub->latest; }
bool CanResume(Hub *hub, uint64_t sn) { return sn == 0 ? hub->floor == hub->epoch : sn >= hub->floor && sn <= hub->latest; }
bool CanDeliver(Hub *hub, uint64_t cursor, uint64_t replay_until) {
    return CanResume(hub, cursor) && (cursor >= hub->lost_through || replay_until >= hub->lost_through);
}
const char *NextEvent(Hub *hub, uint64_t *cursor, uint64_t replay_until) {
    for (unsigned i = 0; i < hub->count; ++i) {
        const auto &event = hub->history[(hub->head + i) % kHistory];
        if (event.sn > *cursor) {
            *cursor = event.sn;
            if (!event.login_event || event.sn > replay_until) return event.json;
        }
    }
    *cursor = hub->latest; return nullptr;
}
char *Envelope(int op, const cJSON *body) {
    cJSON *root = cJSON_CreateObject();
    cJSON *copy = cJSON_Duplicate(body, true);
    if (!root || !copy) { cJSON_Delete(root); cJSON_Delete(copy); return nullptr; }
    if (!cJSON_AddNumberToObject(root, "op", op) || !cJSON_AddItemToObject(root, "body", copy)) {
        cJSON_Delete(copy); cJSON_Delete(root); return nullptr;
    }
    char *text = cJSON_PrintUnformatted(root); cJSON_Delete(root); return text;
}
char *EnvelopeBody(const char *signal) {
    if (!signal) return nullptr;
    cJSON *root = cJSON_Parse(signal);
    if (!root) return nullptr;
    const cJSON *body = Item(root, "body");
    char *text = body ? cJSON_PrintUnformatted(body) : nullptr;
    cJSON_Delete(root);
    return text;
}
static bool StringArray(const cJSON *value) {
    if (!cJSON_IsArray(value)) return false;
    for (const cJSON *p = value->child; p; p = p->next) if (!cJSON_IsString(p)) return false;
    return true;
}
char *Apply(Hub *hub, const char *json, bool meta) {
    cJSON *body = Json(json, strlen(json));
    if (!body) return nullptr;
    char *signal = nullptr;
    if (meta) {
        const cJSON *urls = Item(body, "proxy_urls");
        // Only resource proxies actually served by this adapter may be advertised.
        if (StringArray(urls) && !urls->child) {
            cJSON_DeleteItemFromObjectCaseSensitive(body, "logins");
            signal = Envelope(5, body);
            if (signal) cJSON_ReplaceItemInObjectCaseSensitive(hub->meta, "proxy_urls", cJSON_Duplicate(urls, true));
        }
        cJSON_Delete(body); return signal;
    }
    const cJSON *type = Item(body, "type");
    const cJSON *login = Item(body, "login");
    const cJSON *sn = Item(login, "sn");
    if (!cJSON_IsString(type) || !*type->valuestring || !Integer(sn)) { cJSON_Delete(body); return nullptr; }
    const bool added = !strcmp(type->valuestring, "login-added");
    const bool updated = !strcmp(type->valuestring, "login-updated");
    const bool removed = !strcmp(type->valuestring, "login-removed");
    const bool login_event = added || updated || removed;
    cJSON *logins = cJSON_GetObjectItemCaseSensitive(hub->meta, "logins");
    int index = -1, i = 0;
    for (cJSON *p = logins->child; p; p = p->next, ++i)
        if (Item(p, "sn")->valuedouble == sn->valuedouble) { index = i; break; }
    const cJSON *known = index < 0 ? nullptr : cJSON_GetArrayItem(logins, index);
    const cJSON *status = Item(login, "status");
    const cJSON *platform = Item(login, "platform");
    const cJSON *user = Item(login, "user");
    if (login_event) {
        if (!Integer(status) || status->valuedouble > 4 || !cJSON_IsString(Item(login, "adapter")) ||
            (Item(login, "features") && !StringArray(Item(login, "features"))) ||
            (status->valuedouble == 1 && (!cJSON_IsString(platform) || !cJSON_IsString(Item(user, "id")))) ||
            (added ? index >= 0 : index < 0) || (added && cJSON_GetArraySize(logins) >= 16)) {
            cJSON_Delete(body); return nullptr;
        }
    } else {
        if (!known || Item(known, "status")->valuedouble != 1) { cJSON_Delete(body); return nullptr; }
        // Non-login events always carry the current identity, with precisely these three fields.
        cJSON *identity = cJSON_CreateObject();
        if (!identity || !cJSON_AddItemToObject(identity, "sn", cJSON_Duplicate(Item(known, "sn"), true)) ||
            !cJSON_AddItemToObject(identity, "platform", cJSON_Duplicate(Item(known, "platform"), true)) ||
            !cJSON_AddItemToObject(identity, "user", cJSON_Duplicate(Item(known, "user"), true))) {
            cJSON_Delete(identity); cJSON_Delete(body); return nullptr;
        }
        cJSON_ReplaceItemInObjectCaseSensitive(body, "login", identity);
    }
    if (hub->latest >= 9007199254740991ULL) { cJSON_Delete(body); return nullptr; }
    cJSON_DeleteItemFromObjectCaseSensitive(body, "sn");
    if (!cJSON_AddNumberToObject(body, "sn", static_cast<double>(hub->latest + 1))) { cJSON_Delete(body); return nullptr; }
    if (!Item(body, "timestamp")) {
        timespec ts{}; clock_gettime(CLOCK_REALTIME, &ts);
        cJSON_AddNumberToObject(body, "timestamp", static_cast<double>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000);
    }
    if (!Integer(Item(body, "timestamp"))) { cJSON_Delete(body); return nullptr; }
    signal = Envelope(0, body);
    if (!signal || strlen(signal) >= kEventSize) { free(signal); cJSON_Delete(body); return nullptr; }
    if (login_event) {
        // Commit the new snapshot only after allocation and size validation succeed.
        cJSON *next_meta = cJSON_Duplicate(hub->meta, true);
        cJSON *copy = removed ? nullptr : cJSON_Duplicate(login, true);
        if (!next_meta || (!removed && !copy)) {
            cJSON_Delete(next_meta); cJSON_Delete(copy); free(signal); cJSON_Delete(body); return nullptr;
        }
        cJSON *next_logins = cJSON_GetObjectItemCaseSensitive(next_meta, "logins");
        bool ok = true;
        if (removed) cJSON_DeleteItemFromArray(next_logins, index);
        else if (added) ok = cJSON_AddItemToArray(next_logins, copy);
        else ok = cJSON_ReplaceItemInArray(next_logins, index, copy);
        if (!ok) cJSON_Delete(copy);
        char *snapshot = cJSON_PrintUnformatted(next_meta);
        ok = ok && snapshot && strlen(snapshot) < 12000;
        free(snapshot);
        if (!ok) { cJSON_Delete(next_meta); free(signal); cJSON_Delete(body); return nullptr; }
        cJSON_Delete(hub->meta); hub->meta = next_meta;
        int online = 0;
        const cJSON *logins_now = Item(next_meta, "logins");
        for (const cJSON *p = logins_now ? logins_now->child : nullptr; p; p = p->next)
            if (Item(p, "status") && Item(p, "status")->valuedouble == 1) ++online;
        g_login_count = online;
    }
    ++hub->latest;
    if (hub->count == kHistory) {
        const auto &old = hub->history[hub->head];
        if (!old.login_event) hub->floor = old.sn;
        hub->lost_through = old.sn;
        hub->head = (hub->head + 1) % kHistory; --hub->count;
    }
    auto &entry = hub->history[(hub->head + hub->count++) % kHistory];
    entry.sn = hub->latest; entry.login_event = login_event; strcpy(entry.json, signal);
    cJSON_Delete(body); return signal;
}
} // namespace satori
