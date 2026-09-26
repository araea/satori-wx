#pragma once
#include <stddef.h>

// Satori WebHook delivery (optional, experimental).
//
// The application registers absolute http:// URLs through the authenticated
// /v1/meta/webhook.create API. Every EVENT/META signal this server produces is then
// POSTed to each registration with a Satori-Opcode header and an optional
// Authorization: Bearer token, exactly as the protocol standardises it.
//
// Deliveries happen on one owned thread with a bounded queue: the protocol event
// loop never blocks and never allocates on the delivery path. There is no TLS
// client, so https:// URLs are rejected at registration; this is documented.
namespace satori {
struct WebHooks;
struct WebHookCounters { unsigned long long sent, failed, dropped; };

WebHooks *CreateWebHooks();
void DestroyWebHooks(WebHooks *hooks);
// Replaces the token when the URL is already registered. False on invalid/oversize URL or limit reached.
bool AddWebHook(WebHooks *hooks, const char *url, const char *token);
bool RemoveWebHook(WebHooks *hooks, const char *url);
size_t WebHookCount(WebHooks *hooks);
// Non-blocking; body is the signal body (Event or Meta object). Drops and counts on queue overflow.
void PushWebHook(WebHooks *hooks, int opcode, const char *body);
void WebHookStats(WebHooks *hooks, WebHookCounters *counters);
} // namespace satori
