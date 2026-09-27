#pragma once
#include <stddef.h>
#include <stdint.h>
#include "vendor/cjson/cJSON.h"

namespace satori {
struct Multipart;
constexpr size_t kEventSize = 4096;
constexpr unsigned kHistory = 64;
struct Method { const char *name; const char *fields; bool upload; };
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

struct EventBus;
EventBus *CreateBus();
void DestroyBus(EventBus *bus);
int BusFd(EventBus *bus);
// Safe on a native producer thread. False means backpressure/oversize, never silent success.
bool Publish(EventBus *bus, const char *event_json, bool meta = false);
bool Take(EventBus *bus, char out[kEventSize], bool *meta);
void DrainWake(EventBus *bus);

struct Hub;
Hub *CreateHub();
void DestroyHub(Hub *hub);
const cJSON *Meta(Hub *hub);
// Number of ONLINE logins currently in the hub snapshot. Updated by Apply; the message
// store waits for this so it does not publish before the login is known (events are dropped).
extern volatile int g_login_count;
// True once the listener is bound. The in-process keeper reports it in the notification so
// a port conflict is visible instead of showing a healthy-but-deaf service.
extern volatile bool g_server_ready;
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
struct Response { int status; cJSON *body; }; // Server takes ownership of body.
struct Backend {
    void *context;
    Response (*call)(void *context, const Request &request);
};
} // namespace satori
