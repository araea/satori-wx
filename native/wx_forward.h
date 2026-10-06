#pragma once
#include <stddef.h>

// Merge forwarding: Satori's `<message forward>` holding other `<message>`s becomes WeChat's
// "聊天记录" card (an appmsg of type 19 whose <recorditem> carries the records). This file is the
// part that needs no WeChat: reading the container, and writing the card's title, preview and
// <recordinfo>. Names and avatars the caller left out are filled in by the backend (it has the
// contact store), the card is handed to WeChat by SendForward.
namespace satori {
constexpr size_t kForwardMax = 100;               // messages in one card (WeChat's own selection limit)
constexpr size_t kForwardText = 4000;             // bytes of one message's text (a chat message's limit)
constexpr size_t kForwardRecordMax = 256u * 1024; // bytes of <recordinfo> WeChat is asked to carry

struct ForwardEntry {
    char ref_id[24];         // `<message id="…"/>`: a message of this conversation to embed; "" when written out
    char author_id[96];      // `<author id>`; the backend fills in this account for an entry with none
    char author_name[128];   // `<author name>` (or the author element's text), else filled from the contacts
    char author_avatar[512]; // `<author avatar>` when an http(s) URL, else filled from the contacts
    char text[kForwardText + 1];
    long long created_s; // when the message was written (an embedded message's own time); 0 = now
    long long svr_id;    // an embedded message's server id, else 0
};
struct ForwardError {
    char code[40];
    char detail[160];
};

// Reads the entries of the merge-forward container [begin, end) of `content` (as MessageParts
// reports it). Returns how many (at least one), or -1 with `error` set. `title` (may be null)
// receives the container's non-standard `title` attribute, "" when it has none. A `<message>`
// with nothing in it is skipped, as Satori says it is not sent.
int ForwardParse(const char *content, size_t begin, size_t end, ForwardEntry *out, size_t max, char *title,
                 size_t title_capacity, ForwardError *error);

struct ForwardCard {
    char title[200]; // the card's headline
    char desc[400];  // its preview: the first few "name: text" lines
    char *record;    // <recordinfo> XML, malloc'd (the caller frees); null after a failure
};
// The card for `entries` (every name filled in). `group` says the conversation it goes to is a
// group. The headline is `title_override` when given, else what WeChat writes for a record of that
// many speakers ("群聊的聊天记录", "A与B的聊天记录"). `now_s` is the time of entries with none.
// False when the record would exceed kForwardRecordMax or memory ran out.
bool ForwardBuild(const ForwardEntry *entries, size_t count, const char *title_override, bool group, long long now_s,
                  ForwardCard *card, ForwardError *error);

// The Satori content of the sent message: the container again, with each entry's author and text
// (`title` is repeated when the caller gave one). malloc'd; null when memory ran out.
char *ForwardContent(const ForwardEntry *entries, size_t count, const char *title_override);
} // namespace satori
