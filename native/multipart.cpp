#include "multipart.h"
#include <string.h>
#include <strings.h>
#include <stdio.h>
namespace satori {
namespace {
bool Parameter(const char *header, const char *key, char *out, size_t capacity) {
    const char *p = strchr(header, ';');
    bool found = false;
    while (p && *p) {
        ++p; while (*p == ' ' || *p == '\t') ++p;
        const char *name = p;
        while (*p && *p != '=' && *p != ';' && *p != ' ' && *p != '\t') ++p;
        const size_t name_size = p - name;
        while (*p == ' ' || *p == '\t') ++p;
        if (*p != '=') return false;
        ++p; while (*p == ' ' || *p == '\t') ++p;
        const bool match = strlen(key) == name_size && !strncasecmp(name, key, name_size);
        if (match && found) return false;
        char value[256]; size_t n = 0;
        if (*p == '"') {
            ++p;
            while (*p && *p != '"') {
                if (*p == '\\') ++p;
                if (!*p || static_cast<unsigned char>(*p) < 32 || n + 1 == sizeof(value)) return false;
                value[n++] = *p++;
            }
            if (*p++ != '"') return false;
            while (*p == ' ' || *p == '\t') ++p;
            if (*p && *p != ';') return false;
        } else {
            while (*p && *p != ';' && *p != ' ' && *p != '\t') {
                if (static_cast<unsigned char>(*p) < 32 || n + 1 == sizeof(value)) return false;
                value[n++] = *p++;
            }
            while (*p == ' ' || *p == '\t') ++p;
            if (*p && *p != ';') return false;
        }
        value[n] = 0;
        if (match) { if (n >= capacity) return false; memcpy(out, value, n + 1); found = true; }
        if (!*p) break;
    }
    return found;
}
}
bool MultipartBoundary(const char *type, char *boundary, size_t capacity) {
    if (capacity < 71 || strncasecmp(type, "multipart/form-data;", 20)) return false;
    if (!Parameter(type, "boundary", boundary, 71) || !*boundary) return false;
    for (const char *p = boundary; *p; ++p)
        if (!((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') || strchr("'()+_,-./:=? ", *p))) return false;
    return boundary[strlen(boundary) - 1] != ' ';
}
bool ParsePartHeaders(char *headers, Part *out) {
    Part part;
    memset(&part, 0, sizeof(part));
    strcpy(part.content_type, "application/octet-stream");
    bool disposition = false, content_type = false;
    for (char *p = headers; *p;) {
        char *end = strstr(p, "\r\n"); if (!end) return false; *end = 0;
        char *value = strchr(p, ':'); if (!value) return false; *value++ = 0;
        while (*value == ' ' || *value == '\t') ++value;
        for (const char *v = value; *v; ++v) if (static_cast<unsigned char>(*v) < 32) return false;
        if (!strcasecmp(p, "Content-Disposition")) {
            if (disposition || strncasecmp(value, "form-data;", 10) || !Parameter(value, "name", part.name, sizeof(part.name)) || !*part.name) return false;
            disposition = true;
            if (!Parameter(value, "filename", part.filename, sizeof(part.filename))) return false;
        } else if (!strcasecmp(p, "Content-Type")) {
            if (content_type || strlen(value) >= sizeof(part.content_type)) return false;
            content_type = true; strcpy(part.content_type, value);
        } else return false;
        p = end + 2;
    }
    if (!disposition) return false;
    *out = part;
    return true;
}
bool ParseMultipart(const char *type, const char *data, size_t size, Multipart *out) {
    out->count = 0;
    if (strncasecmp(type, "multipart/form-data;", 20)) return false;
    char boundary[71];
    if (!MultipartBoundary(type, boundary, sizeof(boundary))) return false;
    char marker[76]; snprintf(marker, sizeof(marker), "--%s", boundary);
    const size_t marker_size = strlen(marker);
    size_t pos = 0;
    while (true) {
        if (size - pos < marker_size + 2 || memcmp(data + pos, marker, marker_size)) return false;
        pos += marker_size;
        if (!memcmp(data + pos, "--", 2)) {
            pos += 2;
            if (size - pos >= 2 && !memcmp(data + pos, "\r\n", 2)) pos += 2;
            return out->count > 0 && pos == size;
        }
        if (memcmp(data + pos, "\r\n", 2) || out->count == 16) return false;
        pos += 2;
        const char *header_end = static_cast<const char *>(memmem(data + pos, size - pos, "\r\n\r\n", 4));
        if (!header_end || static_cast<size_t>(header_end - data - pos) > 2048) return false;
        const size_t hs = header_end - (data + pos);
        if (memchr(data + pos, 0, hs)) return false;
        char headers[2051]; memcpy(headers, data + pos, hs + 2); headers[hs + 2] = 0;
        Part &part = out->parts[out->count];
        if (!ParsePartHeaders(headers, &part)) return false;
        for (size_t i = 0; i < out->count; ++i) if (!strcmp(out->parts[i].name, part.name)) return false;
        pos = header_end - data + 4;
        char delimiter[78]; snprintf(delimiter, sizeof(delimiter), "\r\n%s", marker);
        const size_t ds = strlen(delimiter);
        const char *end = data + pos;
        for (;;) {
            end = static_cast<const char *>(memmem(end, size - (end - data), delimiter, ds));
            if (!end) return false;
            const size_t remaining = size - (end - data);
            if (remaining >= ds + 2 && (!memcmp(end + ds, "--", 2) || !memcmp(end + ds, "\r\n", 2))) break;
            ++end;
        }
        part.data = data + pos; part.size = end - part.data; ++out->count;
        pos = end - data + 2;
    }
}
}
