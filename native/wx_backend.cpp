#include "wx_backend.h"
#include "wx_capabilities.h"
#include "wx_keepalive.h"
#include "wx_live.h"
#include "wx_room.h"
#include "wx_send.h"
#include "wx_store.h"
#include "media.h"
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

// ---- message.create ------------------------------------------------------------------------------
// A Satori content string is a sequence of text and <img> elements; WeChat carries either a
// text message or a picture message, never both. So the content is cut at every <img> into the
// sequence of messages it stands for, in the order the author wrote them. Every picture is
// resolved to a local file, and checked to really be a picture, before the first message goes
// out: a bad link fails the whole request instead of leaving half of it sent.
constexpr size_t kSegments = 2 * kImages + 1;
constexpr size_t kImageMax = 8u << 20;             // decoded bytes accepted from a data: URI
constexpr int kImageConfirmMs = 6000;              // how long to wait for WeChat to insert the picture's row

struct Segment {
    bool image = false;
    size_t begin = 0, end = 0;     // text: the slice of the content
    char path[1200] = {};          // image: the local file
};

// Magic bytes of the picture formats WeChat's pipeline takes.
bool LooksLikeImage(const unsigned char *head, size_t size) {
    if (size >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) return true;
    if (size >= 4 && head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') return true;
    if (size >= 4 && !memcmp(head, "GIF8", 4)) return true;
    return size >= 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WEBP", 4);
}

bool FileIsImage(const char *path) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    unsigned char head[16] = {};
    const size_t got = fread(head, 1, sizeof(head), file);
    fclose(file);
    return LooksLikeImage(head, got);
}

// A data:image/...;base64,... source -> a temp file (the same store upload.create uses, so it
// expires by itself). Returns the file's path, or a failure code for the client.
const char *DecodeDataUri(const char *src, char *path, size_t capacity) {
    const char *comma = strchr(src, ',');
    if (!comma) return "media_unresolved";
    char header[128];
    const size_t header_size = static_cast<size_t>(comma - src);
    if (header_size >= sizeof(header)) return "media_unresolved";
    memcpy(header, src, header_size);
    header[header_size] = 0;
    if (!strcasestr(header, ";base64")) return "media_unresolved";
    if (strncasecmp(header, "data:image/", 11)) return "media_unsupported";
    const size_t encoded = strlen(comma + 1);
    if (encoded / 4 * 3 > kImageMax + 3) return "image_too_large";
    auto *bytes = static_cast<unsigned char *>(malloc(encoded / 4 * 3 + 4));
    if (!bytes) return "media_unresolved";
    const long size = Base64Decode(comma + 1, encoded, bytes, encoded / 4 * 3 + 4);
    const char *failure = nullptr;
    if (size <= 0) failure = "media_unresolved";
    else if (static_cast<size_t>(size) > kImageMax) failure = "image_too_large";
    else if (!LooksLikeImage(bytes, static_cast<size_t>(size))) failure = "media_unsupported";
    if (!failure) {
        const char *extension = !strncasecmp(src + 5, "image/png", 9) ? "picture.png" : !strncasecmp(src + 5, "image/gif", 9) ? "picture.gif" :
                                !strncasecmp(src + 5, "image/webp", 10) ? "picture.webp" : "picture.jpg";
        char name[160];
        if (!TempStorePut(extension, "application/octet-stream", reinterpret_cast<const char *>(bytes), static_cast<size_t>(size), name, sizeof(name))) {
            failure = "media_unresolved";
        } else {
            const TempFile *file = TempStoreGet(name);
            if (!file || strlen(file->path) >= capacity) failure = "media_unresolved";
            else memcpy(path, file->path, strlen(file->path) + 1);
        }
    }
    free(bytes);
    return failure;
}

// Resolves one <img src> to a local file. Only sources this adapter can honour are accepted:
// its own upload.create links, and pictures carried inline as data: URIs. WeChat has no public
// URL for a picture and this build has no HTTP client, so a remote URL is refused with a message
// that says what to do instead.
const char *ResolveImage(const char *src, char *path, size_t capacity, const char **detail) {
    *detail = "";
    if (!strncmp(src, "internal:", 9)) {
        if (!TempStoreResolveLink(src, path, capacity)) {
            *detail = "the link is not an upload of this adapter, or it has expired (uploads live for 5 minutes)";
            return "media_unresolved";
        }
    } else if (!strncasecmp(src, "data:", 5)) {
        const char *failure = DecodeDataUri(src, path, capacity);
        if (failure) {
            *detail = !strcmp(failure, "image_too_large") ? "inline pictures are limited to 8 MiB" : "expected data:image/...;base64,...";
            return failure;
        }
    } else {
        *detail = "remote URLs cannot be fetched: upload the file with upload.create, or send it as a data: URI";
        return "media_unresolved";
    }
    if (!FileIsImage(path)) { *detail = "the file is not a JPEG, PNG, GIF or WebP picture"; return "media_unsupported"; }
    return nullptr;
}

bool HasMediaElement(const char *content) {
    static const char *const tags[] = {"<audio", "<video", "<file"};
    for (const char *tag : tags) {
        for (const char *p = content; (p = strcasestr(p, tag)); ++p) {
            const char after = p[strlen(tag)];
            if (after == ' ' || after == '/' || after == '>' || after == '\t' || after == '\n') return true;
        }
    }
    return false;
}

// The picture we just sent, as a Message: the signed link resolves to the copy WeChat stored.
cJSON *SentImageMessage(Store *store, const char *channel_id, long long local_id) {
    char id[32], link[300], content[400];
    snprintf(id, sizeof(id), "%lld", local_id);
    if (!MediaLink(StoreSelfId(store), "image", id, link, sizeof(link))) return nullptr;
    snprintf(content, sizeof(content), "<img src=\"%s\"/>", link);
    cJSON *message = cJSON_CreateObject();
    cJSON *channel = cJSON_CreateObject();
    cJSON *user = cJSON_CreateObject();
    if (!message || !channel || !user) { cJSON_Delete(message); cJSON_Delete(channel); cJSON_Delete(user); return nullptr; }
    cJSON_AddItemToObject(message, "channel", channel);
    cJSON_AddItemToObject(message, "user", user);
    cJSON_AddStringToObject(message, "id", id);
    cJSON_AddStringToObject(message, "content", content);
    cJSON_AddNumberToObject(message, "timestamp", static_cast<double>(time(nullptr)) * 1000);
    cJSON_AddStringToObject(channel, "id", channel_id);
    cJSON_AddNumberToObject(channel, "type", strstr(channel_id, "@chatroom") ? 0 : 1);
    cJSON_AddStringToObject(user, "id", StoreSelfId(store));
    return message;
}

// A failure partway through a multi-message request says how much had already gone out.
Response FailureAfter(const char *code, const char *detail, bool rejected, size_t already_sent) {
    Response response = Failure(code, detail, rejected);
    if (response.body && already_sent) cJSON_AddNumberToObject(response.body, "sent", static_cast<double>(already_sent));
    return response;
}

Response CreateMessages(const Request &request, Store *store) {
    if (!SendEnabled()) return {404, nullptr};
    if (!store) return {503, nullptr};
    const char *channel_id = Text(request, "channel_id");
    const char *content = Text(request, "content");
    if (!*channel_id || !*content) return {400, nullptr};
    const bool group = strstr(channel_id, "@chatroom") != nullptr;

    // Cut the content at its pictures.
    ImageSpan spans[kImages + 1];
    const size_t span_count = ImageSpans(content, spans, kImages + 1);
    if (span_count > kImages) return BadRequest("too_many_images", "at most 4 pictures per message.create");
    Segment segments[kSegments];
    size_t segment_count = 0, cursor = 0;
    for (size_t i = 0; i <= span_count; ++i) {
        const size_t stop = i < span_count ? spans[i].begin : strlen(content);
        if (stop > cursor) { segments[segment_count].begin = cursor; segments[segment_count].end = stop; ++segment_count; }
        if (i < span_count) {
            segments[segment_count].image = true;
            segments[segment_count].begin = spans[i].begin;
            segments[segment_count].end = spans[i].end;
            ++segment_count;
            cursor = spans[i].end;
        }
    }

    // Resolve every picture up front, and flatten every text segment to see which are blank.
    struct Text { char plain[kOutgoingMax + 1]; char ids[2100]; bool blank; };
    auto *texts = static_cast<Text *>(calloc(kSegments, sizeof(Text)));
    if (!texts) return {500, nullptr};
    size_t sendable = 0;
    for (size_t i = 0; i < segment_count; ++i) {
        Segment &segment = segments[i];
        if (segment.image) {
            char *src = static_cast<char *>(malloc(strlen(content) + 1));
            if (!src) { free(texts); return {500, nullptr}; }
            src[0] = 0;
            const bool has_src = TagAttribute(content, ImageSpan{segment.begin, segment.end}, "src", src, strlen(content) + 1) && *src;
            const char *detail = "";
            const char *failure = has_src ? ResolveImage(src, segment.path, sizeof(segment.path), &detail) : "media_unresolved";
            if (!has_src) detail = "<img> without src";
            free(src);
            if (failure) { free(texts); return BadRequest(failure, detail); }
            ++sendable;
            continue;
        }
        const size_t length = segment.end - segment.begin;
        if (length > kOutgoingMax) { free(texts); return BadRequest("content_too_long", "a text run is limited to 4000 bytes"); }
        char slice[kOutgoingMax + 1];
        memcpy(slice, content + segment.begin, length);
        slice[length] = 0;
        Text &text = texts[i];
        // In a group, <at> elements become real mentions (the visible "@name" plus WeChat's own
        // mention list); anywhere else a mention has no meaning and is dropped like any element.
        OutgoingMention mentions[16];
        size_t mention_count = 0;
        OutgoingText(slice, text.plain, sizeof(text.plain), group ? mentions : nullptr, 16, &mention_count, group ? MentionName : nullptr,
                     const_cast<char *>(channel_id));
        for (size_t m = 0; m < mention_count; ++m) {
            const size_t used = strlen(text.ids);
            if (used + strlen(mentions[m].id) + 2 >= sizeof(text.ids)) break;
            snprintf(text.ids + used, sizeof(text.ids) - used, "%s%s", used ? "," : "", mentions[m].id);
        }
        char *tail = text.plain + strlen(text.plain);
        while (tail > text.plain && (tail[-1] == '\n' || tail[-1] == '\r' || tail[-1] == ' ' || tail[-1] == '\t')) --tail;
        *tail = 0;
        text.blank = !*text.plain;
        if (!text.blank) ++sendable;
    }
    if (!sendable) {
        free(texts);
        // Nothing to say and nothing to show: tell "only media we cannot carry" from "empty".
        if (HasMediaElement(content)) return BadRequest("media_unsupported", "this adapter can send text and pictures; audio, video and files are not supported");
        return {400, nullptr};
    }

    cJSON *list = cJSON_CreateArray();
    if (!list) { free(texts); return {500, nullptr}; }
    size_t sent_count = 0;
    for (size_t i = 0; i < segment_count; ++i) {
        cJSON *message = nullptr;
        if (segments[i].image) {
            const long long before = StoreWatermark(store);
            SendResult sent = SendImage(channel_id, StoreSelfId(store), segments[i].path);
            if (!sent.ok) { cJSON_Delete(list); free(texts); return FailureAfter("send_failed", sent.detail, sent.rejected, sent_count); }
            // The pipeline is asynchronous: only a row in WeChat's own table proves it started.
            long long local_id = 0;
            bool confirmed = false;
            for (int waited = 0; waited < kImageConfirmMs && !confirmed; waited += 40) {
                confirmed = StoreFindSentImage(store, channel_id, before, &local_id);
                if (!confirmed) { const timespec pause{0, 40 * 1000 * 1000}; nanosleep(&pause, nullptr); }
            }
            if (!confirmed) {
                cJSON_Delete(list); free(texts);
                return FailureAfter("image_unconfirmed", "WeChat did not record the picture within 6 seconds; it may still be sent", false, sent_count);
            }
            message = SentImageMessage(store, channel_id, local_id);
        } else if (!texts[i].blank) {
            SendResult sent = SendText(channel_id, texts[i].plain, texts[i].ids);
            if (!sent.ok) { cJSON_Delete(list); free(texts); return FailureAfter("send_failed", sent.detail, sent.rejected, sent_count); }
            message = SentMessage(store, channel_id, texts[i].plain, sent.local_id);
        } else {
            continue;
        }
        if (!message) { cJSON_Delete(list); free(texts); return {500, nullptr}; }
        cJSON_AddItemToArray(list, message);
        ++sent_count;
    }
    free(texts);
    // Satori's `message.create` returns a Message[]; official clients call `.map()` on it.
    return {200, list};
}

Response Call(void *, const Request &request) {
    const char *name = request.method->name;
    const OutboundHold hold(name && IsOutbound(name));
    Store *store = LiveStore();

    if (!strcmp(name, "message.create")) return CreateMessages(request, store);

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
