// Host-side tests for the native backend's decision logic, which until now had no coverage at
// all — the file that twice shipped a wrong answer. The real wx_backend.cpp is linked in and
// driven through the same `Backend::call` entry the server uses; the WeChat-side pieces it
// talks to (store, room actions, keepalive) are stubbed below, and the sender is the real one,
// which cannot reach a JavaVM here — that is exactly what lets a test tell "the text path ran"
// (502 send_failed) from "the request was refused before sending" (400/404).
#include "wx_backend.h"
#include "wx_capabilities.h"
#include "wx_store.h"
#include "wx_live.h"
#include "wx_room.h"
#include "wx_send.h"
#include "wx_voice.h"
#include "wx_keepalive.h"
#include "protocol.h"
#include "tempstore.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

// ---- test state the stubs write to -----------------------------------------------------------
long long g_next_row = 7000;
int g_status = 2;
struct SentCall {
    char kind;
    char talker[64];
    char path[1200];
    char title[300];
    char poster[1200];
    int duration;
};
SentCall g_calls[16];
int g_call_count = 0;
satori::VoicePrep g_voice_result = satori::VoicePrep::Failed; // what the (stubbed) audio conversion answers
unsigned g_voice_ms = 2500;
satori::QuoteRef g_last_quote;
char g_last_reply[300];
char g_last_reply_mentions[100];
int g_reply_count = 0;
struct ForwardCall {
    char talker[64];
    char title[200];
    char desc[400];
    char *record;
};
ForwardCall g_forward;
int g_forward_count = 0;
long long g_forward_id = 9100; // what the stub sender answers as the row's id (<= 0: the pipeline hides it)
bool g_forward_ok = true;

// ---- stubs for the WeChat-side collaborators (none of them run in this process) ----------
namespace satori {
// A non-null handle: the stubs below ignore it, and the backend refuses to send without a store.
Store *LiveStore() { return reinterpret_cast<Store *>(1); }
bool StoreFindSentImage(Store *, const char *, long long, long long *) { return false; }
bool StoreFindStalledImage(Store *, const char *, long long, long long *) { return false; }
// A video row that appears at once, and a status the test can flip to "failed".
bool StoreFindSentVideo(Store *, const char *, long long, long long *id) {
    *id = ++::g_next_row;
    return true;
}
bool StoreFindSentVoice(Store *, const char *, long long, long long *id) {
    *id = ++::g_next_row;
    return true;
}
VoicePrep VoicePrepare(const char *in_path, char *out_path, size_t capacity, unsigned *duration_ms, char *, size_t) {
    *duration_ms = 0;
    if (::g_voice_result != VoicePrep::Ready) return ::g_voice_result;
    snprintf(out_path, capacity, "%s", in_path);
    *duration_ms = ::g_voice_ms;
    return VoicePrep::Ready;
}
SendResult SendVoice(const char *talker, const char *path, int duration_ms) {
    SentCall &call = ::g_calls[::g_call_count++ % 16];
    call = {};
    call.kind = 'a';
    snprintf(call.talker, sizeof(call.talker), "%s", talker);
    snprintf(call.path, sizeof(call.path), "%s", path);
    call.duration = duration_ms;
    SendResult result{};
    result.ok = true;
    return result;
}
bool StoreFindSentQuote(Store *, const char *, long long, long long *id) {
    *id = ++::g_next_row;
    return true;
}
bool StoreSentStatus(Store *, long long, int *status) {
    *status = ::g_status;
    return true;
}
// The message being quoted: any id except "404" exists.
bool StoreQuoteTarget(Store *, const char *talker, const char *id, QuoteRef *out) {
    if (!strcmp(id, "404")) return false;
    *out = {};
    out->row_type = !strcmp(id, "300") ? 3 : 1; // "300" is a picture
    out->local_id = 55;
    out->svr_id = 5555;
    out->created_s = 1790000000;
    snprintf(out->talker, sizeof(out->talker), "%s", talker);
    snprintf(out->sender, sizeof(out->sender), !strcmp(id, "77") ? "known_wxid" : "wxid_quoted");
    snprintf(out->text, sizeof(out->text), "the quoted line");
    return true;
}
int LiveLoginSn() { return 1; }
bool StartLiveStore(const char *, EventBus *, int) { return false; }
const char *StoreSelfId(Store *) { return "self_wxid"; }
long long StoreWatermark(Store *) { return 0; }
void StoreSendBegin(Store *, const char *) {}
void StoreSendEnd(Store *, const char *) {}
void StoreNoteSent(Store *, long long) {}
long long StorePoll(Store *, long long, int, bool (*)(void *, const char *), void *, bool *) { return 0; }
cJSON *StoreMessageList(Store *, const char *, const char *, const char *, int, const char *) { return nullptr; }
cJSON *StoreMessageGet(Store *, const char *, const char *) { return nullptr; }
// A known contact: "known_wxid" has a nickname and an avatar; everyone else is unknown.
cJSON *StoreUserGet(Store *, const char *id) {
    if (strcmp(id, "known_wxid")) return nullptr;
    cJSON *user = cJSON_CreateObject();
    cJSON_AddStringToObject(user, "id", id);
    cJSON_AddStringToObject(user, "nick", "Known Nick");
    cJSON_AddStringToObject(user, "avatar", "https://wx.example/known");
    return user;
}
cJSON *StoreFriendList(Store *, const char *, int) { return nullptr; }
cJSON *StoreGuildList(Store *, const char *, int) { return nullptr; }
cJSON *StoreGuildGet(Store *, const char *) { return nullptr; }
cJSON *StoreChannelGet(Store *, const char *) { return nullptr; }
cJSON *StoreChannelList(Store *, const char *, const char *, int) { return nullptr; }
cJSON *StoreGuildMemberList(Store *, const char *, const char *, int) { return nullptr; }
cJSON *StoreGuildMemberGet(Store *, const char *, const char *) { return nullptr; }
cJSON *StoreGuildRoleList(Store *, const char *) { return nullptr; }
cJSON *StoreMemberRoleList(Store *, const char *, const char *) { return nullptr; }
ActionResult RoomRemoveMember(const char *, const char *) { return {}; }
ActionResult RoomSetAdmin(const char *, const char *, bool) { return {}; }
void KeepaliveWakelockBegin() {}
void KeepaliveWakelockEnd() {}

// The media senders are stubbed too (the real ones need WeChat's JVM): they record what the backend
// decided to send, which is exactly what the routing tests are about.
SendResult SendFile(const char *talker, const char *path, const char *title) {
    SentCall &call = ::g_calls[::g_call_count++ % 16];
    call = {};
    call.kind = 'f';
    snprintf(call.talker, sizeof(call.talker), "%s", talker);
    snprintf(call.path, sizeof(call.path), "%s", path);
    snprintf(call.title, sizeof(call.title), "%s", title);
    SendResult result{};
    result.ok = true;
    result.local_id = 9000 + ::g_call_count;
    return result;
}
SendResult SendQuote(const char *, const char *text, const QuoteRef &quote, const char *mention_ids) {
    ::g_last_quote = quote;
    snprintf(::g_last_reply, sizeof(::g_last_reply), "%s", text);
    snprintf(::g_last_reply_mentions, sizeof(::g_last_reply_mentions), "%s", mention_ids ? mention_ids : "");
    ++::g_reply_count;
    SendResult result{};
    result.ok = true;
    result.local_id = -1; // like WeChat's own pipeline: the row is found in the store afterwards
    return result;
}
SendResult SendForward(const char *talker, const char *title, const char *desc, const char *record_info) {
    free(::g_forward.record);
    ::g_forward = {};
    snprintf(::g_forward.talker, sizeof(::g_forward.talker), "%s", talker);
    snprintf(::g_forward.title, sizeof(::g_forward.title), "%s", title);
    snprintf(::g_forward.desc, sizeof(::g_forward.desc), "%s", desc);
    ::g_forward.record = strdup(record_info);
    ++::g_forward_count;
    SendResult result{};
    result.ok = ::g_forward_ok;
    result.local_id = ::g_forward_id;
    if (!result.ok) snprintf(result.detail, sizeof(result.detail), "stub refused");
    return result;
}
bool StoreFindSentRecord(Store *, const char *, long long, long long *id) {
    *id = ++::g_next_row;
    return true;
}
SendResult SendVideo(const char *talker, const char *path, const char *poster, int duration_s) {
    SentCall &call = ::g_calls[::g_call_count++ % 16];
    call = {};
    call.kind = 'v';
    snprintf(call.talker, sizeof(call.talker), "%s", talker);
    snprintf(call.path, sizeof(call.path), "%s", path);
    snprintf(call.poster, sizeof(call.poster), "%s", poster ? poster : "");
    call.duration = duration_s;
    SendResult result{};
    result.ok = true;
    return result;
}
} // namespace satori

namespace {
int failures = 0;

void Check(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}

struct Outcome {
    int status;
    cJSON *body;
};

// Drives one message.create through the real backend.
Outcome CreateIn(const char *channel, const char *content) {
    cJSON *params = cJSON_CreateObject();
    cJSON_AddStringToObject(params, "channel_id", channel);
    cJSON_AddStringToObject(params, "content", content);
    const satori::Method *method = satori::FindMethod("message.create");
    Check(method != nullptr, "message.create is a known method");
    const satori::Request request{method, "wechat", "self_wxid", params, "application/json", "", 0, nullptr};
    const satori::Backend *backend = satori::WeChatBackend();
    Check(backend && backend->call, "the WeChat backend is registered");
    satori::Response response = backend->call(backend->context, request);
    cJSON_Delete(params);
    return {response.status, response.body};
}

Outcome Create(const char *content) { return CreateIn("filehelper", content); }

// The error code the backend put in the body, or "" when there is no body.
const char *Code(const Outcome &outcome) {
    const cJSON *error = outcome.body ? cJSON_GetObjectItemCaseSensitive(outcome.body, "code") : nullptr;
    return cJSON_IsString(error) ? error->valuestring : "";
}
} // namespace

int main() {
    // Text is flattened and handed to the sender. There is no JavaVM here, so the sender
    // refuses after the flattening step: that is the proof the text path was taken and not
    // some earlier refusal.
    {
        const Outcome outcome = Create("a &amp; b<br/>c <at id=\"7\"/><quote id=\"404\"/>");
        Check(outcome.status == 502 && !strcmp(Code(outcome), "send_failed"),
              "text content reaches the sender (flattened, elements dropped)");
        cJSON_Delete(outcome.body);
    }

    // ---- pictures ---------------------------------------------------------------------------------
    // The sender cannot reach a JavaVM here, so a picture that passes every check ends at the
    // sender with 502 send_failed; anything wrong with the request itself must be refused first
    // (400) and, above all, before any part of the request is sent.
    const char *scratch_root = getenv("SATORI_TMPROOT");
    char scratch_template[512];
    snprintf(scratch_template, sizeof(scratch_template), "%s/satori-backend-XXXXXX",
             scratch_root && *scratch_root ? scratch_root : ".");
    char *directory = mkdtemp(scratch_template);
    Check(directory != nullptr, "temp directory");
    satori::TempStoreSetDir(directory);
    const char jpeg[] = "\xFF\xD8\xFF\xE0\0\x10JFIF";
    char jpeg_name[160], text_name[160];
    Check(satori::TempStorePut("a.jpg", "image/jpeg", jpeg, sizeof(jpeg) - 1, jpeg_name, sizeof(jpeg_name)),
          "store a picture");
    Check(satori::TempStorePut("notes.txt", "text/plain", "just words", 10, text_name, sizeof(text_name)),
          "store a non-picture");
    char content[1024];
    {
        snprintf(content, sizeof(content), "<img src=\"internal:wechat/self_wxid/_tmp/%s\"/>", jpeg_name);
        const Outcome outcome = Create(content);
        Check(outcome.status == 502 && !strcmp(Code(outcome), "send_failed"),
              "a picture from upload.create reaches the sender");
        cJSON_Delete(outcome.body);
    }
    {
        // "Only a picture" is content, not an empty message.
        const Outcome outcome = Create("<img src=\"internal:wechat/self_wxid/_tmp/does-not-exist.png\"/>");
        Check(outcome.status == 400 && !strcmp(Code(outcome), "media_unresolved"),
              "a link that is not (or no longer) an upload is refused");
        cJSON_Delete(outcome.body);
    }
    {
        snprintf(content, sizeof(content), "<img src=\"internal:wechat/self_wxid/_tmp/%s\"/>", text_name);
        const Outcome outcome = Create(content);
        Check(outcome.status == 400 && !strcmp(Code(outcome), "media_unsupported"),
              "a file that is not a picture is refused");
        cJSON_Delete(outcome.body);
    }
    {
        const Outcome outcome = Create("<img src=\"https://example.invalid/a.png\"/>");
        Check(outcome.status == 400 && !strcmp(Code(outcome), "media_unresolved"),
              "remote URLs are refused with advice");
        cJSON_Delete(outcome.body);
    }
    {
        const Outcome outcome = Create("<img/>");
        Check(outcome.status == 400 && !strcmp(Code(outcome), "media_unresolved"), "an <img> without a source");
        cJSON_Delete(outcome.body);
    }
    {
        // 1x1 PNG, inline.
        const Outcome outcome = Create(
            "<img src=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/q842iQAAAABJRU5ErkJggg==\"/>");
        Check(outcome.status == 502 && !strcmp(Code(outcome), "send_failed"),
              "an inline picture is decoded and reaches the sender");
        cJSON_Delete(outcome.body);
        const Outcome not_image = Create("<img src=\"data:image/png;base64,aGVsbG8gd29ybGQ=\"/>");
        Check(not_image.status == 400 && !strcmp(Code(not_image), "media_unsupported"),
              "an inline blob that is not a picture");
        cJSON_Delete(not_image.body);
        const Outcome not_base64 = Create("<img src=\"data:image/png,rawbytes\"/>");
        Check(not_base64.status == 400 && !strcmp(Code(not_base64), "media_unresolved"), "only base64 data: URIs");
        cJSON_Delete(not_base64.body);
        const Outcome garbage = Create("<img src=\"data:image/png;base64,!!!!\"/>");
        Check(garbage.status == 400 && !strcmp(Code(garbage), "media_unresolved"), "undecodable data");
        cJSON_Delete(garbage.body);
        const Outcome wrong_type = Create("<img src=\"data:text/plain;base64,aGVsbG8=\"/>");
        Check(wrong_type.status == 400 && !strcmp(Code(wrong_type), "media_unsupported"),
              "a data: URI that is not an image type");
        cJSON_Delete(wrong_type.body);
    }
    {
        // The base64:// scheme (the same one satori-qq takes) without a mime: the format is
        // sniffed from the magic bytes.
        const Outcome outcome = Create(
            "<img src=\"base64://iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/q842iQAAAABJRU5ErkJggg==\"/>");
        Check(outcome.status == 502 && !strcmp(Code(outcome), "send_failed"),
              "a base64:// picture is decoded and reaches the sender");
        cJSON_Delete(outcome.body);
        const Outcome not_image = Create("<img src=\"base64://aGVsbG8gd29ybGQ=\"/>");
        Check(not_image.status == 400 && !strcmp(Code(not_image), "media_unsupported"),
              "a base64:// blob that is not a picture");
        cJSON_Delete(not_image.body);
        const Outcome garbage = Create("<img src=\"base64://!!!!\"/>");
        Check(garbage.status == 400 && !strcmp(Code(garbage), "media_unresolved"), "undecodable base64://");
        cJSON_Delete(garbage.body);
    }
    {
        // Text and picture mixed: the request is validated as a whole. A bad picture stops the
        // text before it from going out, which shows in the error code (400, not 502).
        const Outcome outcome = Create("先说这个<img src=\"https://example.invalid/a.png\"/>再说那个");
        Check(outcome.status == 400 && !strcmp(Code(outcome), "media_unresolved"),
              "a bad picture stops the whole request before anything is sent");
        cJSON_Delete(outcome.body);
        snprintf(content, sizeof(content), "看图<img src=\"internal:wechat/self_wxid/_tmp/%s\"/>怎么样", jpeg_name);
        const Outcome mixed = Create(content);
        Check(mixed.status == 502 && !strcmp(Code(mixed), "send_failed"),
              "text before a good picture goes first (and reaches the sender)");
        cJSON_Delete(mixed.body);
        snprintf(
            content, sizeof(content),
            "<img src=\"internal:wechat/self_wxid/_tmp/%s\"/><img src=\"internal:wechat/self_wxid/_tmp/%s\"/><img src=\"internal:wechat/self_wxid/_tmp/%s\"/>"
            "<img src=\"internal:wechat/self_wxid/_tmp/%s\"/><img src=\"internal:wechat/self_wxid/_tmp/%s\"/>",
            jpeg_name, jpeg_name, jpeg_name, jpeg_name, jpeg_name);
        const Outcome many = Create(content);
        Check(many.status == 400 && !strcmp(Code(many), "too_many_images"), "more than four pictures");
        cJSON_Delete(many.body);
    }
    // ---- replies (<quote>) ------------------------------------------------------------------------
    {
        g_reply_count = 0;
        Outcome reply = Create("<quote id=\"123\"/>好的，收到");
        Check(reply.status == 200 && g_reply_count == 1 && !strcmp(g_last_reply, "好的，收到"),
              "a quoted text goes out as a reply");
        Check(g_last_quote.svr_id == 5555 && !strcmp(g_last_quote.sender, "wxid_quoted") &&
                  !strcmp(g_last_quote.display, "wxid_quoted"),
              "the reply carries the quoted message and, without a better name, the sender's id");
        const cJSON *first = cJSON_GetArrayItem(reply.body, 0);
        const cJSON *body = first ? cJSON_GetObjectItemCaseSensitive(first, "content") : nullptr;
        Check(cJSON_IsString(body) && !strcmp(body->valuestring, "<quote id=\"123\"/>好的，收到"),
              "the reply's content says what it quoted");
        cJSON_Delete(reply.body);

        // A quoted message that cannot be found leaves an ordinary message (here that reaches the
        // real text sender, which has no JavaVM).
        g_reply_count = 0;
        Outcome missing_target = Create("<quote id=\"404\"/>还是要说");
        Check(missing_target.status == 502 && !strcmp(Code(missing_target), "send_failed") && g_reply_count == 0,
              "an unknown quoted message falls back to plain text");
        cJSON_Delete(missing_target.body);

        // Only the first text carries the quote; a quote with no text has nothing to attach to.
        g_reply_count = 0;
        Outcome nothing = Create("<quote id=\"123\"/>");
        Check(nothing.status == 400 && g_reply_count == 0, "a quote with nothing to say is empty");
        cJSON_Delete(nothing.body);
        Outcome mentioned = CreateIn("123@chatroom", "<quote id=\"123\"/><at id=\"wxid_a\" name=\"甲\"/> 看这里");
        Check(mentioned.status == 200 && !strcmp(g_last_reply_mentions, "wxid_a") &&
                  strstr(g_last_reply, "@甲") != nullptr,
              "a group reply keeps its mentions");
        cJSON_Delete(mentioned.body);
        char quoted_media[600];
        snprintf(quoted_media, sizeof(quoted_media),
                 "<quote id=\"123\"/><img src=\"internal:wechat/self_wxid/_tmp/%s\"/>", jpeg_name);
        g_reply_count = 0;
        Outcome with_picture = Create(quoted_media);
        Check(with_picture.status == 502 && !strcmp(Code(with_picture), "send_failed") && g_reply_count == 0,
              "a quote next to a picture leaves the picture alone (no reply is sent)");
        cJSON_Delete(with_picture.body);
    }

    // ---- files, videos and audio ------------------------------------------------------------------
    // The senders are stubs here; what matters is which one the backend picks, with what.
    {
        size_t mp4_size = 0, mp3_size = 0;
        const char *fixtures = getenv("SATORI_FIXTURES");
        char fixture[600];
        snprintf(fixture, sizeof(fixture), "%s/tiny.mp4", fixtures ? fixtures : "tests/fixtures");
        FILE *file = fopen(fixture, "rb");
        Check(file != nullptr, "the mp4 fixture is there");
        static unsigned char mp4[16384];
        mp4_size = file ? fread(mp4, 1, sizeof(mp4), file) : 0;
        if (file) fclose(file);
        static const unsigned char mp3[] = {'I', 'D', '3', 3, 0, 0, 0, 0, 0, 0x21, 'T',
                                            'I', 'T', '2', 0, 0, 0, 5, 0, 0, 0,    'x'};
        mp3_size = sizeof(mp3);
        static const unsigned char mkv[] = {0x1A, 0x45, 0xDF, 0xA3, 0x9F, 0x42, 0x86,
                                            0x81, 0x01, 0x42, 0xF7, 0x81, 0x01};
        char video_name[160], mp3_name[160], mkv_name[160], pdf_name[160], jpeg_name2[160], odd_name[160];
        Check(satori::TempStorePut("clip.mp4", "video/mp4", reinterpret_cast<const char *>(mp4), mp4_size, video_name,
                                   sizeof(video_name)),
              "store an mp4");
        Check(satori::TempStorePut("song.mp3", "audio/mpeg", reinterpret_cast<const char *>(mp3), mp3_size, mp3_name,
                                   sizeof(mp3_name)),
              "store an mp3");
        Check(satori::TempStorePut("movie.mkv", "video/x-matroska", reinterpret_cast<const char *>(mkv), sizeof(mkv),
                                   mkv_name, sizeof(mkv_name)),
              "store an mkv");
        Check(satori::TempStorePut("报告.pdf", "application/pdf", "%PDF-1.4 fake", 13, pdf_name, sizeof(pdf_name)),
              "store a pdf");
        Check(satori::TempStorePut("poster.jpg", "image/jpeg", jpeg, sizeof(jpeg) - 1, jpeg_name2, sizeof(jpeg_name2)),
              "store a poster");
        Check(satori::TempStorePut("noext", "application/octet-stream", "\x01\x02\x03\x04 raw", 9, odd_name,
                                   sizeof(odd_name)),
              "store an unnamed blob");

        auto message_content = [](const Outcome &outcome, int index) -> const char * {
            const cJSON *item = cJSON_IsArray(outcome.body) ? cJSON_GetArrayItem(outcome.body, index) : nullptr;
            const cJSON *content = item ? cJSON_GetObjectItemCaseSensitive(item, "content") : nullptr;
            return cJSON_IsString(content) ? content->valuestring : "";
        };

        // A real MP4 goes out as a video, with its true play length, and comes back as a <video>.
        g_call_count = 0;
        g_status = 2;
        snprintf(content, sizeof(content), "<video src=\"internal:wechat/self_wxid/_tmp/%s\"/>", video_name);
        Outcome video = Create(content);
        Check(video.status == 200 && cJSON_GetArraySize(video.body) == 1, "a video is sent");
        Check(g_call_count == 1 && g_calls[0].kind == 'v' && g_calls[0].duration == 1 && !g_calls[0].poster[0],
              "an mp4 takes the video route with its duration");
        Check(strstr(message_content(video, 0), "<video ") != nullptr, "the reply describes a video");
        cJSON_Delete(video.body);

        // The poster is used when it is a picture, ignored when it is not.
        g_call_count = 0;
        snprintf(content, sizeof(content),
                 "<video src=\"internal:wechat/self_wxid/_tmp/%s\" poster=\"internal:wechat/self_wxid/_tmp/%s\"/>",
                 video_name, jpeg_name2);
        video = Create(content);
        Check(video.status == 200 && g_calls[0].kind == 'v' && g_calls[0].poster[0],
              "a picture poster is passed along");
        cJSON_Delete(video.body);
        g_call_count = 0;
        snprintf(content, sizeof(content),
                 "<video src=\"internal:wechat/self_wxid/_tmp/%s\" poster=\"internal:wechat/self_wxid/_tmp/%s\"/>",
                 video_name, text_name);
        video = Create(content);
        Check(video.status == 200 && g_calls[0].kind == 'v' && !g_calls[0].poster[0],
              "a poster that is not a picture is ignored");
        cJSON_Delete(video.body);

        // A video WeChat cannot play inline goes out as a file, and the reply says so.
        g_call_count = 0;
        snprintf(content, sizeof(content), "<video src=\"internal:wechat/self_wxid/_tmp/%s\"/>", mkv_name);
        Outcome fallback = Create(content);
        Check(fallback.status == 200 && g_calls[0].kind == 'f' && !strcmp(g_calls[0].title, "movie.mkv"),
              "a non-MP4 video is sent as a file under its own name");
        Check(strstr(message_content(fallback, 0), "<file ") != nullptr &&
                  strstr(message_content(fallback, 0), "movie.mkv"),
              "the reply says it was sent as a file");
        cJSON_Delete(fallback.body);

        // Audio that WeChat cannot play as a voice message is a file too.
        g_call_count = 0;
        g_voice_result = satori::VoicePrep::Failed;
        snprintf(content, sizeof(content), "<audio src=\"internal:wechat/self_wxid/_tmp/%s\"/>", mp3_name);
        Outcome audio = Create(content);
        Check(audio.status == 200 && g_calls[0].kind == 'f' && !strcmp(g_calls[0].title, "song.mp3"),
              "an mp3 is sent as a file under its own name");
        cJSON_Delete(audio.body);

        // Audio that converts becomes a voice message, with the length the conversion found.
        g_call_count = 0;
        g_voice_result = satori::VoicePrep::Ready;
        g_voice_ms = 2500;
        Outcome voice = Create(content);
        Check(voice.status == 200 && g_call_count == 1 && g_calls[0].kind == 'a' && g_calls[0].duration == 2500,
              "audio that converts is sent as a voice message with its length");
        Check(strstr(message_content(voice, 0), "<audio ") != nullptr &&
                  strstr(message_content(voice, 0), "duration=\"2.500\"") != nullptr,
              "and the reply is an <audio> with that duration");
        cJSON_Delete(voice.body);
        // A clip past WeChat's limit, or one that cannot be read, is a file (never lost).
        g_voice_result = satori::VoicePrep::TooLong;
        g_call_count = 0;
        Outcome long_clip = Create(content);
        Check(long_clip.status == 200 && g_calls[0].kind == 'f' && !strcmp(g_calls[0].title, "song.mp3"),
              "audio past the voice limit goes out as a file");
        cJSON_Delete(long_clip.body);
        g_voice_result = satori::VoicePrep::Failed;
        // ...and a voice message is text-mixable like any media element.
        g_voice_result = satori::VoicePrep::Ready;
        g_call_count = 0;
        snprintf(
            content, sizeof(content),
            "<audio src=\"internal:wechat/self_wxid/_tmp/%s\"/><file src=\"internal:wechat/self_wxid/_tmp/%s\" title=\"a.pdf\"/>",
            mp3_name, pdf_name);
        Outcome both = Create(content);
        Check(both.status == 200 && cJSON_GetArraySize(both.body) == 2 && g_calls[0].kind == 'a' &&
                  g_calls[1].kind == 'f',
              "a voice message and a file arrive as two messages in that order");
        cJSON_Delete(both.body);
        g_voice_result = satori::VoicePrep::Failed;
        snprintf(content, sizeof(content), "<audio src=\"internal:wechat/self_wxid/_tmp/%s\"/>", mp3_name);

        // Files: the title wins, then the upload's own name, then a name made from the content.
        g_call_count = 0;
        snprintf(content, sizeof(content), "<file src=\"internal:wechat/self_wxid/_tmp/%s\" title=\"季度 报告.pdf\"/>",
                 pdf_name);
        Outcome named = Create(content);
        Check(named.status == 200 && g_calls[0].kind == 'f' && !strcmp(g_calls[0].title, "季度 报告.pdf") &&
                  !strcmp(g_calls[0].talker, "filehelper"),
              "a file is sent under its title");
        Check(strstr(message_content(named, 0), "title=\"季度 报告.pdf\"") != nullptr, "the reply carries the title");
        cJSON_Delete(named.body);
        g_call_count = 0;
        snprintf(content, sizeof(content), "<file src=\"internal:wechat/self_wxid/_tmp/%s\"/>", pdf_name);
        Outcome untitled = Create(content);
        Check(untitled.status == 200 && !strcmp(g_calls[0].title, "报告.pdf"),
              "an untitled file keeps the name it was uploaded under (UTF-8 intact)");
        cJSON_Delete(untitled.body);
        g_call_count = 0;
        snprintf(content, sizeof(content), "<file src=\"internal:wechat/self_wxid/_tmp/%s\"/>", odd_name);
        Outcome nameless = Create(content);
        Check(nameless.status == 200 && !strcmp(g_calls[0].title, "noext"),
              "a name without an extension stays as it is when the bytes say nothing");
        cJSON_Delete(nameless.body);
        g_call_count = 0;
        snprintf(content, sizeof(content),
                 "<file src=\"internal:wechat/self_wxid/_tmp/%s\" title=\"../../etc/pass/wd.pdf\"/>", pdf_name);
        Outcome sneaky = Create(content);
        Check(sneaky.status == 200 && !strchr(g_calls[0].title, '/') && strstr(g_calls[0].title, "wd.pdf"),
              "path separators never reach WeChat's file name");
        cJSON_Delete(sneaky.body);
        // Inline sources: a data: URI or base64://, named after what they turn out to be.
        g_call_count = 0;
        Outcome inline_file = Create("<file src=\"data:application/pdf;base64,JVBERi0xLjQgZmFrZQ==\"/>");
        Check(inline_file.status == 200 && !strcmp(g_calls[0].title, "file.pdf"),
              "an inline file is named from its bytes");
        cJSON_Delete(inline_file.body);
        g_call_count = 0;
        inline_file = Create("<file src=\"base64://JVBERi0xLjQgZmFrZQ==\" title=\"合同\"/>");
        Check(inline_file.status == 200 && !strcmp(g_calls[0].title, "合同.pdf"),
              "a title without an extension gets the sniffed one");
        cJSON_Delete(inline_file.body);

        // Order: every media element is its own message, in the order written.
        g_call_count = 0;
        snprintf(
            content, sizeof(content),
            "先看视频<video src=\"internal:wechat/self_wxid/_tmp/%s\"/>再看文件<file src=\"internal:wechat/self_wxid/_tmp/%s\" title=\"a.pdf\"/>",
            video_name, pdf_name);
        Outcome ordered = Create(content);
        Check(ordered.status == 502 && !strcmp(Code(ordered), "send_failed") && g_call_count == 0,
              "the text before a video goes first (and, with no JVM here, stops the request at the sender)");
        cJSON_Delete(ordered.body);
        snprintf(
            content, sizeof(content),
            "<video src=\"internal:wechat/self_wxid/_tmp/%s\"/><file src=\"internal:wechat/self_wxid/_tmp/%s\" title=\"a.pdf\"/>",
            video_name, pdf_name);
        ordered = Create(content);
        Check(ordered.status == 200 && cJSON_GetArraySize(ordered.body) == 2 && g_call_count == 2 &&
                  g_calls[0].kind == 'v' && g_calls[1].kind == 'f',
              "a video then a file arrive as two messages in that order");
        cJSON_Delete(ordered.body);

        // <message> is the container for "one message": each part goes through the pipeline of its
        // own and the replies come back in order, whatever the part holds.
        g_call_count = 0;
        snprintf(content, sizeof(content),
                 "<message><file src=\"internal:wechat/self_wxid/_tmp/%s\" title=\"a.pdf\"/></message>"
                 "<message> </message><message><video src=\"internal:wechat/self_wxid/_tmp/%s\"/></message>",
                 pdf_name, video_name);
        Outcome containers = Create(content);
        Check(containers.status == 200 && cJSON_GetArraySize(containers.body) == 2 && g_call_count == 2 &&
                  g_calls[0].kind == 'f' && g_calls[1].kind == 'v',
              "two <message> parts are two sends in order, a blank part is skipped");
        cJSON_Delete(containers.body);
        g_call_count = 0;
        snprintf(content, sizeof(content),
                 "<file src=\"internal:wechat/self_wxid/_tmp/%s\" title=\"a.pdf\"/><message/>"
                 "<file src=\"internal:wechat/self_wxid/_tmp/%s\" title=\"b.pdf\"/>",
                 pdf_name, pdf_name);
        Outcome separator = Create(content);
        Check(separator.status == 200 && cJSON_GetArraySize(separator.body) == 2 && g_call_count == 2 &&
                  !strcmp(g_calls[1].title, "b.pdf"),
              "a self-closing <message/> separates two messages");
        cJSON_Delete(separator.body);
        {
            // Merge forwarding: one card per <message forward>, built and checked before anything is sent.
            g_call_count = 0;
            g_forward_count = 0;
            Outcome card =
                Create("<message forward><message><author id=\"a1\" name=\"Alice\"/>hi</message>"
                       "<message><author id=\"known_wxid\"/>yo <at id=\"x\" name=\"X\"/></message></message>");
            Check(card.status == 200 && cJSON_GetArraySize(card.body) == 1 && g_forward_count == 1,
                  "a merged forward is one message");
            if (card.status == 200 && cJSON_GetArraySize(card.body) == 1) {
                const cJSON *message = cJSON_GetArrayItem(card.body, 0);
                const cJSON *id = cJSON_GetObjectItemCaseSensitive(message, "id");
                const cJSON *reply = cJSON_GetObjectItemCaseSensitive(message, "content");
                Check(cJSON_IsString(id) && !strcmp(id->valuestring, "9100"), "with the row's id");
                Check(cJSON_IsString(reply) && strstr(reply->valuestring, "<message forward>") == reply->valuestring &&
                          strstr(reply->valuestring, "name=\"Known Nick\"") &&
                          strstr(reply->valuestring, "avatar=\"https://wx.example/known\""),
                      "answered with the container, names and avatars filled in from the contacts");
            }
            Check(!strcmp(g_forward.talker, "filehelper") && !strcmp(g_forward.title, "Alice与Known Nick的聊天记录"),
                  "headline from two speakers");
            Check(strstr(g_forward.desc, "Alice: hi\nKnown Nick: yo @X") != nullptr, "preview lines");
            Check(g_forward.record &&
                      strstr(g_forward.record, "<sourceheadurl>https://wx.example/known</sourceheadurl>") &&
                      strstr(g_forward.record, "<datalist count=\"2\">"),
                  "the record carries both lines");
            cJSON_Delete(card.body);

            // No author: this account writes the line. In a group the headline is the group's.
            g_forward_count = 0;
            card = CreateIn("123@chatroom", "<message forward title=\"周报\"><message>only me</message></message>");
            Check(card.status == 200 && g_forward_count == 1 && !strcmp(g_forward.title, "周报"),
                  "the title extension is used");
            Check(g_forward.record && strstr(g_forward.record, "<sourcename>self_wxid</sourcename>"),
                  "an authorless line is this account's");
            cJSON_Delete(card.body);
            card = CreateIn("123@chatroom", "<message forward><message><author name=\"A\"/>x</message></message>");
            Check(card.status == 200 && !strcmp(g_forward.title, "群聊的聊天记录"), "a group gets the group headline");
            cJSON_Delete(card.body);

            // Text around a forward is sent separately, in order; the forward sits between.
            g_call_count = 0;
            g_forward_count = 0;
            snprintf(
                content, sizeof(content),
                "<file src=\"internal:wechat/self_wxid/_tmp/%s\" title=\"a.pdf\"/>"
                "<message forward><message>x</message></message><file src=\"internal:wechat/self_wxid/_tmp/%s\" title=\"b.pdf\"/>",
                pdf_name, pdf_name);
            card = Create(content);
            Check(card.status == 200 && cJSON_GetArraySize(card.body) == 3 && g_call_count == 2 && g_forward_count == 1,
                  "files around a forward: three messages");
            cJSON_Delete(card.body);

            // Embedding a message of the conversation by id.
            g_forward_count = 0;
            card =
                Create("<message forward><message id=\"77\"/><message><author name=\"B\"/>reply</message></message>");
            Check(card.status == 200 && g_forward_count == 1, "an embedded message");
            Check(g_forward.record && strstr(g_forward.record, "<datadesc>the quoted line</datadesc>") &&
                      strstr(g_forward.record, "<sourcename>Known Nick</sourcename>") &&
                      strstr(g_forward.record, "<srcMsgCreateTime>1790000000</srcMsgCreateTime>") &&
                      strstr(g_forward.record, "<fromnewmsgid>5555</fromnewmsgid>"),
                  "its text, author, time and server id come from the store");
            cJSON_Delete(card.body);
            g_forward_count = 0;
            card = Create("<message forward><message id=\"404\"/></message>");
            Check(card.status == 400 && !strcmp(Code(card), "forward_message_not_found") && g_forward_count == 0,
                  "an unknown message id");
            cJSON_Delete(card.body);
            card = Create("<message forward><message id=\"300\"/></message>");
            Check(card.status == 400 && !strcmp(Code(card), "forward_media_unsupported") && g_forward_count == 0,
                  "a picture cannot be embedded yet");
            cJSON_Delete(card.body);

            // Refused before anything is sent.
            g_call_count = 0;
            card = Create(
                "<file src=\"internal:wechat/self_wxid/_tmp/x\" title=\"a.pdf\"/><message forward><message><img src=\"x\"/></message></message>");
            Check(card.status == 400 && !strcmp(Code(card), "forward_media_unsupported") && g_call_count == 0 &&
                      g_forward_count == 0,
                  "a bad line fails the request before the earlier parts go out");
            cJSON_Delete(card.body);
            card = Create("<message forward></message>");
            Check(card.status == 400 && !strcmp(Code(card), "forward_empty") && g_forward_count == 0,
                  "an empty forward");
            cJSON_Delete(card.body);
            card = Create(
                "<message forward><message>a<message forward><message>b</message></message></message></message>");
            Check(card.status == 400 && !strcmp(Code(card), "forward_nested_unsupported"), "a forward inside a line");
            cJSON_Delete(card.body);

            // The row is found in the store when WeChat's pipeline does not hand its id back; a failing sender fails the request.
            g_forward_id = -1;
            card = Create("<message forward><message>x</message></message>");
            Check(card.status == 200 && cJSON_GetArraySize(card.body) == 1,
                  "a pipeline that hides the id is looked up in the store");
            cJSON_Delete(card.body);
            g_forward_id = 9100;
            g_status = 5;
            card = Create("<message forward><message>x</message></message>");
            Check(card.status == 502 && !strcmp(Code(card), "send_failed"), "WeChat's failed status fails the request");
            cJSON_Delete(card.body);
            g_status = 2;
            g_forward_ok = false;
            card = Create("<message forward><message>x</message></message>");
            Check(card.status == 502 && !strcmp(Code(card), "send_failed"), "a refused send fails the request");
            cJSON_Delete(card.body);
            g_forward_ok = true;

            // Forwarding one message by id is a different thing and stays refused.
            const Outcome quoted_forward = Create("<message id=\"9\" forward/>");
            Check(quoted_forward.status == 400 && !strcmp(Code(quoted_forward), "forward_unsupported"),
                  "forwarding one message by id is refused");
            cJSON_Delete(quoted_forward.body);
            const Outcome nothing = Create("<message> </message><message><at id=\"x\"/></message>");
            Check(nothing.status == 400, "containers with nothing to send are an empty message");
            cJSON_Delete(nothing.body);
        }
        {
            // A part's failure reports what the earlier parts already delivered.
            g_call_count = 0;
            g_status = 5;
            snprintf(content, sizeof(content),
                     "<message><file src=\"internal:wechat/self_wxid/_tmp/%s\" title=\"a.pdf\"/></message>", pdf_name);
            Outcome failing = Create(content);
            Check(failing.status == 502 && !strcmp(Code(failing), "upload_failed"), "a failing part fails the request");
            cJSON_Delete(failing.body);
            g_status = 2;
        }

        // A refused upload is a failure the caller hears about; a slow one is not.
        g_status = 5;
        snprintf(content, sizeof(content), "<file src=\"internal:wechat/self_wxid/_tmp/%s\" title=\"a.pdf\"/>",
                 pdf_name);
        Outcome failed = Create(content);
        Check(failed.status == 502 && !strcmp(Code(failed), "upload_failed"), "WeChat's failed status is reported");
        cJSON_Delete(failed.body);
        g_status = 1;
        Outcome slow = Create(content);
        Check(slow.status == 200, "an upload still running when the wait ends is not an error");
        cJSON_Delete(slow.body);
        g_status = 2;

        // Requests that can never work are refused before anything is sent.
        g_call_count = 0;
        Outcome remote = Create("<video src=\"https://example.invalid/a.mp4\"/>");
        Check(remote.status == 400 && !strcmp(Code(remote), "media_unresolved") && g_call_count == 0,
              "a remote video URL is refused with advice");
        cJSON_Delete(remote.body);
        Outcome missing = Create("<file title=\"x.txt\"/>");
        Check(missing.status == 400 && !strcmp(Code(missing), "media_unresolved"), "a file without a source");
        cJSON_Delete(missing.body);
        Outcome gone = Create("<audio src=\"internal:wechat/self_wxid/_tmp/does-not-exist.mp3\"/>");
        Check(gone.status == 400 && !strcmp(Code(gone), "media_unresolved"), "an expired audio link");
        cJSON_Delete(gone.body);
        Outcome not_a_picture = Create("<img src=\"internal:wechat/self_wxid/_tmp/"
                                       "x\"/>");
        Check(not_a_picture.status == 400, "a picture link that does not resolve is still refused");
        cJSON_Delete(not_a_picture.body);
        char many[2400] = {};
        for (int i = 0; i < 9; ++i) {
            char one[200];
            snprintf(one, sizeof(one), "<file src=\"internal:wechat/self_wxid/_tmp/%s\" title=\"f%d.pdf\"/>", pdf_name,
                     i);
            strncat(many, one, sizeof(many) - strlen(many) - 1);
        }
        g_call_count = 0;
        Outcome too_many = Create(many);
        Check(too_many.status == 400 && !strcmp(Code(too_many), "too_many_media") && g_call_count == 0,
              "more than eight media elements");
        cJSON_Delete(too_many.body);
        // Nothing gets out when one of several is bad.
        g_call_count = 0;
        snprintf(
            content, sizeof(content),
            "<file src=\"internal:wechat/self_wxid/_tmp/%s\" title=\"ok.pdf\"/><video src=\"https://example.invalid/a.mp4\"/>",
            pdf_name);
        Outcome half = Create(content);
        Check(half.status == 400 && g_call_count == 0,
              "a bad element stops the whole request before the good one is sent");
        cJSON_Delete(half.body);
        // A video posted as a data: URI takes the same route as an uploaded one.
        g_call_count = 0;
        char inline_video[20000];
        static char b64[16000];
        static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        size_t out = 0;
        for (size_t i = 0; i < mp4_size; i += 3) {
            const unsigned a = mp4[i], b = i + 1 < mp4_size ? mp4[i + 1] : 0, c = i + 2 < mp4_size ? mp4[i + 2] : 0;
            b64[out++] = alphabet[a >> 2];
            b64[out++] = alphabet[((a & 3) << 4) | (b >> 4)];
            b64[out++] = i + 1 < mp4_size ? alphabet[((b & 15) << 2) | (c >> 6)] : '=';
            b64[out++] = i + 2 < mp4_size ? alphabet[c & 63] : '=';
        }
        b64[out] = 0;
        snprintf(inline_video, sizeof(inline_video), "<video src=\"data:video/mp4;base64,%s\"/>", b64);
        Outcome inline_result = Create(inline_video);
        Check(inline_result.status == 200 && g_calls[0].kind == 'v' && g_calls[0].duration == 1,
              "an inline mp4 is a video too");
        cJSON_Delete(inline_result.body);
    }
    {
        char long_text[4200];
        memset(long_text, 'x', sizeof(long_text) - 1);
        long_text[sizeof(long_text) - 1] = 0;
        const Outcome outcome = Create(long_text);
        Check(outcome.status == 400 && !strcmp(Code(outcome), "content_too_long"),
              "an over-long text run is refused with a code");
        cJSON_Delete(outcome.body);
    }

    // In a group a mention is content: "@name" goes out as text with WeChat's own mention list,
    // so a message that is only a mention is not empty. In a private chat a mention means nothing
    // and drops like any other element.
    {
        const Outcome group = CreateIn("123@chatroom", "<at id=\"wxid_a\" name=\"甲\"/>");
        Check(group.status == 502 && !strcmp(Code(group), "send_failed"),
              "a mention-only message in a group reaches the sender");
        cJSON_Delete(group.body);
        const Outcome direct = CreateIn("wxid_a", "<at id=\"wxid_a\" name=\"甲\"/>");
        Check(direct.status == 400, "a mention-only message in a private chat is empty");
        cJSON_Delete(direct.body);
    }

    // Whitespace-only text is empty, not a message.
    {
        const Outcome outcome = Create("   \n\t ");
        Check(outcome.status == 400, "blank content is refused");
        cJSON_Delete(outcome.body);
    }

    // A request that is neither text nor a picture keeps the plain 400.
    {
        const Outcome outcome = Create("");
        Check(outcome.status == 400 && !strcmp(Code(outcome), ""), "empty content has no error code");
        cJSON_Delete(outcome.body);
    }

    char cleanup[600];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", directory);
    if (system(cleanup)) fprintf(stderr, "warning: could not remove %s\n", directory);

    if (failures) {
        fprintf(stderr, "%d backend test(s) failed\n", failures);
        return 1;
    }
    printf("backend tests: PASS\n");
    return 0;
}
