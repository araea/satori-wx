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
    bool rejected;      // refused before dispatch by configuration, allow list or pacing
    long long local_id; // WeChat local message id (msgId), or -1
    int net_id;         // dispatcher return; negative means the scene was rejected
    char detail[160];   // human-readable reason when ok is false
};
// Sends plain text to a talker (wxid or <id>@chatroom). Thread-safe, rate limited.
SendResult SendText(const char *talker, const char *content);

// Snapshot for /v1/internal/status and /v1/internal/capabilities. Counters are per process
// and reset on restart; "ready" only becomes true once the WeChat classes have resolved.
struct SendStatus {
    bool enabled;          // send=on with a valid configuration
    bool ready;            // WeChat send classes resolved on the host class loader
    bool allowed_any;      // allow list is non-empty (otherwise every target is refused)
    long long sent;        // dispatched successfully
    long long failed;      // reached the send pipeline but failed
    long long rejected;    // refused before dispatch (disabled / allow list / pacing)
    long long last_age_ms; // ms since the last attempt, or -1 when there was none
    bool last_ok;
    int last_net_id;
    long long last_local_id;
    char last_target[96];
    char last_error[160];
    char allow[512];
};
void SendStatusGet(SendStatus *status);
} // namespace satori
