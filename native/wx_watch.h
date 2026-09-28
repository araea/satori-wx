#pragma once

// Change notices for the account database, so the live poller reacts to a new message within
// milliseconds instead of on a timer.
//
// The watch is on the directory that holds the database, not on the file: SQLite may delete and
// recreate the -wal file, and a watch on the file itself would silently die with it. Only writes
// to `EnMicroMsg.db` and `EnMicroMsg.db-wal` count; the other files in that directory are noise.
// Readers never depend on this for correctness: every caller keeps a timeout as a safety net.
namespace satori {
// Returns an inotify descriptor watching `database`'s directory, or -1 (no such directory,
// inotify unavailable). The caller owns it and closes it with close().
int WatchOpen(const char *database);
// Sleeps up to `timeout_ms` (negative counts as 0), or until a write to the database or its WAL is
// announced; true when one was. With fd < 0 it just sleeps and returns false. The notice queue is
// emptied on every wake, so a burst of writes counts as one.
bool WatchWait(int fd, long long timeout_ms);
} // namespace satori
