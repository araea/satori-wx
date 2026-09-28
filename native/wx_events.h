#pragma once
#include "wx_store.h"

// Satori events that are not "a new row appeared": recalled messages, group membership, friends.
//
// WeChat announces none of these; they only show up as changes to state the store can read
// (a message row rewritten in place, a chatroom's member list, a contact's type). The scanner
// keeps a snapshot, notices the difference on its next pass and turns it into
// message-deleted / guild-member-added|removed / guild-added|removed / friend-added|removed.
// Two rules keep it honest: a difference must be seen on two consecutive passes before it is
// believed (WeChat rewrites these rows in several steps during a sync), and nothing is announced
// for the state that already existed when the scanner started.
namespace satori {
struct Scanner;
Scanner *CreateScanner(Store *store, int login_sn);
void DestroyScanner(Scanner *scanner);
// One pass; call it every few seconds. `now_ms` is wall-clock milliseconds. Events go to `emit`;
// one it refuses (the bus is full) is kept and offered again, in order, on the next pass.
void ScannerStep(Scanner *scanner, bool (*emit)(void *context, const char *event), void *context, long long now_ms);
// Events dropped because the retry queue overflowed.
long long ScannerDropped(Scanner *scanner);
} // namespace satori
