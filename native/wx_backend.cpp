#include "wx_backend.h"
#include "wx_keepalive.h"
#include "wx_live.h"
#include "wx_room.h"
#include "wx_send.h"
#include "wx_voice.h"
#include "wx_store.h"
#include "media.h"
#include "mp4_probe.h"
#include "tempstore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
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
    cJSON_AddNumberToObject(message, "created_at", static_cast<double>(time(nullptr)) * 1000);
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
// A Satori content string is a sequence of text and media elements (<img>, <video>, <audio>,
// <file>); WeChat carries each of those as a message of its own, never mixed with text. So the
// content is cut at every media element into the sequence of messages it stands for, in the order
// the author wrote them. Every media source is resolved to a local file, and checked to be what it
// claims, before the first message goes out: a bad link fails the whole request instead of leaving
// half of it sent.
constexpr size_t kMedia = 8;                        // media elements per request
constexpr size_t kSegments = 2 * kMedia + 1;
constexpr size_t kImageMax = 8u << 20;              // decoded bytes accepted from an inline picture
constexpr size_t kInlineMax = 12u << 20;            // ...and from any other inline media (a request is <= 16 MiB)
constexpr int kImageConfirmMs = 6000;               // how long to wait for WeChat to insert the picture's row
constexpr int kRowWaitMs = 6000;                    // ...and a video's row
constexpr int kSettleMs = 8000;                     // ceiling for waiting on one file/video upload to finish
constexpr int kRequestBudgetMs = 12000;             // ...and for all the waiting one request may do (the server thread is shared)

enum class Kind { Text, Image, Video, Audio, File };
// What is actually sent. A <video> that is not an MP4 family container, or an <audio> in a format
// WeChat cannot play as a voice message, goes out as a file: still delivered, still playable by
// the recipient, and the returned Message says so (it holds a <file>).
enum class Route { Text, Image, Video, Voice, File };

struct Segment {
    Kind kind = Kind::Text;
    Route route = Route::Text;
    size_t begin = 0, end = 0;     // text: the slice of the content; media: the whole tag
    char path[1200] = {};          // media: the local file
    char poster[1200] = {};        // video: optional JPEG poster
    char title[256] = {};          // file: the name the recipient sees
    int duration_s = 0;            // video: play length in seconds
    unsigned duration_ms = 0;      // voice: length in milliseconds
    long long size = 0;            // media: bytes
};

// Magic bytes of the picture formats WeChat's pipeline takes.
bool LooksLikeImage(const unsigned char *head, size_t size) {
    if (size >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) return true;
    if (size >= 4 && head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') return true;
    if (size >= 4 && !memcmp(head, "GIF8", 4)) return true;
    return size >= 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WEBP", 4);
}

bool FileHead(const char *path, unsigned char *head, size_t capacity, size_t *got) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    *got = fread(head, 1, capacity, file);
    fclose(file);
    return true;
}

bool FileIsImage(const char *path) {
    unsigned char head[16] = {};
    size_t got = 0;
    return FileHead(path, head, sizeof(head), &got) && LooksLikeImage(head, got);
}

// A file extension (no dot) for what the bytes look like, or "" when nothing is recognised.
// Used to name inline media and to give an untitled <file> a name that opens with the right app.
const char *SniffExtension(const unsigned char *head, size_t size) {
    if (size >= 3 && head[0] == 0xFF && head[1] == 0xD8 && head[2] == 0xFF) return "jpg";
    if (size >= 4 && head[0] == 0x89 && head[1] == 'P' && head[2] == 'N' && head[3] == 'G') return "png";
    if (size >= 4 && !memcmp(head, "GIF8", 4)) return "gif";
    if (size >= 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WEBP", 4)) return "webp";
    if (size >= 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "WAVE", 4)) return "wav";
    if (size >= 12 && !memcmp(head, "RIFF", 4) && !memcmp(head + 8, "AVI ", 4)) return "avi";
    if (size >= 8 && !memcmp(head + 4, "ftyp", 4)) {
        if (!memcmp(head + 8, "M4A ", 4) || !memcmp(head + 8, "M4B ", 4)) return "m4a";
        if (!memcmp(head + 8, "qt  ", 4)) return "mov";
        if (!memcmp(head + 8, "heic", 4) || !memcmp(head + 8, "heix", 4) || !memcmp(head + 8, "mif1", 4)) return "heic";
        return "mp4";
    }
    if (size >= 4 && head[0] == 0x1A && head[1] == 0x45 && head[2] == 0xDF && head[3] == 0xA3) return "mkv";
    if (size >= 3 && !memcmp(head, "ID3", 3)) return "mp3";
    if (size >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0) return "mp3";
    if (size >= 4 && !memcmp(head, "OggS", 4)) return "ogg";
    if (size >= 4 && !memcmp(head, "fLaC", 4)) return "flac";
    if (size >= 6 && !memcmp(head, "#!AMR\n", 6)) return "amr";
    if (size >= 9 && (!memcmp(head, "#!SILK_V3", 9) || (head[0] == 0x02 && size >= 10 && !memcmp(head + 1, "#!SILK_V3", 9)))) return "silk";
    if (size >= 4 && !memcmp(head, "%PDF", 4)) return "pdf";
    if (size >= 4 && !memcmp(head, "PK\x03\x04", 4)) return "zip";
    if (size >= 3 && !memcmp(head, "FLV", 3)) return "flv";
    return "";
}

// The extension a mime type suggests, for inline media that arrives with one.
const char *MimeExtension(const char *mime) {
    struct Entry { const char *prefix, *extension; };
    static const Entry table[] = {
        {"video/mp4", "mp4"}, {"video/quicktime", "mov"}, {"video/webm", "webm"}, {"video/x-matroska", "mkv"},
        {"audio/mpeg", "mp3"}, {"audio/mp3", "mp3"}, {"audio/wav", "wav"}, {"audio/x-wav", "wav"}, {"audio/ogg", "ogg"},
        {"audio/mp4", "m4a"}, {"audio/aac", "aac"}, {"audio/amr", "amr"}, {"audio/silk", "silk"}, {"audio/flac", "flac"},
        {"application/pdf", "pdf"}, {"application/zip", "zip"}, {"text/plain", "txt"}, {"image/png", "png"},
        {"image/jpeg", "jpg"}, {"image/gif", "gif"}, {"image/webp", "webp"},
    };
    for (const Entry &entry : table) if (mime && !strncasecmp(mime, entry.prefix, strlen(entry.prefix))) return entry.extension;
    return "";
}

// A decoded inline payload -> a temp file named `hint`.<extension> (the same store upload.create
// uses, so it expires by itself). `title_hint` becomes the download name.
const char *StoreInline(Kind kind, const unsigned char *bytes, size_t size, const char *mime, char *path, size_t capacity,
                        char *sniffed_name, size_t sniffed_capacity) {
    if (kind == Kind::Image) {
        if (size > kImageMax) return "image_too_large";
        if (!LooksLikeImage(bytes, size)) return "media_unsupported";
    } else if (size > kInlineMax) {
        return "media_too_large";
    }
    const char *extension = SniffExtension(bytes, size);
    if (!*extension) extension = MimeExtension(mime);
    if (!*extension) extension = kind == Kind::Image ? "jpg" : "bin";
    const char *stem = kind == Kind::Image ? "picture" : kind == Kind::Video ? "video" : kind == Kind::Audio ? "audio" : "file";
    char filename[64];
    snprintf(filename, sizeof(filename), "%s.%s", stem, extension);
    if (sniffed_name) snprintf(sniffed_name, sniffed_capacity, "%s", filename);
    char name[160];
    if (!TempStorePut(filename, "application/octet-stream", reinterpret_cast<const char *>(bytes), size, name, sizeof(name))) return "media_unresolved";
    const TempFile *file = TempStoreGet(name);
    if (!file || strlen(file->path) >= capacity) return "media_unresolved";
    memcpy(path, file->path, strlen(file->path) + 1);
    return nullptr;
}

// A base64 payload -> a temp file. mime may be empty.
const char *DecodeBase64Media(Kind kind, const char *b64, size_t length, const char *mime, char *path, size_t capacity,
                              char *name, size_t name_capacity) {
    const size_t limit = kind == Kind::Image ? kImageMax : kInlineMax;
    if (length / 4 * 3 > limit + 3) return kind == Kind::Image ? "image_too_large" : "media_too_large";
    auto *bytes = static_cast<unsigned char *>(malloc(length / 4 * 3 + 4));
    if (!bytes) return "media_unresolved";
    const long size = Base64Decode(b64, length, bytes, length / 4 * 3 + 4);
    const char *failure = size <= 0 ? "media_unresolved"
                        : StoreInline(kind, bytes, static_cast<size_t>(size), mime, path, capacity, name, name_capacity);
    free(bytes);
    return failure;
}

// Resolves one media element's `src` to a local file. Only sources this adapter can honour are
// accepted: its own upload.create links, media carried inline as data: URIs, and the community
// base64:// scheme (no mime, the format is sniffed from the magic bytes). WeChat has no public URL
// for media and this build has no HTTP client, so a remote URL is refused with a message that says
// what to do instead. `name` receives the client's file name when the source knows one.
const char *ResolveSource(Kind kind, const char *src, char *path, size_t capacity, char *name, size_t name_capacity, const char **detail) {
    *detail = "";
    name[0] = 0;
    const bool image = kind == Kind::Image;
    const char *too_large = image ? "image_too_large" : "media_too_large";
    if (!strncmp(src, "internal:", 9)) {
        if (!TempStoreResolveLink(src, path, capacity)) {
            *detail = "the link is not an upload of this adapter, or it has expired (uploads live for 5 minutes)";
            return "media_unresolved";
        }
        if (const char *stored = strstr(src, "/_tmp/")) TempStoreOriginalName(stored + 6, name, name_capacity);
    } else if (!strncasecmp(src, "data:", 5)) {
        const char *comma = strchr(src, ',');
        char header[128];
        const size_t header_size = comma ? static_cast<size_t>(comma - src) : 0;
        if (!comma || header_size >= sizeof(header)) { *detail = "expected data:<mime>;base64,<data>"; return "media_unresolved"; }
        memcpy(header, src, header_size);
        header[header_size] = 0;
        if (!strcasestr(header, ";base64")) { *detail = "expected data:<mime>;base64,<data>"; return "media_unresolved"; }
        if (image && strncasecmp(header, "data:image/", 11)) { *detail = "expected data:image/...;base64,..."; return "media_unsupported"; }
        const char *failure = DecodeBase64Media(kind, comma + 1, strlen(comma + 1), src + 5, path, capacity, name, name_capacity);
        if (failure) {
            *detail = !strcmp(failure, too_large) ? (image ? "inline pictures are limited to 8 MiB" : "inline media is limited to 12 MiB: use upload.create") : "undecodable base64 payload";
            return failure;
        }
        name[0] = 0;   // an inline payload has no name of its own
    } else if (!strncasecmp(src, "base64://", 9)) {
        const char *failure = DecodeBase64Media(kind, src + 9, strlen(src + 9), "", path, capacity, name, name_capacity);
        if (failure) {
            *detail = !strcmp(failure, too_large) ? (image ? "inline pictures are limited to 8 MiB" : "inline media is limited to 12 MiB: use upload.create") : "expected base64://<base64-encoded data>";
            return failure;
        }
        name[0] = 0;
    } else {
        *detail = "remote URLs cannot be fetched: upload the file with upload.create, or send it inline as a data: URI";
        return "media_unresolved";
    }
    return nullptr;
}

// Keeps a title usable as a file name: no path separators or control characters, and short enough.
void CleanTitle(const char *in, char *out, size_t capacity) {
    size_t used = 0;
    for (const unsigned char *p = reinterpret_cast<const unsigned char *>(in); *p && used + 1 < capacity; ++p) {
        if (*p < 0x20 || *p == 0x7f || *p == '/' || *p == '\\') continue;
        if (used + 4 >= capacity && *p >= 0x80) break;   // never cut a UTF-8 sequence in half
        out[used++] = static_cast<char>(*p);
    }
    while (used && (out[used - 1] == ' ' || out[used - 1] == '.')) --used;
    out[used] = 0;
}

// Decides how one resolved media element is sent, and fills in what that needs (a video's play
// length, a file's name). Returns an error code, or null.
const char *Classify(Segment &segment, const char *tag_title, const char *original_name, const char **detail) {
    unsigned char head[64] = {};
    size_t got = 0;
    struct stat info{};
    if (stat(segment.path, &info) || !S_ISREG(info.st_mode)) { *detail = "the resolved file is missing"; return "media_unresolved"; }
    segment.size = static_cast<long long>(info.st_size);
    if (segment.size <= 0) { *detail = "the file is empty"; return "media_unresolved"; }
    FileHead(segment.path, head, sizeof(head), &got);
    const char *extension = SniffExtension(head, got);
    if (segment.kind == Kind::Image) {
        if (!LooksLikeImage(head, got)) { *detail = "the file is not a JPEG, PNG, GIF or WebP picture"; return "media_unsupported"; }
        segment.route = Route::Image;
        return nullptr;
    }
    segment.route = Route::File;
    if (segment.kind == Kind::Audio) {
        // WeChat plays SILK. Anything Android can decode is converted (or, when it already is
        // WeChat's format, used as is); a clip past WeChat's 60 second limit, or one that cannot be
        // read, is sent as a file instead so it is never lost.
        char silk[1200] = {}, why[160] = {};
        unsigned ms = 0;
        if (VoicePrepare(segment.path, silk, sizeof(silk), &ms, why, sizeof(why)) == VoicePrep::Ready) {
            snprintf(segment.path, sizeof(segment.path), "%s", silk);
            segment.route = Route::Voice;
            segment.duration_ms = ms;
            struct stat silk_info{};
            if (!stat(silk, &silk_info)) segment.size = static_cast<long long>(silk_info.st_size);
            return nullptr;
        }
    }
    if (segment.kind == Kind::Video) {
        Mp4Info mp4;
        if (Mp4Probe(segment.path, &mp4) && mp4.container && mp4.has_video) {
            segment.route = Route::Video;
            segment.duration_s = static_cast<int>((mp4.duration_ms + 999) / 1000);
            if (segment.duration_s < 1) segment.duration_s = 1;
        }
    }
    // A file's name: the tag's title, else the client's own file name, else a name made from what
    // the bytes turned out to be.
    char title[256] = {};
    if (tag_title && *tag_title) CleanTitle(tag_title, title, sizeof(title));
    if (!*title && original_name && *original_name) CleanTitle(original_name, title, sizeof(title));
    if (!*title) {
        const char *stem = segment.kind == Kind::Video ? "video" : segment.kind == Kind::Audio ? "audio" : "file";
        snprintf(title, sizeof(title), "%s.%s", stem, *extension ? extension : "bin");
    } else if (!strrchr(title, '.') && *extension) {
        const size_t used = strlen(title);
        if (used + strlen(extension) + 2 < sizeof(title)) snprintf(title + used, sizeof(title) - used, ".%s", extension);
    }
    snprintf(segment.title, sizeof(segment.title), "%s", title);
    return nullptr;
}

void EscapeAttribute(const char *text, char *out, size_t capacity) {
    size_t used = 0;
    for (; *text && used + 7 < capacity; ++text) {
        const char *entity = *text == '&' ? "&amp;" : *text == '<' ? "&lt;" : *text == '>' ? "&gt;" : *text == '"' ? "&quot;" : nullptr;
        if (entity) { const size_t n = strlen(entity); memcpy(out + used, entity, n); used += n; }
        else out[used++] = *text;
    }
    out[used] = 0;
}

// The Message for a media row we just sent. The store's own decoding is the most faithful (signed
// links, duration, poster); when it cannot read the row back, a message built from what we sent.
cJSON *SentMediaMessage(Store *store, const char *channel_id, long long local_id, const Segment &segment) {
    char id[32];
    snprintf(id, sizeof(id), "%lld", local_id);
    if (cJSON *stored = StoreMessageGet(store, channel_id, id)) return stored;
    char content[900] = {};
    char link[300] = {};
    const char *kind = segment.route == Route::Image ? "image" : segment.route == Route::Video ? "video" : segment.route == Route::Voice ? "voice" : "file";
    const bool linked = MediaLink(StoreSelfId(store), kind, id, link, sizeof(link));
    if (segment.route == Route::Image) {
        snprintf(content, sizeof(content), "<img src=\"%s\"/>", linked ? link : "");
    } else if (segment.route == Route::Video) {
        snprintf(content, sizeof(content), "<video src=\"%s\" duration=\"%d\"/>", linked ? link : "", segment.duration_s);
    } else if (segment.route == Route::Voice) {
        snprintf(content, sizeof(content), "<audio src=\"%s\" duration=\"%.3f\"/>", linked ? link : "", segment.duration_ms / 1000.0);
    } else {
        char title[600];
        EscapeAttribute(segment.title, title, sizeof(title));
        snprintf(content, sizeof(content), "<file src=\"%s\" title=\"%s\"/>", linked ? link : "", title);
    }
    cJSON *message = cJSON_CreateObject();
    cJSON *channel = cJSON_CreateObject();
    cJSON *user = cJSON_CreateObject();
    if (!message || !channel || !user) { cJSON_Delete(message); cJSON_Delete(channel); cJSON_Delete(user); return nullptr; }
    cJSON_AddItemToObject(message, "channel", channel);
    cJSON_AddItemToObject(message, "user", user);
    cJSON_AddStringToObject(message, "id", id);
    cJSON_AddStringToObject(message, "content", content);
    cJSON_AddNumberToObject(message, "created_at", static_cast<double>(time(nullptr)) * 1000);
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

long long NowMs() {
    timespec ts{};
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<long long>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}

void Pause(int ms) {
    const timespec pause{0, ms * 1000 * 1000};
    nanosleep(&pause, nullptr);
}

// Waits (bounded by both `limit_ms` and what is left of the request's budget) until WeChat's own
// row leaves "sending": 2 = sent, 5 = failed, 1 = still uploading when the wait ran out.
int WaitSettled(Store *store, long long local_id, int limit_ms, long long deadline) {
    int status = 1;
    const long long stop = NowMs() + limit_ms < deadline ? NowMs() + limit_ms : deadline;
    for (;;) {
        if (!StoreSentStatus(store, local_id, &status)) status = 1;
        if (status != 1 || NowMs() >= stop) return status;
        Pause(100);
    }
}

// How long a file or video of `bytes` is given to finish uploading before the request answers.
int SettleLimit(long long bytes) {
    const long long limit = 1500 + bytes / (1 << 20) * 400;
    return limit > kSettleMs ? kSettleMs : static_cast<int>(limit);
}

// The quoted sender's name the way the conversation shows it: their group nickname in a group, their
// own name otherwise; the wxid when nothing better is known.
void QuoteDisplayName(Store *store, const char *channel_id, QuoteRef &quote) {
    quote.display[0] = 0;
    if (strstr(channel_id, "@chatroom") && MentionName(const_cast<char *>(channel_id), quote.sender, quote.display, sizeof(quote.display))) return;
    if (cJSON *user = StoreUserGet(store, quote.sender)) {
        const cJSON *nick = cJSON_GetObjectItemCaseSensitive(user, "nick");
        const cJSON *name = cJSON_GetObjectItemCaseSensitive(user, "name");
        const char *chosen = cJSON_IsString(nick) && *nick->valuestring ? nick->valuestring : cJSON_IsString(name) ? name->valuestring : "";
        snprintf(quote.display, sizeof(quote.display), "%s", chosen);
        cJSON_Delete(user);
    }
    if (!quote.display[0]) snprintf(quote.display, sizeof(quote.display), "%s", quote.sender);
}

// The messages one <message> part stands for. `already_sent` is what earlier parts of the same
// request delivered (a failure reports the total); `tolerate_blank` lets a part with nothing to send
// (whitespace, an <author/> alone) pass as an empty list instead of failing the whole request.
Response CreateOne(const Request &request, Store *store, const char *content, size_t already_sent, bool tolerate_blank) {
    if (!store) return {503, nullptr};
    const char *channel_id = Text(request, "channel_id");
    if (!*channel_id || !*content) return {400, nullptr};
    const bool group = strstr(channel_id, "@chatroom") != nullptr;
    // <quote id="..."/> turns the first text of the request into a reply to that message. WeChat's
    // reply carries text only, so a request with no text has nothing to attach it to, and a quoted
    // message that cannot be found just leaves an ordinary message.
    char quote_id[40] = {};
    {
        ImageSpan quote_tag;
        if (FirstTag(content, "quote", &quote_tag)) TagAttribute(content, quote_tag, "id", quote_id, sizeof(quote_id));
    }
    bool quote_pending = quote_id[0] != 0;
    // Rows this request makes are ours, not the owner's typing: the event poller asks the store.
    struct SendWindow {
        Store *store; const char *talker;
        SendWindow(Store *s, const char *t) : store(s), talker(t) { StoreSendBegin(store, talker); }
        ~SendWindow() { StoreSendEnd(store, talker); }
    } send_window(store, channel_id);

    // Cut the content at its media elements.
    MediaSpan spans[kMedia + 1];
    const size_t span_count = MediaSpans(content, spans, kMedia + 1);
    if (span_count > kMedia) return BadRequest("too_many_media", "at most 8 media elements per message.create");
    size_t images = 0;
    for (size_t i = 0; i < span_count; ++i) if (spans[i].kind == 'i') ++images;
    if (images > kImages) return BadRequest("too_many_images", "at most 4 pictures per message.create");
    Segment segments[kSegments];
    size_t segment_count = 0, cursor = 0;
    for (size_t i = 0; i <= span_count; ++i) {
        const size_t stop = i < span_count ? spans[i].begin : strlen(content);
        if (stop > cursor) { segments[segment_count].begin = cursor; segments[segment_count].end = stop; ++segment_count; }
        if (i < span_count) {
            Segment &media = segments[segment_count];
            media.kind = spans[i].kind == 'i' ? Kind::Image : spans[i].kind == 'v' ? Kind::Video : spans[i].kind == 'a' ? Kind::Audio : Kind::File;
            media.begin = spans[i].begin;
            media.end = spans[i].end;
            ++segment_count;
            cursor = spans[i].end;
        }
    }

    // Resolve every media element up front, and flatten every text segment to see which are blank.
    struct Text { char plain[kOutgoingMax + 1]; char ids[2100]; bool blank; };
    auto *texts = static_cast<Text *>(calloc(kSegments, sizeof(Text)));
    if (!texts) return {500, nullptr};
    size_t sendable = 0;
    for (size_t i = 0; i < segment_count; ++i) {
        Segment &segment = segments[i];
        if (segment.kind != Kind::Text) {
            const ImageSpan tag{segment.begin, segment.end};
            char *src = static_cast<char *>(malloc(strlen(content) + 1));
            if (!src) { free(texts); return {500, nullptr}; }
            src[0] = 0;
            const bool has_src = TagAttribute(content, tag, "src", src, strlen(content) + 1) && *src;
            const char *detail = "";
            char original[256] = {};
            const char *failure = has_src ? ResolveSource(segment.kind, src, segment.path, sizeof(segment.path), original, sizeof(original), &detail)
                                          : "media_unresolved";
            if (!has_src) detail = "the element has no src";
            free(src);
            if (failure) { free(texts); return BadRequest(failure, detail); }
            char title[256] = {};
            TagAttribute(content, tag, "title", title, sizeof(title));
            failure = Classify(segment, title, original, &detail);
            if (failure) { free(texts); return BadRequest(failure, detail); }
            if (segment.route == Route::Video) {
                // An optional poster: a picture that cannot be read is simply not used.
                char poster_src[1200] = {};
                if (TagAttribute(content, tag, "poster", poster_src, sizeof(poster_src)) && *poster_src) {
                    char poster_original[256];
                    const char *poster_detail = "";
                    if (ResolveSource(Kind::Image, poster_src, segment.poster, sizeof(segment.poster), poster_original, sizeof(poster_original), &poster_detail) ||
                        !FileIsImage(segment.poster)) segment.poster[0] = 0;
                }
            }
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
        return tolerate_blank ? Response{200, cJSON_CreateArray()} : Response{400, nullptr};
    }

    cJSON *list = cJSON_CreateArray();
    if (!list) { free(texts); return {500, nullptr}; }
    const long long deadline = NowMs() + kRequestBudgetMs;
    size_t sent_count = already_sent;
    for (size_t i = 0; i < segment_count; ++i) {
        cJSON *message = nullptr;
        const Segment &segment = segments[i];
        if (segment.route == Route::Image) {
            const long long before = StoreWatermark(store);
            SendResult sent = SendImage(channel_id, StoreSelfId(store), segment.path);
            if (!sent.ok) { cJSON_Delete(list); free(texts); return FailureAfter("send_failed", sent.detail, sent.rejected, sent_count); }
            // The pipeline is asynchronous: only a row in WeChat's own table proves it started.
            long long local_id = 0;
            bool confirmed = false;
            for (int waited = 0; waited < kImageConfirmMs && !confirmed; waited += 40) {
                confirmed = StoreFindSentImage(store, channel_id, before, &local_id);
                if (!confirmed) Pause(40);
            }
            if (!confirmed) {
                cJSON_Delete(list); free(texts);
                // A row with empty content means WeChat's pipeline took the request and stalled
                // (it rejects degenerate pictures, e.g. 1x1); say so instead of guessing.
                long long stalled_id = 0;
                if (StoreFindStalledImage(store, channel_id, before, &stalled_id))
                    return FailureAfter("image_unconfirmed", "WeChat recorded the picture but its pipeline stalled (content empty); the image was not sent", false, sent_count);
                return FailureAfter("image_unconfirmed", "WeChat did not record the picture within 6 seconds; it may still be sent", false, sent_count);
            }
            StoreNoteSent(store, local_id);
            message = SentMediaMessage(store, channel_id, local_id, segment);
        } else if (segment.route == Route::File || segment.route == Route::Video || segment.route == Route::Voice) {
            const bool video = segment.route == Route::Video;
            const bool voice = segment.route == Route::Voice;
            const char *what = video ? "video" : voice ? "voice message" : "file";
            long long local_id = 0;
            if (voice) {
                const long long before = StoreWatermark(store);
                SendResult sent = SendVoice(channel_id, segment.path, static_cast<int>(segment.duration_ms));
                if (!sent.ok) { cJSON_Delete(list); free(texts); return FailureAfter("send_failed", sent.detail, sent.rejected, sent_count); }
                bool found = false;
                for (int waited = 0; waited < kRowWaitMs && !found; waited += 50) {
                    found = StoreFindSentVoice(store, channel_id, before, &local_id);
                    if (!found) Pause(50);
                }
                if (!found) {
                    cJSON_Delete(list); free(texts);
                    return FailureAfter("voice_unconfirmed", "WeChat did not record the voice message within 6 seconds; it may still be sent", false, sent_count);
                }
            } else if (video) {
                const long long before = StoreWatermark(store);
                SendResult sent = SendVideo(channel_id, segment.path, segment.poster, segment.duration_s);
                if (!sent.ok) { cJSON_Delete(list); free(texts); return FailureAfter("send_failed", sent.detail, sent.rejected, sent_count); }
                // The video task runs in WeChat's own coroutine; its row appears when it starts.
                bool found = false;
                for (int waited = 0; waited < kRowWaitMs && !found; waited += 50) {
                    found = StoreFindSentVideo(store, channel_id, before, &local_id);
                    if (!found) Pause(50);
                }
                if (!found) {
                    cJSON_Delete(list); free(texts);
                    return FailureAfter("video_unconfirmed", "WeChat did not record the video within 6 seconds; it may still be sent", false, sent_count);
                }
            } else {
                SendResult sent = SendFile(channel_id, segment.path, segment.title);
                if (!sent.ok) { cJSON_Delete(list); free(texts); return FailureAfter("send_failed", sent.detail, sent.rejected, sent_count); }
                local_id = sent.local_id;
            }
            StoreNoteSent(store, local_id);
            // The row exists now and WeChat is uploading; a failure it reports quickly (no network,
            // a rejected file) is worth telling the caller, a slow upload is left to finish.
            const int status = WaitSettled(store, local_id, SettleLimit(segment.size), deadline);
            if (status == 5) {
                cJSON_Delete(list); free(texts);
                char detail[120];
                snprintf(detail, sizeof(detail), "WeChat could not upload the %s (its send status is failed)", what);
                return FailureAfter("upload_failed", detail, false, sent_count);
            }
            message = SentMediaMessage(store, channel_id, local_id, segment);
        } else if (!texts[i].blank) {
            SendResult sent{};
            bool quoted = false;
            if (quote_pending) {
                quote_pending = false;
                QuoteRef target;
                if (StoreQuoteTarget(store, channel_id, quote_id, &target)) {
                    QuoteDisplayName(store, channel_id, target);
                    const long long before = StoreWatermark(store);
                    sent = SendQuote(channel_id, texts[i].plain, target, texts[i].ids);
                    quoted = sent.ok;
                    if (quoted && sent.local_id <= 0) {
                        // WeChat's send pipeline takes the reply and inserts its row a moment later.
                        long long local_id = 0;
                        bool found = false;
                        for (int waited = 0; waited < kRowWaitMs && !found; waited += 50) {
                            found = StoreFindSentQuote(store, channel_id, before, &local_id);
                            if (!found) Pause(50);
                        }
                        if (!found) {
                            cJSON_Delete(list); free(texts);
                            return FailureAfter("send_unconfirmed", "WeChat did not record the reply within 6 seconds; it may still be sent", false, sent_count);
                        }
                        sent.local_id = local_id;
                    }
                }
            }
            if (!quoted) sent = SendText(channel_id, texts[i].plain, texts[i].ids);
            if (!sent.ok) { cJSON_Delete(list); free(texts); return FailureAfter("send_failed", sent.detail, sent.rejected, sent_count); }
            StoreNoteSent(store, sent.local_id);
            message = SentMessage(store, channel_id, texts[i].plain, sent.local_id);
            if (message && quoted) {
                // The reply's content is the quote element followed by its text, as Satori writes it.
                cJSON *field = cJSON_GetObjectItemCaseSensitive(message, "content");
                char escaped_id[80];
                EscapeAttribute(quote_id, escaped_id, sizeof(escaped_id));
                char combined[kOutgoingMax + 200];
                snprintf(combined, sizeof(combined), "<quote id=\"%s\"/>%s", escaped_id, cJSON_IsString(field) ? field->valuestring : "");
                cJSON_ReplaceItemInObjectCaseSensitive(message, "content", cJSON_CreateString(combined));
            }
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

// `<message>` is Satori's container for "this is one message": what precedes and follows it is sent
// separately, so a request may carry several. Each part goes through the same pipeline (its own
// text and media split, its own <quote>) and the replies are returned in order. Merge forwarding
// (`<message forward>`) has no WeChat counterpart, and sending its contents as separate messages
// would silently be something else, so it is refused.
constexpr size_t kParts = 16;

Response CreateMessages(const Request &request, Store *store) {
    const char *content = Text(request, "content");
    if (!store) return {503, nullptr};
    if (!*Text(request, "channel_id") || !*content) return {400, nullptr};
    MessagePart parts[kParts + 1];
    bool forward = false;
    const size_t count = MessageParts(content, parts, kParts + 1, &forward);
    if (forward) return BadRequest("forward_unsupported", "WeChat cannot merge-forward messages");
    if (count > kParts) return BadRequest("too_many_messages", "at most 16 <message> parts per message.create");
    if (count <= 1 && !strstr(content, "<message")) return CreateOne(request, store, content, 0, false);
    cJSON *all = cJSON_CreateArray();
    if (!all) return {500, nullptr};
    size_t delivered = 0;
    for (size_t i = 0; i < count; ++i) {
        char *slice = static_cast<char *>(malloc(parts[i].end - parts[i].begin + 1));
        if (!slice) { cJSON_Delete(all); return {500, nullptr}; }
        memcpy(slice, content + parts[i].begin, parts[i].end - parts[i].begin);
        slice[parts[i].end - parts[i].begin] = 0;
        Response part = CreateOne(request, store, slice, delivered, true);
        free(slice);
        if (part.status != 200 || !part.body) { cJSON_Delete(all); return part; }
        // Move the part's messages into the combined reply.
        while (part.body->child) {
            cJSON *message = cJSON_DetachItemFromArray(part.body, 0);
            cJSON_AddItemToArray(all, message);
            ++delivered;
        }
        cJSON_Delete(part.body);
    }
    if (!delivered) { cJSON_Delete(all); return {400, nullptr}; }
    return {200, all};
}

Response Call(void *, const Request &request) {
    const char *name = request.method->name;
    const OutboundHold hold(name && IsOutbound(name));
    Store *store = LiveStore();

    if (!strcmp(name, "message.create")) return CreateMessages(request, store);

    if (!strcmp(name, "message.delete")) {
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
        const char *channel_id = Text(request, "channel_id");
        if (!*channel_id || !strstr(channel_id, "@chatroom")) return {400, nullptr};
        ActionResult action = RoomRemoveMember(channel_id, StoreSelfId(store));
        if (!action.ok) return Failure("leave_failed", action.detail, action.rejected);
        return {200, cJSON_CreateObject()};
    }
    if (!strcmp(name, "guild.member.kick")) {
        const char *guild_id = Text(request, "guild_id"), *user_id = Text(request, "user_id");
        if (!*guild_id || !*user_id) return {400, nullptr};
        ActionResult action = RoomRemoveMember(guild_id, user_id);
        if (!action.ok) return Failure("kick_failed", action.detail, action.rejected);
        return {200, cJSON_CreateObject()};
    }
    if (!strcmp(name, "guild.member.role.set") || !strcmp(name, "guild.member.role.unset")) {
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
