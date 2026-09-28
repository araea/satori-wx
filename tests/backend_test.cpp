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
#include "wx_keepalive.h"
#include "protocol.h"
#include "tempstore.h"
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <string.h>

// ---- stubs for the WeChat-side collaborators (none of them run in this process) ----------
namespace satori {
// A non-null handle: the stubs below ignore it, and the backend refuses to send without a store.
Store *LiveStore() { return reinterpret_cast<Store *>(1); }
bool StoreFindSentImage(Store *, const char *, long long, long long *) { return false; }
int LiveLoginSn() { return 1; }
bool StartLiveStore(const char *, EventBus *, int) { return false; }
const char *StoreSelfId(Store *) { return "self_wxid"; }
long long StoreWatermark(Store *) { return 0; }
long long StorePoll(Store *, long long, int, bool (*)(void *, const char *), void *, bool *) { return 0; }
cJSON *StoreMessageList(Store *, const char *, const char *, const char *, int, const char *) { return nullptr; }
cJSON *StoreMessageGet(Store *, const char *, const char *) { return nullptr; }
cJSON *StoreUserGet(Store *, const char *) { return nullptr; }
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
} // namespace satori

namespace {
int failures = 0;

void Check(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
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
    const cJSON *error = outcome.body ? cJSON_GetObjectItemCaseSensitive(outcome.body, "error") : nullptr;
    return cJSON_IsString(error) ? error->valuestring : "";
}
} // namespace

int main() {
    // send=off: the method is not published, so it must not even be attempted.
    satori::SetSendEnabled(false);
    {
        const Outcome outcome = Create("你好");
        Check(outcome.status == 404, "send off returns 404 for message.create");
        cJSON_Delete(outcome.body);
    }

    satori::SetSendEnabled(true);

    // Text is flattened and handed to the sender. There is no JavaVM here, so the sender
    // refuses after the flattening step: that is the proof the text path was taken and not
    // some earlier refusal.
    {
        const Outcome outcome = Create("a &amp; b<br/>c <at id=\"7\"/><quote id=\"8\"/>");
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
    snprintf(scratch_template, sizeof(scratch_template), "%s/satori-backend-XXXXXX", scratch_root && *scratch_root ? scratch_root : ".");
    char *directory = mkdtemp(scratch_template);
    Check(directory != nullptr, "temp directory");
    satori::TempStoreSetDir(directory);
    const char jpeg[] = "\xFF\xD8\xFF\xE0\0\x10JFIF";
    char jpeg_name[160], text_name[160];
    Check(satori::TempStorePut("a.jpg", "image/jpeg", jpeg, sizeof(jpeg) - 1, jpeg_name, sizeof(jpeg_name)), "store a picture");
    Check(satori::TempStorePut("notes.txt", "text/plain", "just words", 10, text_name, sizeof(text_name)), "store a non-picture");
    char content[1024];
    {
        snprintf(content, sizeof(content), "<img src=\"internal:wechat/self_wxid/_tmp/%s\"/>", jpeg_name);
        const Outcome outcome = Create(content);
        Check(outcome.status == 502 && !strcmp(Code(outcome), "send_failed"), "a picture from upload.create reaches the sender");
        cJSON_Delete(outcome.body);
    }
    {
        // "Only a picture" is content, not an empty message.
        const Outcome outcome = Create("<img src=\"internal:wechat/self_wxid/_tmp/does-not-exist.png\"/>");
        Check(outcome.status == 400 && !strcmp(Code(outcome), "media_unresolved"), "a link that is not (or no longer) an upload is refused");
        cJSON_Delete(outcome.body);
    }
    {
        snprintf(content, sizeof(content), "<img src=\"internal:wechat/self_wxid/_tmp/%s\"/>", text_name);
        const Outcome outcome = Create(content);
        Check(outcome.status == 400 && !strcmp(Code(outcome), "media_unsupported"), "a file that is not a picture is refused");
        cJSON_Delete(outcome.body);
    }
    {
        const Outcome outcome = Create("<img src=\"https://example.invalid/a.png\"/>");
        Check(outcome.status == 400 && !strcmp(Code(outcome), "media_unresolved"), "remote URLs are refused with advice");
        cJSON_Delete(outcome.body);
    }
    {
        const Outcome outcome = Create("<img/>");
        Check(outcome.status == 400 && !strcmp(Code(outcome), "media_unresolved"), "an <img> without a source");
        cJSON_Delete(outcome.body);
    }
    {
        // 1x1 PNG, inline.
        const Outcome outcome = Create("<img src=\"data:image/png;base64,iVBORw0KGgoAAAANSUhEUgAAAAEAAAABCAYAAAAfFcSJAAAADUlEQVR4nGP4z8DwHwAFAAH/q842iQAAAABJRU5ErkJggg==\"/>");
        Check(outcome.status == 502 && !strcmp(Code(outcome), "send_failed"), "an inline picture is decoded and reaches the sender");
        cJSON_Delete(outcome.body);
        const Outcome not_image = Create("<img src=\"data:image/png;base64,aGVsbG8gd29ybGQ=\"/>");
        Check(not_image.status == 400 && !strcmp(Code(not_image), "media_unsupported"), "an inline blob that is not a picture");
        cJSON_Delete(not_image.body);
        const Outcome not_base64 = Create("<img src=\"data:image/png,rawbytes\"/>");
        Check(not_base64.status == 400 && !strcmp(Code(not_base64), "media_unresolved"), "only base64 data: URIs");
        cJSON_Delete(not_base64.body);
        const Outcome garbage = Create("<img src=\"data:image/png;base64,!!!!\"/>");
        Check(garbage.status == 400 && !strcmp(Code(garbage), "media_unresolved"), "undecodable data");
        cJSON_Delete(garbage.body);
        const Outcome wrong_type = Create("<img src=\"data:text/plain;base64,aGVsbG8=\"/>");
        Check(wrong_type.status == 400 && !strcmp(Code(wrong_type), "media_unsupported"), "a data: URI that is not an image type");
        cJSON_Delete(wrong_type.body);
    }
    {
        // Text and picture mixed: the request is validated as a whole. A bad picture stops the
        // text before it from going out, which shows in the error code (400, not 502).
        const Outcome outcome = Create("先说这个<img src=\"https://example.invalid/a.png\"/>再说那个");
        Check(outcome.status == 400 && !strcmp(Code(outcome), "media_unresolved"), "a bad picture stops the whole request before anything is sent");
        cJSON_Delete(outcome.body);
        snprintf(content, sizeof(content), "看图<img src=\"internal:wechat/self_wxid/_tmp/%s\"/>怎么样", jpeg_name);
        const Outcome mixed = Create(content);
        Check(mixed.status == 502 && !strcmp(Code(mixed), "send_failed"), "text before a good picture goes first (and reaches the sender)");
        cJSON_Delete(mixed.body);
        snprintf(content, sizeof(content), "<img src=\"internal:wechat/self_wxid/_tmp/%s\"/><img src=\"internal:wechat/self_wxid/_tmp/%s\"/><img src=\"internal:wechat/self_wxid/_tmp/%s\"/>"
                 "<img src=\"internal:wechat/self_wxid/_tmp/%s\"/><img src=\"internal:wechat/self_wxid/_tmp/%s\"/>", jpeg_name, jpeg_name, jpeg_name, jpeg_name, jpeg_name);
        const Outcome many = Create(content);
        Check(many.status == 400 && !strcmp(Code(many), "too_many_images"), "more than four pictures");
        cJSON_Delete(many.body);
    }
    {
        // Audio, video and files are still not carried, and the client can tell.
        const Outcome audio = Create("<audio src=\"internal:wechat/self_wxid/_tmp/a.mp3\"/>");
        Check(audio.status == 400 && !strcmp(Code(audio), "media_unsupported"), "audio alone is refused as unsupported");
        cJSON_Delete(audio.body);
        const Outcome with_text = Create("听<audio src=\"x\"/>这个");
        Check(with_text.status == 502 && !strcmp(Code(with_text), "send_failed"), "the text next to an unsupported element still goes");
        cJSON_Delete(with_text.body);
    }
    {
        char long_text[4200];
        memset(long_text, 'x', sizeof(long_text) - 1);
        long_text[sizeof(long_text) - 1] = 0;
        const Outcome outcome = Create(long_text);
        Check(outcome.status == 400 && !strcmp(Code(outcome), "content_too_long"), "an over-long text run is refused with a code");
        cJSON_Delete(outcome.body);
    }

    // In a group a mention is content: "@name" goes out as text with WeChat's own mention list,
    // so a message that is only a mention is not empty. In a private chat a mention means nothing
    // and drops like any other element.
    {
        const Outcome group = CreateIn("123@chatroom", "<at id=\"wxid_a\" name=\"甲\"/>");
        Check(group.status == 502 && !strcmp(Code(group), "send_failed"), "a mention-only message in a group reaches the sender");
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

    satori::SetSendEnabled(false);
    char cleanup[600];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", directory);
    if (system(cleanup)) fprintf(stderr, "warning: could not remove %s\n", directory);

    if (failures) { fprintf(stderr, "%d backend test(s) failed\n", failures); return 1; }
    printf("backend tests: PASS\n");
    return 0;
}
