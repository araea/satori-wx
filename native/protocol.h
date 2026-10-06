#pragma once
#include <stddef.h>
#include <stdint.h>
#include "vendor/cjson/cJSON.h"

namespace satori {
struct Multipart;
// Upper bound (exclusive) for one serialized event or signal. Events live on the heap, so this
// only caps a single message; long WeChat texts and rich elements fit comfortably.
constexpr size_t kEventSize = 131072;
constexpr unsigned kHistory = 64;
struct Method {
    const char *name;
    const char *fields;
    bool upload;
};
extern const Method kMethods[];
extern const size_t kMethodCount;
const Method *FindMethod(const char *name);
bool ValidateParams(const Method &method, const cJSON *body);
cJSON *Json(const char *data, size_t size);
bool Utf8(const char *data, size_t size);
// Structured content helpers preserve Satori markup instead of stripping it.
bool EscapeText(const char *text, char *out, size_t capacity);
// Flattens a Satori `content` string for a text-only adapter: keeps escaped text,
// turns <br/> into a newline, and drops elements it cannot carry (quote, at, emoji,
// img, audio, video, file, forward...). Always NUL-terminates; returns bytes written.
size_t PlainText(const char *content, char *out, size_t capacity);
// A mention found in outgoing content: WeChat wants the visible "@name" in the text and the
// account ids in a separate list.
struct OutgoingMention {
    char id[96];
    char name[96];
};
// Supplies a display name for an <at id="..."/> that carries none; may leave `name` empty.
using MentionNamer = bool (*)(void *context, const char *id, char *name, size_t capacity);
// PlainText for a sender that can mention: each <at id="x" name="y"/> becomes "@y" followed by
// U+2005 in `out` (what WeChat's own picker inserts) and is appended to `mentions`; <at
// type="all"/> is "@所有人" with the id "notify@all". `<a href>` keeps its target after the text.
// With `mentions` null it is exactly PlainText.
size_t OutgoingText(const char *content, char *out, size_t capacity, OutgoingMention *mentions, size_t max_mentions,
                    size_t *mention_count, MentionNamer namer, void *context);
// Image element sources in a Satori `content` string, in order. Only <img> is collected:
// its `src` is the one element WeChat can actually deliver from a local file. Copies are
// NUL-terminated; returns how many were written (never more than `max`).
constexpr size_t kImageSrcMax = 512;
size_t ImageSources(const char *content, char (*out)[kImageSrcMax], size_t max);
// Where the <img> elements sit in `content`: [begin, end) of each whole tag, and its src. Used
// to send a message that mixes text and pictures as the sequence of messages WeChat can carry,
// in the order the author wrote them. `src` is not limited to kImageSrcMax here: a data: URI
// carrying the picture itself may be megabytes, so it is copied into a buffer of `src_capacity`
// bytes supplied by the caller, and `src_size` says how long the attribute really was.
struct ImageSpan {
    size_t begin, end;
};
size_t ImageSpans(const char *content, ImageSpan *out, size_t max);
// The same scan for every element WeChat carries as a message of its own: pictures, audio,
// video and files. `kind` is 'i' (img), 'a' (audio), 'v' (video) or 'f' (file). Closing tags
// are ignored, so `<file src=".."></file>` counts once.
struct MediaSpan {
    size_t begin, end;
    char kind;
};
size_t MediaSpans(const char *content, MediaSpan *out, size_t max);
// Where a content string divides into the messages it stands for: every <message> container,
// self-closing <message/> separator or </message> is a boundary, and each non-empty stretch
// between boundaries is one message. Content with no <message> tag is a single part. A `forward`
// message (attribute in any spelling) is a part of its own that is not a stretch of text: `kind` 'm'
// marks a merge-forward container, [begin, end) being the whole element with the <message>s nested
// in it; 'r' marks a self-closing forward of one message by id (`<message id="…" forward/>`), the
// whole tag. Ordinary parts have kind 0.
struct MessagePart {
    size_t begin, end;
    char kind;
};
size_t MessageParts(const char *content, MessagePart *out, size_t max);
// The <message> elements directly inside a merge-forward container [begin, end) (the whole element,
// as MessageParts reports it): each child's extent, and the stretch between its tags.
struct ForwardChild {
    size_t begin, end, inner_begin, inner_end;
    bool self_closing;
};
size_t ForwardChildren(const char *content, size_t begin, size_t end, ForwardChild *out, size_t max);
// The first element called `name` (case-insensitive, opening tags only) in `content`.
bool FirstTag(const char *content, const char *name, ImageSpan *out);
// The value of `name` inside the tag [begin, end) of `content`, entity-decoded, or false.
bool TagAttribute(const char *content, const ImageSpan &tag, const char *name, char *out, size_t capacity);
// RFC 4648 base64 (whitespace ignored, padding optional). Returns the decoded size, or -1 on a
// character outside the alphabet or when `capacity` is too small.
long Base64Decode(const char *in, size_t size, unsigned char *out, size_t capacity);

struct EventBus;
EventBus *CreateBus();
void DestroyBus(EventBus *bus);
int BusFd(EventBus *bus);
// Safe on a native producer thread. False means backpressure/oversize, never silent success.
bool Publish(EventBus *bus, const char *event_json, bool meta = false);
// Pops the oldest queued event as a malloc'd string the caller frees, or null when empty.
char *Take(EventBus *bus, bool *meta);
void DrainWake(EventBus *bus);

struct Hub;
Hub *CreateHub();
void DestroyHub(Hub *hub);
const cJSON *Meta(Hub *hub);
// The READY body: the meta snapshot plus this process's session identity under `satori_wx`
// (`session_id`, `sn`). A client that sees `session_id` change knows the server restarted and
// that its `sn` cursor belongs to an earlier life. Caller frees.
cJSON *ReadyBody(Hub *hub);
// Number of ONLINE logins currently in the hub snapshot. Updated by Apply; the message
// store waits for this so it does not publish before the login is known (events are dropped).
extern volatile int g_login_count;
// True once the listener is bound. The in-process keeper reports it in the notification so
// a port conflict is visible instead of showing a healthy-but-deaf service.
extern volatile bool g_server_ready;
// Open /v1/events WebSocket clients (not transient HTTP requests). The in-process keeper uses
// it the way satori-qq uses its connection count: to keep the Wi-Fi radio out of screen-off
// power save while a Satori client is actually attached, and to show it on the notification.
extern volatile int g_client_count;
const cJSON *FindLogin(Hub *hub, const char *platform, const char *user);
uint64_t Latest(Hub *hub);
bool CanResume(Hub *hub, uint64_t sn);
bool CanDeliver(Hub *hub, uint64_t cursor, uint64_t replay_until);
const char *NextEvent(Hub *hub, uint64_t *cursor, uint64_t replay_until);
// Mutates the snapshot, stamps sequence/time and returns an owned signal, or null on invalid input.
char *Apply(Hub *hub, const char *json, bool meta);
char *Envelope(int op, const cJSON *body);
// Extracts the "body" value from an {"op":N,"body":...} envelope as a malloc'd JSON string.
char *EnvelopeBody(const char *signal);

struct Request {
    const Method *method;
    const char *platform;
    const char *user;
    const cJSON *params; // borrowed; null for multipart
    const char *content_type;
    const char *raw;
    size_t size;
    const Multipart *uploads;
};
struct Response {
    int status;
    cJSON *body;
}; // Server takes ownership of body.
struct Backend {
    void *context;
    Response (*call)(void *context, const Request &request);
};
} // namespace satori
