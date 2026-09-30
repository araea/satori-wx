#pragma once
#include <stddef.h>
#include "wx_quote.h"

// Reflection-based WeChat message sender.
//
// It calls WeChat's own send pipeline through the host class loader instead of patching
// any code: the app's `v51.r0` (NetSceneSendMsg) constructor inserts the message into the
// app's own database and `doScene()` dispatches it to the mars transport, exactly as the
// app does. Nothing is hooked, no ArtMethod is rewritten and no dex is loaded.
//
// Only usable after SendInit() (JavaVM from postAppSpecialize). There is no on/off switch, no
// target whitelist and no rate limit: a client's message.create is dispatched immediately, like
// any other Satori implementation. Not connecting the client is how you stop it sending.
// Delivery is asynchronous: a successful call means the scene was accepted for dispatch, not
// that the peer received anything.
namespace satori {
void SendInit(void *vm);
bool SendReady();
struct SendResult {
    bool ok;
    bool rejected;      // refused before dispatch (bad request or not allowed, e.g. recalling someone else's message)
    long long local_id; // WeChat local message id (msgId), or -1
    int net_id;         // dispatcher return; negative means the scene was rejected
    char detail[160];   // human-readable reason when ok is false
};
// Sends plain text to a talker (wxid or <id>@chatroom). Thread-safe; no rate limit.
// `mention_ids` (optional, comma-separated wxids, "notify@all" for @everyone) makes the "@name"
// occurrences in `content` real mentions: it is WeChat's own <atuserlist> in the message source,
// passed the same way the chat UI passes it. Ignored when the send classes lack that overload.
SendResult SendText(const char *talker, const char *content, const char *mention_ids = nullptr);
// Sends a local image file (JPEG / PNG / GIF / WebP, readable by WeChat's own uid) to `talker`
// through the image pipeline the app itself uses for "send picture": the message-images feature
// service, whose rj() launches the whole prepare -> upload -> insert -> send coroutine and
// returns only a progress flow. That call is asynchronous, so `ok` means "handed to WeChat", not
// "a message exists": callers confirm with the database (StoreFindSentImage) before claiming
// anything. `self_id` is this account's wxid (the pipeline needs the sender).
SendResult SendImage(const char *talker, const char *self_id, const char *path);
// Sends a local file as a WeChat file message (the grey "file" bubble, type-6 appmsg) through the
// same code the chat UI reaches: WeChat's own AppMsgLogic inserts the row, tracks the attachment
// and starts its own upload. The row exists when this returns; the upload finishes on its own
// afterwards (poll the row's status: 1 sending, 2 sent, 5 failed). `title` is the file name the
// recipient sees. The file is hard-linked (or copied) into WeChat's attachment directory first, so
// the caller may delete its own copy as soon as this returns.
SendResult SendFile(const char *talker, const char *path, const char *title);
// Sends a local video (H.264/AAC MP4) as a WeChat video message: the playable bubble, not a file.
// It hands the file to WeChat's video-send feature service (the one behind "send video" in the
// chat UI), which extracts the poster, compresses when WeChat thinks it should, uploads and sends.
// Asynchronous: `ok` means the task was launched; the row (type 43) shows up in the message table
// a moment later. `thumb_path` is an optional JPEG poster; null or empty lets WeChat pick a frame.
// `duration_s` is the play length in whole seconds (what the bubble shows); 0 when unknown.
// The video is hard-linked (or copied) to a staging name first, so it survives the caller's own
// temporary expiring while WeChat is still compressing and uploading it.
SendResult SendVideo(const char *talker, const char *path, const char *thumb_path, int duration_s);
// Sends `text` as a reply to `quote`: WeChat's "引用" message (appmsg type 57 with a <refermsg>),
// built the way the chat UI's own quote-reply is, through AppMsgLogic. The recipient sees the
// quoted line above the text. `mention_ids` (comma-separated wxids, may be null) makes the "@name"
// occurrences in a group reply real mentions. WeChat inserts the reply's row a moment after this
// returns and does not hand its id back: `local_id` is -1 (unless it did), and the caller finds the
// row with StoreFindSentQuote.
SendResult SendQuote(const char *talker, const char *text, const QuoteRef &quote, const char *mention_ids);
// Sends a merged-forward card (WeChat's "聊天记录": an appmsg of type 19) to `talker`. `record_info` is the
// <recordinfo> XML the card carries (ForwardBuild), `title` and `desc` its headline and preview. WeChat's
// AppMsgLogic inserts the row and sends it, as it does for a file; nothing is uploaded, the records travel
// inside the message. The row exists when this returns (`local_id`); poll its status like a file's.
SendResult SendForward(const char *talker, const char *title, const char *desc, const char *record_info);
// Sends a SILK voice file (WeChat's own voice format: "\x02#!SILK_V3" followed by length-prefixed 20 ms
// packets) as a voice message, through the code the recorder's "stop" and the forward-voice action
// share: WeChat's VoiceLogic registers the voice file, inserts the message row and hands it to its own
// uploader. `duration_ms` is the length shown on the bubble. The row exists when this returns; the
// upload finishes on its own (poll the row's status).
SendResult SendVoice(const char *talker, const char *silk_path, int duration_ms);
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
    bool ready;            // JavaVM is wired into the sender (it will attempt to resolve)
    bool resolved;         // WeChat send classes are cached on the host class loader
    bool dispatcher;       // the network dispatcher was reachable at the last probe
    long long sent;        // dispatched successfully
    long long failed;      // reached the send pipeline but failed
    long long rejected;    // refused before dispatch (bad request, or a message that is not ours to recall)
    long long recalled;    // revoke scenes accepted for dispatch
    long long media;       // pictures handed to the image pipeline
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
