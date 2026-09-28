#include "wx_backend.h"
#include "wx_capabilities.h"
#include "wx_keepalive.h"
#include "wx_live.h"
#include "wx_room.h"
#include "wx_send.h"
#include "wx_store.h"
#include "tempstore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

namespace satori {
namespace {
constexpr size_t kOutgoingMax = 4000;
constexpr size_t kImages = 4;

// Holds the CPU (and, through the shared keeper, the Wi-Fi radio) for the duration of one
// outbound mutation, mirroring satori-qq's WakeLockCtl.begin()/end(): WeChat's kernel can
// upload while a scene runs, and a screen-off CPU/radio makes that transfer fail. The hold is
// ref-counted and expires on its own, so a wedged send cannot keep the device awake.
struct OutboundHold {
    explicit OutboundHold(bool on) : on_(on) { if (on_) KeepaliveWakelockBegin(); }
    ~OutboundHold() { if (on_) KeepaliveWakelockEnd(); }
    bool on_;
};

bool IsOutbound(const char *name) {
    return !strcmp(name, "message.create") || !strcmp(name, "message.delete") ||
           !strcmp(name, "channel.delete") || !strcmp(name, "guild.member.kick") ||
           !strcmp(name, "guild.member.role.set") || !strcmp(name, "guild.member.role.unset");
}

const char *Text(const Request &request, const char *key) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(request.params, key);
    return cJSON_IsString(value) ? value->valuestring : "";
}

int Limit(const Request &request, int fallback) {
    const cJSON *value = cJSON_GetObjectItemCaseSensitive(request.params, "limit");
    return cJSON_IsNumber(value) ? static_cast<int>(value->valuedouble) : fallback;
}

Response Read(cJSON *object) {
    if (!object) return {404, nullptr};
    return {200, object};
}

// Builds the Satori Message for a message we just queued. It is not read back from the
// database (WeChat may commit concurrently); the id is the local message id the app's
// NetSceneSendMsg constructor returned, and delivery is asynchronous.
cJSON *SentMessage(Store *store, const char *channel_id, const char *content, long long local_id) {
    char escaped[kOutgoingMax + 64];
    if (!EscapeText(content, escaped, sizeof(escaped))) snprintf(escaped, sizeof(escaped), "%s", content);
    cJSON *message = cJSON_CreateObject();
    cJSON *channel = cJSON_CreateObject();
    cJSON *user = cJSON_CreateObject();
    if (!message || !channel || !user) { cJSON_Delete(message); cJSON_Delete(channel); cJSON_Delete(user); return nullptr; }
    cJSON_AddItemToObject(message, "channel", channel);
    cJSON_AddItemToObject(message, "user", user);
    char id[32];
    snprintf(id, sizeof(id), "%lld", local_id);
    cJSON_AddStringToObject(message, "id", id);
    cJSON_AddStringToObject(message, "content", escaped);
    cJSON_AddNumberToObject(message, "timestamp", static_cast<double>(time(nullptr)));
    cJSON_AddNumberToObject(message, "created_at", static_cast<double>(time(nullptr)));
    cJSON_AddStringToObject(channel, "id", channel_id);
    cJSON_AddNumberToObject(channel, "type", strstr(channel_id, "@chatroom") ? 0 : 1);
    cJSON_AddStringToObject(user, "id", StoreSelfId(store));
    return message;
}

Response Failure(const char *code, const char *detail, bool rejected) {
    cJSON *body = cJSON_CreateObject();
    if (!body) return {502, nullptr};
    cJSON_AddStringToObject(body, "error", code);
    if (detail && *detail) cJSON_AddStringToObject(body, "detail", detail);
    // Distinguishes "refused by policy" (disabled, pacing) from a send that
    // actually reached WeChat and failed.
    if (rejected) cJSON_AddBoolToObject(body, "rejected", true);
    return {502, body};
}

// A client error on the request body itself (an element a text-only adapter cannot carry).
Response BadRequest(const char *code, const char *detail) {
    cJSON *body = cJSON_CreateObject();
    if (!body) return {400, nullptr};
    cJSON_AddStringToObject(body, "error", code);
    if (detail && *detail) cJSON_AddStringToObject(body, "detail", detail);
    return {400, body};
}

// The name to show for a mentioned member: what the group calls them, else their own name.
bool MentionName(void *context, const char *id, char *name, size_t capacity) {
    Store *store = LiveStore();
    if (!store || !context) return false;
    cJSON *member = StoreGuildMemberGet(store, static_cast<const char *>(context), id);
    if (!member) return false;
    const cJSON *nick = cJSON_GetObjectItemCaseSensitive(member, "nick");
    const cJSON *user = cJSON_GetObjectItemCaseSensitive(member, "user");
    const cJSON *fallback = cJSON_GetObjectItemCaseSensitive(user, "nick");
    if (!cJSON_IsString(fallback) || !*fallback->valuestring) fallback = cJSON_GetObjectItemCaseSensitive(user, "name");
    const char *chosen = cJSON_IsString(nick) && *nick->valuestring ? nick->valuestring
                       : cJSON_IsString(fallback) ? fallback->valuestring : "";
    snprintf(name, capacity, "%s", chosen);
    cJSON_Delete(member);
    return *name != 0;
}

Response Call(void *, const Request &request) {
    const char *name = request.method->name;
    const OutboundHold hold(name && IsOutbound(name));
    Store *store = LiveStore();

    if (!strcmp(name, "message.create")) {
        if (!SendEnabled()) return {404, nullptr};
        const char *channel_id = Text(request, "channel_id");
        const char *content = Text(request, "content");
        if (!*channel_id || !*content) return {400, nullptr};
        if (strlen(content) > kOutgoingMax) return {400, nullptr};
        // WeChat carries plain text only through this adapter. Flatten the Satori content
        // first, or a reply or an @ would reach the chat as literal <quote>/<at> tags.
        // Anything that only carries an id (a photo, a file, a voice note) is not sendable
        // here, so report that instead of sending tags or pretending it went out.
        char plain[kOutgoingMax + 1];
        // In a group, <at> elements become real mentions (the visible "@name" plus WeChat's own
        // mention list); anywhere else a mention has no meaning and is dropped like any element.
        const bool group = strstr(channel_id, "@chatroom") != nullptr;
        OutgoingMention mentions[16];
        size_t mention_count = 0;
        OutgoingText(content, plain, sizeof(plain), group ? mentions : nullptr, 16, &mention_count, group ? MentionName : nullptr,
                     const_cast<char *>(channel_id));
        char mention_ids[2100] = {};
        for (size_t i = 0; i < mention_count; ++i) {
            const size_t used = strlen(mention_ids);
            if (used + strlen(mentions[i].id) + 2 >= sizeof(mention_ids)) break;
            snprintf(mention_ids + used, sizeof(mention_ids) - used, "%s%s", used ? "," : "", mentions[i].id);
        }
        char *tail = plain + strlen(plain);
        while (tail > plain && (tail[-1] == '\n' || tail[-1] == '\r' || tail[-1] == ' ' || tail[-1] == '\t')) --tail;
        *tail = 0;
        if (!*plain) {
            char sources[kImages][kImageSrcMax];
            if (ImageSources(content, sources, kImages))
                return BadRequest("media_unsupported", "this adapter cannot send media elements");
            return {400, nullptr};
        }
        SendResult sent = SendText(channel_id, plain, mention_ids);
        if (!sent.ok) return Failure("send_failed", sent.detail, sent.rejected);
        if (!store) return {503, nullptr};
        cJSON *message = SentMessage(store, channel_id, plain, sent.local_id);
        if (!message) return {500, nullptr};
        cJSON *list = cJSON_CreateArray();
        if (!list) { cJSON_Delete(message); return {500, nullptr}; }
        cJSON_AddItemToArray(list, message);
        // Satori's `message.create` returns a Message[]; official clients call `.map()` on it.
        return {200, list};
    }

    if (!strcmp(name, "message.delete")) {
        if (!SendEnabled()) return {404, nullptr};
        const char *channel_id = Text(request, "channel_id");
        const char *message_id = Text(request, "message_id");
        if (!*channel_id || !*message_id) return {400, nullptr};
        SendResult recalled = SendRecall(channel_id, message_id);
        if (!recalled.ok) return Failure("delete_failed", recalled.detail, recalled.rejected);
        return {200, cJSON_CreateObject()};
    }

    if (!store) return {503, nullptr};

    // Write actions reuse the sender's opt-in switch. Success means WeChat's own scene was
    // accepted for dispatch, not that the group server applied it.
    if (!strcmp(name, "channel.delete")) {
        if (!SendEnabled()) return {404, nullptr};
        const char *channel_id = Text(request, "channel_id");
        if (!*channel_id || !strstr(channel_id, "@chatroom")) return {400, nullptr};
        ActionResult action = RoomRemoveMember(channel_id, StoreSelfId(store));
        if (!action.ok) return Failure("leave_failed", action.detail, action.rejected);
        return {200, cJSON_CreateObject()};
    }
    if (!strcmp(name, "guild.member.kick")) {
        if (!SendEnabled()) return {404, nullptr};
        const char *guild_id = Text(request, "guild_id"), *user_id = Text(request, "user_id");
        if (!*guild_id || !*user_id) return {400, nullptr};
        ActionResult action = RoomRemoveMember(guild_id, user_id);
        if (!action.ok) return Failure("kick_failed", action.detail, action.rejected);
        return {200, cJSON_CreateObject()};
    }
    if (!strcmp(name, "guild.member.role.set") || !strcmp(name, "guild.member.role.unset")) {
        if (!SendEnabled()) return {404, nullptr};
        const char *guild_id = Text(request, "guild_id"), *user_id = Text(request, "user_id");
        const char *role_id = Text(request, "role_id");
        if (!*guild_id || !*user_id || !*role_id) return {400, nullptr};
        // Only WeChat's group-admin bit is mutable through the server; owner/member are fixed.
        if (strcmp(role_id, "admin")) return Failure("role_not_supported", "only the admin role is mutable", false);
        const bool enable = !strcmp(name, "guild.member.role.set");
        ActionResult action = RoomSetAdmin(guild_id, user_id, enable);
        if (!action.ok) return Failure(enable ? "role_set_failed" : "role_unset_failed", action.detail, action.rejected);
        return {200, cJSON_CreateObject()};
    }

    if (!strcmp(name, "message.get"))
        return Read(StoreMessageGet(store, Text(request, "channel_id"), Text(request, "message_id")));
    if (!strcmp(name, "message.list")) {
        cJSON *list = StoreMessageList(store, Text(request, "channel_id"), Text(request, "next"), Text(request, "direction"),
                                       Limit(request, 50), Text(request, "order"));
        return list ? Response{200, list} : Response{400, nullptr};
    }
    if (!strcmp(name, "user.get"))
        return Read(StoreUserGet(store, Text(request, "user_id")));
    if (!strcmp(name, "friend.list")) {
        cJSON *list = StoreFriendList(store, Text(request, "next"), Limit(request, 50));
        return list ? Response{200, list} : Response{400, nullptr};
    }
    if (!strcmp(name, "guild.get"))
        return Read(StoreGuildGet(store, Text(request, "guild_id")));
    if (!strcmp(name, "guild.list")) {
        cJSON *list = StoreGuildList(store, Text(request, "next"), Limit(request, 50));
        return list ? Response{200, list} : Response{400, nullptr};
    }
    if (!strcmp(name, "channel.get"))
        return Read(StoreChannelGet(store, Text(request, "channel_id")));
    if (!strcmp(name, "channel.list")) {
        cJSON *list = StoreChannelList(store, Text(request, "guild_id"), Text(request, "next"), Limit(request, 50));
        return list ? Response{200, list} : Response{400, nullptr};
    }
    if (!strcmp(name, "guild.member.get"))
        return Read(StoreGuildMemberGet(store, Text(request, "guild_id"), Text(request, "user_id")));
    if (!strcmp(name, "guild.member.list")) {
        cJSON *list = StoreGuildMemberList(store, Text(request, "guild_id"), Text(request, "next"), Limit(request, 50));
        return list ? Response{200, list} : Response{404, nullptr};
    }
    if (!strcmp(name, "guild.role.list")) {
        cJSON *list = StoreGuildRoleList(store, Text(request, "guild_id"));
        return list ? Response{200, list} : Response{404, nullptr};
    }
    if (!strcmp(name, "guild.member.role.list")) {
        cJSON *list = StoreMemberRoleList(store, Text(request, "guild_id"), Text(request, "user_id"));
        return list ? Response{200, list} : Response{404, nullptr};
    }
    if (!strcmp(name, "user.channel.create"))
        return Read(StoreChannelGet(store, Text(request, "user_id")));
    return {501, nullptr};
}
} // namespace

const Backend *WeChatBackend() {
    static const Backend backend{nullptr, Call};
    return &backend;
}
} // namespace satori
