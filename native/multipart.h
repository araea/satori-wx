#pragma once
#include <stddef.h>
namespace satori {
struct Part {
    char name[64], filename[256], content_type[128];
    const char *data;
    size_t size;
};
struct Multipart { Part parts[16]; size_t count; };
// Bounded, zero-copy binary multipart/form-data parser. Pointers borrow the HTTP body.
bool ParseMultipart(const char *content_type, const char *data, size_t size, Multipart *out);
// The pieces the streaming upload parser shares with it. `boundary` needs 71 bytes.
bool MultipartBoundary(const char *content_type, char *boundary, size_t capacity);
// Parses one part's header block (CRLF-terminated lines, NUL-terminated overall, at most 2 KiB):
// fills name / filename / content_type. False for anything but Content-Disposition (form-data
// with a name) and Content-Type, for duplicates, and for control characters.
bool ParsePartHeaders(char *headers, Part *part);
}
