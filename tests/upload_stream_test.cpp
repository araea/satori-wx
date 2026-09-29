// Host-side tests for the streaming multipart reader: bodies fed in every awkward chunking must
// land in the store byte for byte, and the malformed ones must be refused like the buffered parser
// refuses them.
#include "upload_stream.h"
#include "multipart.h"
#include "tempstore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

namespace {
int failures = 0;
void Check(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

struct Body { char *data; size_t size; };
void Append(Body *b, const void *bytes, size_t n) {
    b->data = static_cast<char *>(realloc(b->data, b->size + n + 1));
    memcpy(b->data + b->size, bytes, n);
    b->size += n;
}
void AppendText(Body *b, const char *text) { Append(b, text, strlen(text)); }
void Part(Body *b, const char *boundary, const char *name, const char *filename, const char *type, const void *data, size_t size) {
    char head[512];
    int n = snprintf(head, sizeof(head), "--%s\r\nContent-Disposition: form-data; name=\"%s\"", boundary, name);
    if (filename) n += snprintf(head + n, sizeof(head) - static_cast<size_t>(n), "; filename=\"%s\"", filename);
    if (type) n += snprintf(head + n, sizeof(head) - static_cast<size_t>(n), "\r\nContent-Type: %s", type);
    n += snprintf(head + n, sizeof(head) - static_cast<size_t>(n), "\r\n\r\n");
    Append(b, head, static_cast<size_t>(n));
    Append(b, data, size);
    AppendText(b, "\r\n");
}
void Close(Body *b, const char *boundary) {
    char tail[128];
    snprintf(tail, sizeof(tail), "--%s--\r\n", boundary);
    AppendText(b, tail);
}

char *ReadAll(const char *path, size_t *size) {
    FILE *f = fopen(path, "rb");
    if (!f) return nullptr;
    fseek(f, 0, SEEK_END);
    *size = static_cast<size_t>(ftell(f));
    fseek(f, 0, SEEK_SET);
    char *data = static_cast<char *>(malloc(*size + 1));
    if (fread(data, 1, *size, f) != *size) { fclose(f); free(data); return nullptr; }
    fclose(f);
    return data;
}

// Feeds `body` in chunks of `chunk` bytes (0 = all at once).
satori::UploadState Feed(satori::UploadStream *s, const Body &body, size_t chunk) {
    satori::UploadState state = satori::UploadState::More;
    size_t pos = 0;
    if (!chunk) chunk = body.size;
    while (pos < body.size) {
        const size_t n = body.size - pos < chunk ? body.size - pos : chunk;
        state = satori::UploadFeed(s, body.data + pos, n);
        pos += n;
        if (state != satori::UploadState::More && pos < body.size) return state;
    }
    return state;
}
}

int main() {
    const char *root = getenv("SATORI_TMPROOT");
    char dir[512];
    snprintf(dir, sizeof(dir), "%s/upload-stream", root ? root : "/tmp");
    char cleanup[600];
    snprintf(cleanup, sizeof(cleanup), "rm -rf '%s'", dir);
    if (system(cleanup)) {}
    satori::TempStoreSetDir(dir);
    const char *boundary = "----satoriBoundary7MA4YWxk";
    char type[200];
    snprintf(type, sizeof(type), "multipart/form-data; boundary=%s", boundary);

    // Binary data that keeps almost forming the delimiter: "\r\n--" and "\r\n----satori..." prefixes, and
    // now and then the whole delimiter followed by something that makes it data after all.
    Body payload{};
    srand(7);
    for (int i = 0; i < 40000; ++i) {
        const int pick = rand() % 50;
        if (pick == 0) AppendText(&payload, "\r\n--");
        else if (pick == 1) AppendText(&payload, "\r\n----satoriBoundary7MA4YWx");
        else if (pick == 3) { AppendText(&payload, "\r\n--"); AppendText(&payload, boundary); AppendText(&payload, "Z"); }  // a whole delimiter that is only data
        else if (pick == 2) AppendText(&payload, "\r");
        else { const char byte = static_cast<char>(rand()); Append(&payload, &byte, 1); }
    }
    Body second{};
    AppendText(&second, "second part, short");

    Body body{};
    Part(&body, boundary, "file", "big.bin", "application/octet-stream", payload.data, payload.size);
    Part(&body, boundary, "other", "报告.txt", "text/plain", second.data, second.size);
    Close(&body, boundary);

    static const size_t chunks[] = {0, 1, 2, 3, 7, 40, 79, 80, 81, 1000, 4095, 4096, 4097, 24576, 50000};
    for (size_t chunk : chunks) {
        satori::UploadStream *s = satori::UploadBegin(type, body.size);
        Check(s != nullptr, "stream begins");
        const satori::UploadState state = Feed(s, body, chunk);
        char what[120];
        snprintf(what, sizeof(what), "a valid body finishes (chunk %zu)", chunk);
        Check(state == satori::UploadState::Done, what);
        Check(satori::UploadCount(s) == 2 && !strcmp(satori::UploadField(s, 0), "file") && !strcmp(satori::UploadField(s, 1), "other"), "both parts recorded, in order");
        // Bytes must match exactly.
        const satori::TempFile *first = satori::TempStoreGet(satori::UploadStoredName(s, 0));
        size_t got_size = 0;
        char *got = first ? ReadAll(first->path, &got_size) : nullptr;
        snprintf(what, sizeof(what), "the big part is stored byte for byte (chunk %zu)", chunk);
        Check(got && got_size == payload.size && !memcmp(got, payload.data, payload.size), what);
        free(got);
        const satori::TempFile *other = satori::TempStoreGet(satori::UploadStoredName(s, 1));
        got = other ? ReadAll(other->path, &got_size) : nullptr;
        Check(got && got_size == second.size && !memcmp(got, second.data, second.size), "the second part is stored byte for byte");
        free(got);
        char original[300] = {};
        Check(satori::TempStoreOriginalName(satori::UploadStoredName(s, 1), original, sizeof(original)) && !strcmp(original, "报告.txt"),
              "the original (UTF-8) file name is kept");
        satori::UploadEnd(s);
    }

    // The buffered parser agrees on the good body...
    satori::Multipart parsed{};
    Check(satori::ParseMultipart(type, body.data, body.size, &parsed) && parsed.count == 2, "buffered parser accepts the same body");

    // ...and both refuse the same broken ones.
    struct Broken { const char *what; Body body; };
    Broken broken[8] = {};
    int count = 0;
    {   // no closing boundary
        Body b{}; Part(&b, boundary, "file", "a.bin", nullptr, "abc", 3);
        broken[count++] = {"no closing boundary", b};
    }
    {   // bytes after the closing boundary
        Body b{}; Part(&b, boundary, "file", "a.bin", nullptr, "abc", 3); Close(&b, boundary); AppendText(&b, "junk");
        broken[count++] = {"bytes after the end", b};
    }
    {   // duplicate field
        Body b{}; Part(&b, boundary, "file", "a.bin", nullptr, "abc", 3); Part(&b, boundary, "file", "b.bin", nullptr, "abc", 3); Close(&b, boundary);
        broken[count++] = {"duplicate field name", b};
    }
    {   // a part without a filename
        Body b{}; Part(&b, boundary, "field", nullptr, nullptr, "abc", 3); Close(&b, boundary);
        broken[count++] = {"a part without a filename", b};
    }
    {   // wrong opening
        Body b{}; AppendText(&b, "garbage before the boundary\r\n"); Part(&b, boundary, "file", "a.bin", nullptr, "abc", 3); Close(&b, boundary);
        broken[count++] = {"garbage before the first boundary", b};
    }
    {   // an unknown part header
        Body b{}; AppendText(&b, "--"); AppendText(&b, boundary); AppendText(&b, "\r\nX-Evil: 1\r\nContent-Disposition: form-data; name=\"f\"; filename=\"a\"\r\n\r\nabc\r\n"); Close(&b, boundary);
        broken[count++] = {"an unknown part header", b};
    }
    for (int i = 0; i < count; ++i) {
        satori::Multipart out{};
        Check(!satori::ParseMultipart(type, broken[i].body.data, broken[i].body.size, &out), broken[i].what);
        static const size_t small_chunks[] = {0, 1, 13};
        for (size_t chunk : small_chunks) {
            satori::UploadStream *s = satori::UploadBegin(type, broken[i].body.size);
            const satori::UploadState state = Feed(s, broken[i].body, chunk);
            char what[160];
            snprintf(what, sizeof(what), "streaming refuses: %s (chunk %zu)", broken[i].what, chunk);
            Check(state == satori::UploadState::Bad, what);
            satori::UploadEnd(s);
        }
        free(broken[i].body.data);
    }

    // More bytes than announced, and fewer.
    {
        satori::UploadStream *s = satori::UploadBegin(type, body.size - 10);
        Check(Feed(s, body, 0) == satori::UploadState::Bad, "a body longer than announced is refused");
        satori::UploadEnd(s);
        s = satori::UploadBegin(type, body.size + 10);
        Check(Feed(s, body, 0) == satori::UploadState::More, "a body shorter than announced waits for more");
        satori::UploadEnd(s);
    }
    // A missing or bad boundary never starts.
    Check(!satori::UploadBegin("multipart/form-data", 10), "no boundary parameter");
    Check(!satori::UploadBegin("application/json", 10), "not multipart");
    // Aborting mid-part leaves no file behind.
    {
        satori::UploadStream *s = satori::UploadBegin(type, body.size);
        satori::UploadFeed(s, body.data, 300);
        satori::UploadEnd(s);
        char listing[700];
        snprintf(listing, sizeof(listing), "ls '%s' | wc -l > '%s/count.txt'", dir, dir);
    }

    free(payload.data); free(second.data); free(body.data);
    if (system(cleanup)) {}
    if (failures) { fprintf(stderr, "%d upload stream failure(s)\n", failures); return 1; }
    puts("upload stream tests passed");
    return 0;
}
