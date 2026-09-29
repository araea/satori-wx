#include "upload_stream.h"
#include "multipart.h"
#include "tempstore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace satori {
namespace {
constexpr size_t kMaxParts = 16;
constexpr size_t kHeaderMax = 2048;
constexpr size_t kSlice = 4096;
enum Phase { kOpening, kHeaders, kBody, kEpilogue, kFinished };
}

struct UploadStream {
    char marker[76];          // "--boundary"
    size_t marker_size;
    char delimiter[80];       // "\r\n--boundary": what ends a part's data
    size_t delimiter_size;
    uint64_t remaining;       // announced body bytes not yet fed
    Phase phase;
    char head[kHeaderMax + 8];   // opening line, or one part's header block, or the two bytes after a delimiter
    size_t head_size;
    char work[kSlice + 80];   // carried bytes (a possible delimiter start) followed by the slice being scanned
    size_t carry_size;
    TempWriter *writer;
    Part part;                // headers of the part being read
    struct Done { char field[64]; char stored[128]; } done[kMaxParts];
    size_t done_count;
};

UploadStream *UploadBegin(const char *content_type, uint64_t body_size) {
    char boundary[71];
    if (!MultipartBoundary(content_type, boundary, sizeof(boundary))) return nullptr;
    auto *stream = static_cast<UploadStream *>(calloc(1, sizeof(UploadStream)));
    if (!stream) return nullptr;
    snprintf(stream->marker, sizeof(stream->marker), "--%s", boundary);
    stream->marker_size = strlen(stream->marker);
    snprintf(stream->delimiter, sizeof(stream->delimiter), "\r\n%s", stream->marker);
    stream->delimiter_size = strlen(stream->delimiter);
    stream->remaining = body_size;
    stream->phase = kOpening;
    return stream;
}

void UploadEnd(UploadStream *stream) {
    if (!stream) return;
    TempStoreAbort(stream->writer);
    free(stream);
}

size_t UploadCount(const UploadStream *stream) { return stream ? stream->done_count : 0; }
const char *UploadField(const UploadStream *stream, size_t index) { return stream && index < stream->done_count ? stream->done[index].field : ""; }
const char *UploadStoredName(const UploadStream *stream, size_t index) { return stream && index < stream->done_count ? stream->done[index].stored : ""; }

namespace {
// Writes `size` bytes of the current part.
bool Put(UploadStream *s, const char *data, size_t size) { return !size || TempStoreWrite(s->writer, data, size); }

// Consumes bytes into `head` until it holds `want` bytes; returns how many input bytes were used.
size_t Gather(UploadStream *s, const char *data, size_t size, size_t want) {
    const size_t take = want > s->head_size ? want - s->head_size : 0;
    const size_t n = take < size ? take : size;
    memcpy(s->head + s->head_size, data, n);
    s->head_size += n;
    return n;
}
} // namespace

UploadState UploadFeed(UploadStream *s, const char *data, size_t size) {
    if (!s) return UploadState::Bad;
    if (size > s->remaining) return UploadState::Bad;
    s->remaining -= size;
    while (size) {
        switch (s->phase) {
        case kOpening: {
            // "--boundary\r\n" must open the body.
            const size_t want = s->marker_size + 2;
            const size_t used = Gather(s, data, size, want);
            data += used; size -= used;
            if (s->head_size < want) break;
            if (memcmp(s->head, s->marker, s->marker_size) || memcmp(s->head + s->marker_size, "\r\n", 2)) return UploadState::Bad;
            s->head_size = 0;
            s->phase = kHeaders;
            break;
        }
        case kHeaders: {
            // Accumulate up to the blank line that ends the header block.
            while (size && s->head_size < kHeaderMax + 4) {
                s->head[s->head_size++] = *data++; --size;
                if (s->head_size >= 4 && !memcmp(s->head + s->head_size - 4, "\r\n\r\n", 4)) {
                    s->head[s->head_size - 2] = 0;   // keep the last line's CRLF, drop the blank line
                    if (memchr(s->head, 0, s->head_size - 2)) return UploadState::Bad;
                    if (s->done_count == kMaxParts || !ParsePartHeaders(s->head, &s->part)) return UploadState::Bad;
                    for (size_t i = 0; i < s->done_count; ++i) if (!strcmp(s->done[i].field, s->part.name)) return UploadState::Bad;
                    s->writer = TempStoreBegin(s->part.filename, s->part.content_type);
                    if (!s->writer) return UploadState::Failed;
                    s->head_size = 0;
                    s->carry_size = 0;
                    s->phase = kBody;
                    break;
                }
            }
            if (s->phase == kHeaders && s->head_size >= kHeaderMax + 4) return UploadState::Bad;
            break;
        }
        case kBody: {
            // Look for "\r\n--boundary" in carry + this slice. `work` holds the carried bytes (the
            // tail of what came before, which may be the start of a delimiter) followed by the new
            // slice; a match may straddle the seam. Like the buffered parser, a delimiter only
            // counts when "--" or CRLF follows it; anything else is ordinary data.
            const size_t slice = size < kSlice ? size : kSlice;
            memcpy(s->work + s->carry_size, data, slice);
            const size_t total = s->carry_size + slice;
            size_t scan = 0;
            for (;;) {
                const char *hit = total - scan >= s->delimiter_size
                    ? static_cast<const char *>(memmem(s->work + scan, total - scan, s->delimiter, s->delimiter_size)) : nullptr;
                if (!hit) {
                    // No complete delimiter: all but the last (delimiter - 1) bytes are data.
                    const size_t keep = s->delimiter_size - 1;
                    if (total > keep) {
                        if (!Put(s, s->work, total - keep)) return UploadState::Failed;
                        memmove(s->work, s->work + (total - keep), keep);
                        s->carry_size = keep;
                    } else {
                        s->carry_size = total;
                    }
                    data += slice; size -= slice;
                    break;
                }
                const size_t at = static_cast<size_t>(hit - s->work);
                const size_t after = total - at - s->delimiter_size;
                if (after < 2) {
                    // A whole delimiter but not yet the two bytes that say what it is: hold from it.
                    if (!Put(s, s->work, at)) return UploadState::Failed;
                    memmove(s->work, s->work + at, total - at);
                    s->carry_size = total - at;
                    data += slice; size -= slice;
                    break;
                }
                const char *follow = hit + s->delimiter_size;
                const bool closing = !memcmp(follow, "--", 2), next_part = !memcmp(follow, "\r\n", 2);
                if (!closing && !next_part) { scan = at + 1; continue; }   // a lookalike inside the data
                if (!Put(s, s->work, at)) return UploadState::Failed;
                char stored[128];
                const bool committed = TempStoreFinish(s->writer, stored, sizeof(stored));
                s->writer = nullptr;
                if (!committed) return UploadState::Failed;
                snprintf(s->done[s->done_count].field, sizeof(s->done[0].field), "%s", s->part.name);
                snprintf(s->done[s->done_count].stored, sizeof(s->done[0].stored), "%s", stored);
                ++s->done_count;
                s->head_size = 0;
                s->phase = closing ? kEpilogue : kHeaders;
                // Everything after the two bytes that followed the delimiter is still unread input.
                const size_t unread = after - 2;
                const size_t consumed = slice - unread;
                data += consumed; size -= consumed;
                s->carry_size = 0;
                break;
            }
            break;
        }
        case kEpilogue: {
            // Nothing but one optional CRLF may follow the closing boundary.
            if (s->head_size == 0 && size >= 1 && data[0] == '\r') { s->head[s->head_size++] = *data++; --size; break; }
            if (s->head_size == 1 && size >= 1 && data[0] == '\n') { ++data; --size; s->head_size = 2; s->phase = kFinished; break; }
            return UploadState::Bad;
        }
        case kFinished:
            return UploadState::Bad;
        }
    }
    if (s->remaining) return UploadState::More;
    // Everything announced has arrived: the body must have ended exactly at its closing boundary.
    const bool finished = s->phase == kFinished || (s->phase == kEpilogue && s->head_size == 0);
    return finished && s->done_count > 0 ? UploadState::Done : UploadState::Bad;
}
} // namespace satori
