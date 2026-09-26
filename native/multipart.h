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
}
