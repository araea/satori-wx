#pragma once
#include <stddef.h>

// Reflection-based WeChat message sender.
//
// It calls WeChat's own send pipeline through the host class loader instead of patching
// any code: the app's `v51.r0` (NetSceneSendMsg) constructor inserts the message into the
// app's own database and `doScene()` dispatches it to the mars transport, exactly as the
// app does. Nothing is hooked, no ArtMethod is rewritten and no dex is loaded.
//
// The sender is opt-in. It is only usable after SendInit() (JavaVM from postAppSpecialize)
// and SendConfigure(); every target must be listed in the allow list. Delivery is
// asynchronous: a successful call means the scene was accepted for dispatch, not that the
// peer received anything.
namespace satori {
void SendInit(void *vm);
void SendConfigure(const char *allow_semicolon_list);
bool SendReady();
struct SendResult {
    bool ok;
    long long local_id; // WeChat local message id (msgId), or -1
    int net_id;         // dispatcher return; negative means the scene was rejected
    char detail[160];   // human-readable reason when ok is false
};
// Sends plain text to a talker (wxid or <id>@chatroom). Thread-safe, rate limited.
SendResult SendText(const char *talker, const char *content);
// Counters for diagnostics.
void SendStats(long long *sent, long long *failed);
} // namespace satori
