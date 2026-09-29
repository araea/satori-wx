#pragma once
#include <stddef.h>
#include <stdint.h>
// Streaming multipart/form-data reader for `upload.create`: the request body is parsed as it
// arrives and every part is written straight to the temp store, so an upload's size is bounded
// by the disk and not by memory. It accepts exactly the shape the buffered parser accepts (one
// part per field, each with a filename, closing boundary, nothing after it) and rejects the
// same malformed bodies.
namespace satori {
struct UploadStream;
enum class UploadState { More, Done, Bad, Failed };
// `body_size` is the announced Content-Length. Null when the content type has no valid boundary.
UploadStream *UploadBegin(const char *content_type, uint64_t body_size);
// Feeds the next bytes of the body. `Done` once the closing boundary and the announced number
// of bytes have both been seen; `Bad` for a malformed or over-long body; `Failed` when the
// store could not take the data (disk full, no free slot). After Bad or Failed, stop feeding.
UploadState UploadFeed(UploadStream *stream, const char *data, size_t size);
// The finished parts, in order: the form field name, the stored name (see TempStorePut).
size_t UploadCount(const UploadStream *stream);
const char *UploadField(const UploadStream *stream, size_t index);
const char *UploadStoredName(const UploadStream *stream, size_t index);
// Frees the stream and deletes the partial file of a part that never finished. Null-safe.
void UploadEnd(UploadStream *stream);
} // namespace satori
