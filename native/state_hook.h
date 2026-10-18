#pragma once

// The one hook through which whoever changes a value the resident keeper shows tells it so.
// Kept apart from protocol.h so files that cannot take that header's names (wx_send.cpp has its own
// `Method`) can still call it.
namespace satori {
// Whoever keeps a view of this module's state (the resident keeper: notification, wake locks and
// the published state file) registers one callback here and is told, from the thread that made the
// change, whenever a value it reads has changed: the login set, the listener, a client attaching or
// detaching, the sender's readiness. It must be quick and must not block: it only records that
// something changed. Null (the default, and in every test) makes NotifyStateChanged() a no-op.
using StateListener = void (*)();
void SetStateListener(StateListener listener);
void NotifyStateChanged();
} // namespace satori
