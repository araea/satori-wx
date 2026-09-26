#pragma once
#include <stddef.h>

// Minimal, read-only SQLCipher/SQLite client used to read WeChat's own databases.
//
// WeChat ships libWCDB.so, which exports the whole SQLCipher C API. Instead of hooking
// WeChat's database code, the adapter dlopen()s that already-loaded library and uses it as
// an ordinary client on a second connection. Nothing in WeChat's code, ArtMethods or sqlite
// connections is modified, and only SELECTs are issued.
//
// The database key is obtained separately (see docs/wechat-store.md); it is only held in
// memory and never written to disk.
namespace satori {
struct Wcdb;
// One callback invocation per row. Return false to stop early; that is not an error.
typedef bool (*WcdbRow)(Wcdb *db, void *stmt, void *context);

// library: soname ("libWCDB.so") or an absolute path. key may be null for plain SQLite.
// read_only selects SQLITE_OPEN_READONLY; WAL readers may need read-write instead.
// Returns null on dlopen/symbol/open failure.
Wcdb *WcdbOpen(const char *library, const char *path, const void *key, int key_size, int read_only);
// Same, but configures the SQLCipher page size / compatibility version around the key.
Wcdb *WcdbOpenEx(const char *library, const char *path, const void *key, int key_size,
                 int page_size, int cipher_version, int pragmas_before_key, int read_only);
void WcdbClose(Wcdb *db);
// Runs one prepared statement over all rows.
bool WcdbQuery(Wcdb *db, const char *sql, WcdbRow callback, void *context);
// Runs a statement that returns no rows (DDL, PRAGMA).
bool WcdbExec(Wcdb *db, const char *sql);
// Last error text, never null.
const char *WcdbError(Wcdb *db);

int WcdbColumns(Wcdb *db, void *stmt);
const char *WcdbName(Wcdb *db, void *stmt, int column);
const char *WcdbText(Wcdb *db, void *stmt, int column);
long long WcdbInt(Wcdb *db, void *stmt, int column);
bool WcdbIsNull(Wcdb *db, void *stmt, int column);
} // namespace satori
