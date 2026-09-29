#pragma once
#include <stddef.h>

// WeChat's "拍一戳" (pat): the double-tap on a member's avatar in a chat. Implemented with the
// same reflection-only approach as the sender and the room actions: WeChat's own pat manager
// inserts the local interaction row (nv3.l.nj) and the app's own NetSceneSendPat (qv3.b,
// cgi /cgi-bin/micromsg-bin/sendpat) is dispatched through WeChat's own network queue.
// Nothing is hooked and no dex is loaded.
//
// Like every other write action there is no switch, no whitelist and no rate limit. Success
// means the scene was accepted for dispatch, not that the server accepted the pat.
#include "wx_room.h"  // ActionResult

namespace satori {
// Pats `user` in the chat `channel` (a wxid for a private chat, or <id>@chatroom).
ActionResult PatSend(const char *channel, const char *user);
// The server-side provider shape (see server.h): a thin wrapper around PatSend.
bool PatDispatch(const char *channel, const char *user, bool *rejected, char *detail, size_t size);
} // namespace satori
