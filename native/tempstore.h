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
bool TempStorePut(const char *filename, const char *content_type, const char *data, size_t size,
                  char *out, size_t capacity);
struct TempFile {
    const char *path;
    const char *content_type;
    size_t size;
};
// Looks up a stored name; null when missing or expired. The result is borrowed.
const TempFile *TempStoreGet(const char *name);
} // namespace satori
