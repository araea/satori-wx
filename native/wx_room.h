#pragma once
#include <stddef.h>

// WeChat group (guild/channel) write actions, implemented with the same reflection-only
// approach as the sender: the app's own NetScene classes are constructed and dispatched
// through the app's own network queue. Nothing is hooked and no dex is loaded.
//
// Like the sender they are always available. A successful call means the scene
// was accepted for dispatch, not that the server applied it; the client gets a 200 and the
// async result is only visible in WeChat.
namespace satori {
struct ActionResult {
    bool ok;
    bool rejected;      // refused before dispatch
    char detail[160];   // reason when ok is false
};
// qn.p (delchatroommember) with one member: kick, or leave when user == self.
ActionResult RoomRemoveMember(const char *chatroom, const char *user);
// qn.b / qn.e (add/delchatroomadmin) through the Cgi runner.
ActionResult RoomSetAdmin(const char *chatroom, const char *user, bool enable);
} // namespace satori
