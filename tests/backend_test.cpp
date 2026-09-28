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
#include <stdio.h>
#include <string.h>

// ---- stubs for the WeChat-side collaborators (none of them run in this process) ----------
namespace satori {
Store *LiveStore() { return nullptr; }
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

    // A picture alone is not sendable, and the client must be able to tell that apart from
    // "empty content" so it can stop retrying or fall back to text.
    {
        const Outcome outcome = Create("<img src=\"internal:wechat/self_wxid/_tmp/a.png\"/>");
        Check(outcome.status == 400 && !strcmp(Code(outcome), "media_unsupported"),
              "an image-only content is refused with media_unsupported");
        cJSON_Delete(outcome.body);
    }

    // Pictures mixed with text: the text still goes out (the element is dropped, not echoed).
    {
        const Outcome outcome = Create("<img src=\"internal:wechat/self_wxid/_tmp/a.png\"/>看图");
        Check(outcome.status == 502 && !strcmp(Code(outcome), "send_failed"),
              "text next to an image still reaches the sender");
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

    if (failures) { fprintf(stderr, "%d backend test(s) failed\n", failures); return 1; }
    printf("backend tests: PASS\n");
    return 0;
}
