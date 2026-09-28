#pragma once
#include <stdlib.h>
#include <string.h>

// A small growable text buffer. The module is built without a C++ standard library, so this
// stands in for std::string where markup has to be assembled from pieces of unknown length.
// Allocation failure is sticky: once `failed` is set every later append is a no-op and Take()
// returns null, so callers can chain appends and check once at the end.
namespace satori {
struct TextBuf {
    char *data = nullptr;
    size_t size = 0, capacity = 0;
    bool failed = false;

    TextBuf() = default;
    TextBuf(const TextBuf &) = delete;
    TextBuf &operator=(const TextBuf &) = delete;
    ~TextBuf() { free(data); }

    bool Reserve(size_t extra) {
        if (failed) return false;
        if (extra > static_cast<size_t>(-1) - size - 1) { failed = true; return false; }
        const size_t need = size + extra + 1;
        if (need <= capacity) return true;
        size_t grown = capacity ? capacity : 256;
        while (grown < need) grown *= 2;
        char *bigger = static_cast<char *>(realloc(data, grown));
        if (!bigger) { failed = true; return false; }
        data = bigger; capacity = grown;
        return true;
    }
    void Append(const char *text, size_t length) {
        if (!length || !Reserve(length)) return;
        memcpy(data + size, text, length);
        size += length;
        data[size] = 0;
    }
    void Append(const char *text) { if (text) Append(text, strlen(text)); }
    void Append(char c) { Append(&c, 1); }
    // Text node: the three characters that would otherwise start markup.
    void Text(const char *text, size_t length) {
        for (size_t i = 0; i < length; ++i) {
            switch (text[i]) {
                case '&': Append("&amp;", 5); break;
                case '<': Append("&lt;", 4); break;
                case '>': Append("&gt;", 4); break;
                default: Append(text[i]); break;
            }
        }
    }
    void Text(const char *text) { if (text) Text(text, strlen(text)); }
    // Attribute value inside double quotes: additionally escapes the quote itself.
    void Attr(const char *text, size_t length) {
        for (size_t i = 0; i < length; ++i) {
            switch (text[i]) {
                case '&': Append("&amp;", 5); break;
                case '<': Append("&lt;", 4); break;
                case '>': Append("&gt;", 4); break;
                case '"': Append("&quot;", 6); break;
                default: Append(text[i]); break;
            }
        }
    }
    void Attr(const char *text) { if (text) Attr(text, strlen(text)); }
    // Hands the buffer to the caller as a NUL-terminated malloc'd string (never null unless an
    // allocation failed) and resets this object.
    char *Take() {
        if (failed) return nullptr;
        if (!data) { data = static_cast<char *>(malloc(1)); if (!data) return nullptr; data[0] = 0; }
        char *out = data;
        data = nullptr; size = capacity = 0;
        return out;
    }
};
} // namespace satori
