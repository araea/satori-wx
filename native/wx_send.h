#pragma once
#include <stddef.h>

// Reflection-based WeChat message sender.
//
// It calls WeChat's own send pipeline through the host class loader instead of patching
// any code: the app's `v51.r0` (NetSceneSendMsg) constructor inserts the message into the
// app's own database and `doScene()` dispatches it to the mars transport, exactly as the
// app does. Nothing is hooked, no ArtMethod is rewritten and no dex is loaded.
//
// The sender is opt-in: only usable after SendInit() (JavaVM from postAppSpecialize) and when
// the config says send=on. There is neither a target whitelist nor a rate limit: once on, a
// client's message.create is dispatched immediately, like any other Satori implementation.
// Delivery is asynchronous: a successful call means the scene was accepted for dispatch, not
// that the peer received anything.
namespace satori {
void SendInit(void *vm);
bool SendReady();
struct SendResult {
    bool ok;
    bool rejected;      // refused before dispatch because send is disabled
    long long local_id; // WeChat local message id (msgId), or -1
    int net_id;         // dispatcher return; negative means the scene was rejected
    char detail[160];   // human-readable reason when ok is false
};
// Sends plain text to a talker (wxid or <id>@chatroom). Thread-safe; no rate limit.
SendResult SendText(const char *talker, const char *content);
// Sends a local image file to a talker through WeChat's own NetSceneSendMsg, the same route
// the app takes when it forwards a picture: the scene is built with WeChat's media
// constructor (message type 42, or 66 for a GIF), which inserts the row and hands the local
// path to the image upload scene. `file` must be readable by WeChat's uid. Same opt-in
// switch, same "accepted for dispatch" semantics as SendText.
SendResult SendMedia(const char *talker, const char *file, bool gif);
// Recalls one of our own messages by its local id (the id message.create returned, decimal
// string). Uses WeChat's own NetSceneRevokeMsg scene. Refused for
// messages the account did not send.
SendResult SendRecall(const char *talker, const char *message_id);
// Resolves the WeChat send classes and probes the dispatcher ahead of the first send so the
// status block reports real capability instead of "not tried yet". Safe to call repeatedly;
// does nothing until the JavaVM is available. Returns true once the classes are resolved
// (the dispatcher may still be down right after login, hence SendDispatcherReady()).
bool SendWarmUp();
bool SendDispatcherReady();

// Snapshot for /v1/internal/status and /v1/internal/capabilities. Counters are per process
// and reset on restart.
struct SendStatus {
    bool enabled;          // send=on with a valid configuration
    bool ready;            // JavaVM is wired into the sender (it will attempt to resolve)
    bool resolved;         // WeChat send classes are cached on the host class loader
    bool dispatcher;       // the network dispatcher was reachable at the last probe
    long long sent;        // dispatched successfully
    long long failed;      // reached the send pipeline but failed
    long long rejected;    // refused before dispatch (send disabled)
    long long recalled;    // revoke scenes accepted for dispatch
    long long media;       // image scenes accepted for dispatch
    long long last_age_ms; // ms since the last attempt, or -1 when there was none
    bool last_ok;
    int last_net_id;
    long long last_local_id;
    char last_target[96];
    char last_error[160];
};
void SendStatusGet(SendStatus *status);

// ---- Reflection helpers shared with the room/contact actions (implemented in wx_send.cpp).
// The host class loader and the network dispatcher are resolved once for the whole module.
// `ReflectEnv` attaches the calling thread to the JavaVM; `ReflectLoad` returns a local class
// reference or null; `ReflectDispatchScene` calls scene.doScene(dispatcher, no-op callback).
void *ReflectEnv();
bool ReflectResolve(char *detail, size_t size);
void *ReflectLoad(const char *name);
void *ReflectCallback();
int ReflectDispatchScene(void *scene, void *do_scene, char *detail, size_t size);
} // namespace satori
