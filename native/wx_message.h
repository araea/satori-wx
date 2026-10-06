#pragma once
#include <stddef.h>

// Turns one row of WeChat's `message` table into Satori message content.
//
// This layer is pure: no database, no files, no clock. The store feeds it a row and gets back
// the author, the Satori content markup and, for a reply, what the row itself says about the
// quoted message; everything that needs a lookup (the quoted message's local id, display names,
// whether a media file exists) stays in the store, so the whole mapping is unit-testable from
// plain strings.
namespace satori {
enum class MsgKind {
    Text,
    Image,
    Voice,
    Video,
    Emoji,
    Location,
    Card,
    Link,
    File,
    Quote,
    Other,   // delivered as messages
    System,  // "xxx joined the group", pat-pat, red-packet tips...: bookkeeping, not chat
    Revoke,  // a message that was recalled (WeChat rewrites the original row in place)
    Ignored, // call logs, mail pushes and other rows no client can use
};

struct MessageRow {
    long long id = 0;     // msgId (the local id, also the rowid)
    long long svr_id = 0; // msgSvrId; 0 when absent
    int type = 0;
    int is_send = 0;
    long long create_time = 0; // milliseconds
    const char *talker = "";
    const char *content = "";
    const char *img_path = "";
    // The <msgsource> XML WeChat keeps in `lvbuffer` (it carries the @-mention list), or "".
    const char *msg_source = "";
};

struct Decoded {
    MsgKind kind = MsgKind::Ignored;
    bool deliver = false;    // true for the kinds listed first above
    char sender[96] = {};    // author wxid when the row says so ("" for a private chat: the talker)
    char *content = nullptr; // malloc'd Satori markup; set when `deliver`
    // Replies: the quoted message, as far as the row itself knows it.
    char refer_svr_id[24] = {};
    char refer_user[96] = {};
    char refer_name[128] = {};
    char *refer_text = nullptr; // malloc'd plain (unescaped) text of the quoted message
    Decoded() = default;
    Decoded(const Decoded &) = delete;
    Decoded &operator=(const Decoded &) = delete;
    ~Decoded();
    void Reset();
};

// Fills `out`; the previous contents are released first. `self_id` is the account's wxid: the
// login that media links are bound to. Without it media falls back to a text placeholder,
// because a link nobody can verify would only mislead.
void DecodeMessage(const MessageRow &row, const char *self_id, Decoded *out);

// Whether a `message.type` is one of the two rewrite-in-place markers WeChat leaves when a
// message is recalled ("xxx 撤回了一条消息" / "你撤回了一条消息").
bool IsRevokeType(int type);
// Whether a `message.type` is a system/bookkeeping row. Kept in one place so history queries,
// the poller and the decoder agree on what a "message" is.
bool IsSystemType(int type);
} // namespace satori
