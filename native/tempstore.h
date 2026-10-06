#pragma once
#include <stddef.h>
#include <stdint.h>

namespace satori {
// Built-in `/v1/upload.create` backing store and the `internal:` target of `/v1/proxy`.
//
// Satori's resource best practice is: adapters that cannot upload to the platform fall back
// to the SDK's local implementation, which stores the bytes and hands back an
// `internal:{platform}/{user.id}/_tmp/{name}` link with a short TTL. The proxy route serves
// that link. Files live under one directory with 0600/0700 permissions and are purged lazily.
void TempStoreSetDir(const char *dir);
// Resolves the storage directory (explicit, then SATORI_TMPDIR, then TMPDIR) and creates it.
bool TempStoreAvailable();
// Writes `data` under a fresh unguessable name derived from `filename`, records the content
// type and a 5 minute expiry, and copies the stored name into `out`. False on any I/O error.
bool TempStorePut(const char *filename, const char *content_type, const char *data, size_t size, char *out,
                  size_t capacity);
// Streaming variant of TempStorePut for bodies that must not be held in memory (a video is
// tens or hundreds of MiB): begin, write any number of chunks, then finish (or abort). The
// writer reserves one of the store's slots for its whole life; nothing is visible to
// TempStoreGet until finish. `filename` is what the client called the file; it is kept (UTF-8,
// control characters stripped) so a later <file> without a title can still say what it was.
struct TempWriter;
TempWriter *TempStoreBegin(const char *filename, const char *content_type);
bool TempStoreWrite(TempWriter *writer, const char *data, size_t size);
// Commits the file and frees the writer; copies the stored name into `out`.
bool TempStoreFinish(TempWriter *writer, char *out, size_t capacity);
// Deletes the partial file and frees the writer. Null-safe.
void TempStoreAbort(TempWriter *writer);
// The client's original file name for a stored name (empty when unknown). False if not stored.
bool TempStoreOriginalName(const char *name, char *out, size_t capacity);
struct TempFile {
    const char *path;
    const char *content_type;
    size_t size;
};
// Looks up a stored name; null when missing or expired. The result is borrowed.
const TempFile *TempStoreGet(const char *name);
// Resolves a Satori resource link produced by an earlier TempStorePut
// (`internal:wechat/<user>/_tmp/<name>`) to the local file it points at.
// False for anything else: another platform, another internal route, or a name the store
// does not know (expired, never uploaded, or not from this process).
bool TempStoreResolveLink(const char *url, char *out, size_t capacity);
} // namespace satori
