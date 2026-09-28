#include "xml_lite.h"
#include <stdint.h>
#include <string.h>
#include <strings.h>

namespace satori {
namespace {
bool NameChar(char c) {
    return !(c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '/' || c == '>' || c == '=');
}
bool StartsWith(const char *p, const char *end, const char *literal) {
    const size_t n = strlen(literal);
    return static_cast<size_t>(end - p) >= n && !memcmp(p, literal, n);
}
const char *Find(const char *p, const char *end, const char *literal) {
    const size_t n = strlen(literal);
    if (static_cast<size_t>(end - p) < n) return nullptr;
    return static_cast<const char *>(memmem(p, static_cast<size_t>(end - p), literal, n));
}
// Index of the '>' that closes the tag starting at `p` ('<'), honouring quoted attribute values.
const char *TagEnd(const char *p, const char *end) {
    char quote = 0;
    for (++p; p < end; ++p) {
        if (quote) { if (*p == quote) quote = 0; continue; }
        if (*p == '"' || *p == '\'') { quote = *p; continue; }
        if (*p == '>') return p;
    }
    return nullptr;
}
void AppendUtf8(uint32_t code, char *out, size_t capacity, size_t *used) {
    char bytes[4];
    size_t n = 0;
    if (code < 0x80) bytes[n++] = static_cast<char>(code);
    else if (code < 0x800) { bytes[n++] = static_cast<char>(0xC0 | (code >> 6)); bytes[n++] = static_cast<char>(0x80 | (code & 0x3F)); }
    else if (code < 0x10000) {
        bytes[n++] = static_cast<char>(0xE0 | (code >> 12)); bytes[n++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F));
        bytes[n++] = static_cast<char>(0x80 | (code & 0x3F));
    } else if (code <= 0x10FFFF) {
        bytes[n++] = static_cast<char>(0xF0 | (code >> 18)); bytes[n++] = static_cast<char>(0x80 | ((code >> 12) & 0x3F));
        bytes[n++] = static_cast<char>(0x80 | ((code >> 6) & 0x3F)); bytes[n++] = static_cast<char>(0x80 | (code & 0x3F));
    } else return;
    if (*used + n >= capacity) return;
    memcpy(out + *used, bytes, n);
    *used += n;
}
// Decodes the text between `p` and `end` (no CDATA inside) into `out`.
void DecodeRun(const char *p, const char *end, char *out, size_t capacity, size_t *used) {
    while (p < end) {
        if (*p != '&') {
            if (*used + 1 >= capacity) return;
            out[(*used)++] = *p++;
            continue;
        }
        const char *semi = static_cast<const char *>(memchr(p, ';', static_cast<size_t>(end - p)));
        if (!semi || semi - p > 10) { if (*used + 1 >= capacity) return; out[(*used)++] = *p++; continue; }
        const size_t n = static_cast<size_t>(semi - p) - 1;  // characters between '&' and ';'
        const char *name = p + 1;
        char literal = 0;
        if (n == 3 && !memcmp(name, "amp", 3)) literal = '&';
        else if (n == 2 && !memcmp(name, "lt", 2)) literal = '<';
        else if (n == 2 && !memcmp(name, "gt", 2)) literal = '>';
        else if (n == 4 && !memcmp(name, "quot", 4)) literal = '"';
        else if (n == 4 && !memcmp(name, "apos", 4)) literal = '\'';
        if (literal) {
            if (*used + 1 >= capacity) return;
            out[(*used)++] = literal; p = semi + 1;
            continue;
        }
        if (n >= 2 && name[0] == '#') {
            uint32_t code = 0;
            bool ok = true;
            if (name[1] == 'x' || name[1] == 'X') {
                if (n < 3) ok = false;
                for (size_t i = 2; ok && i < n; ++i) {
                    const char h = name[i];
                    const int v = h >= '0' && h <= '9' ? h - '0' : h >= 'a' && h <= 'f' ? h - 'a' + 10 : h >= 'A' && h <= 'F' ? h - 'A' + 10 : -1;
                    if (v < 0) ok = false; else code = code * 16 + static_cast<uint32_t>(v);
                }
            } else {
                for (size_t i = 1; ok && i < n; ++i) {
                    if (name[i] < '0' || name[i] > '9') ok = false; else code = code * 10 + static_cast<uint32_t>(name[i] - '0');
                }
            }
            if (ok && code && (code < 0xD800 || code > 0xDFFF)) { AppendUtf8(code, out, capacity, used); p = semi + 1; continue; }
        }
        if (*used + 1 >= capacity) return;
        out[(*used)++] = *p++;
    }
}
} // namespace

XmlSlice XmlDocument(const char *xml) {
    XmlSlice slice;
    if (!xml) return slice;
    slice.begin = xml;
    slice.end = xml + strlen(xml);
    return slice;
}

bool XmlChild(XmlSlice scope, const char *name, XmlSlice *inner, XmlSlice *attrs) {
    if (!scope.Valid() || !name || !*name) return false;
    const size_t wanted = strlen(name);
    const char *p = scope.begin, *end = scope.end;
    int depth = 0;
    bool matching = false;
    const char *inner_begin = nullptr, *attrs_begin = nullptr, *attrs_end = nullptr;
    while (p < end) {
        p = static_cast<const char *>(memchr(p, '<', static_cast<size_t>(end - p)));
        if (!p) return false;
        if (StartsWith(p, end, "<!--")) { const char *close = Find(p + 4, end, "-->"); if (!close) return false; p = close + 3; continue; }
        if (StartsWith(p, end, "<![CDATA[")) { const char *close = Find(p + 9, end, "]]>"); if (!close) return false; p = close + 3; continue; }
        if (StartsWith(p, end, "<?")) { const char *close = Find(p + 2, end, "?>"); if (!close) return false; p = close + 2; continue; }
        if (StartsWith(p, end, "<!")) { const char *close = TagEnd(p, end); if (!close) return false; p = close + 1; continue; }
        if (StartsWith(p, end, "</")) {
            const char *close = memchr(p, '>', static_cast<size_t>(end - p)) ? static_cast<const char *>(memchr(p, '>', static_cast<size_t>(end - p))) : nullptr;
            if (!close) return false;
            if (depth > 0) --depth;
            if (depth == 0 && matching) {
                if (inner) { inner->begin = inner_begin; inner->end = p; }
                if (attrs) { attrs->begin = attrs_begin; attrs->end = attrs_end; }
                return true;
            }
            p = close + 1;
            continue;
        }
        const char *close = TagEnd(p, end);
        if (!close) return false;
        const char *tag = p + 1, *tag_name_end = tag;
        while (tag_name_end < close && NameChar(*tag_name_end)) ++tag_name_end;
        const bool self_closing = close > p + 1 && close[-1] == '/';
        if (depth == 0 && static_cast<size_t>(tag_name_end - tag) == wanted && !memcmp(tag, name, wanted)) {
            const char *a_end = self_closing ? close - 1 : close;
            if (self_closing) {
                if (inner) { inner->begin = inner->end = close + 1; }
                if (attrs) { attrs->begin = tag_name_end; attrs->end = a_end; }
                return true;
            }
            matching = true;
            inner_begin = close + 1;
            attrs_begin = tag_name_end; attrs_end = a_end;
        }
        if (!self_closing) ++depth;
        p = close + 1;
    }
    return false;
}

bool XmlPath(XmlSlice scope, const char *path, XmlSlice *inner, XmlSlice *attrs) {
    if (!scope.Valid() || !path || !*path) return false;
    XmlSlice current = scope, last_attrs;
    const char *p = path;
    while (*p) {
        const char *slash = strchr(p, '/');
        const size_t length = slash ? static_cast<size_t>(slash - p) : strlen(p);
        char name[64];
        if (!length || length >= sizeof(name)) return false;
        memcpy(name, p, length); name[length] = 0;
        XmlSlice next, next_attrs;
        if (!XmlChild(current, name, &next, &next_attrs)) return false;
        current = next; last_attrs = next_attrs;
        p += length + (slash ? 1 : 0);
    }
    if (inner) *inner = current;
    if (attrs) *attrs = last_attrs;
    return true;
}

size_t XmlText(XmlSlice slice, char *out, size_t capacity) {
    if (!out || !capacity) return 0;
    size_t used = 0;
    out[0] = 0;
    if (!slice.Valid()) return 0;
    const char *p = slice.begin, *end = slice.end;
    while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
    while (end > p && (end[-1] == ' ' || end[-1] == '\t' || end[-1] == '\r' || end[-1] == '\n')) --end;
    while (p < end) {
        if (StartsWith(p, end, "<![CDATA[")) {
            const char *close = Find(p + 9, end, "]]>");
            const char *stop = close ? close : end;
            const size_t n = static_cast<size_t>(stop - (p + 9));
            const size_t room = capacity - 1 - used;
            const size_t take = n < room ? n : room;
            memcpy(out + used, p + 9, take);
            used += take;
            p = close ? close + 3 : end;
            continue;
        }
        const char *next = Find(p, end, "<![CDATA[");
        DecodeRun(p, next ? next : end, out, capacity, &used);
        p = next ? next : end;
    }
    // Never leave half of a multi-byte character behind.
    while (used > 0) {
        size_t start = used - 1;
        while (start > 0 && (static_cast<unsigned char>(out[start]) & 0xC0) == 0x80) --start;
        const unsigned char lead = static_cast<unsigned char>(out[start]);
        const size_t need = lead < 0x80 ? 1 : lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
        if (used - start >= need) break;
        used = start;
    }
    out[used] = 0;
    return used;
}

bool XmlGetText(XmlSlice scope, const char *path, char *out, size_t capacity) {
    XmlSlice inner;
    if (out && capacity) out[0] = 0;
    if (!XmlPath(scope, path, &inner)) return false;
    XmlText(inner, out, capacity);
    return true;
}

bool XmlAttribute(XmlSlice attrs, const char *name, char *out, size_t capacity) {
    if (!attrs.Valid() || !name || !out || !capacity) return false;
    out[0] = 0;
    const size_t wanted = strlen(name);
    const char *p = attrs.begin, *end = attrs.end;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n' || *p == '/')) ++p;
        const char *key = p;
        while (p < end && NameChar(*p)) ++p;
        const size_t key_size = static_cast<size_t>(p - key);
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (p >= end || *p != '=') { if (key_size == 0) ++p; continue; }
        ++p;
        while (p < end && (*p == ' ' || *p == '\t')) ++p;
        if (p >= end) return false;
        const char delimiter = *p;
        const char *value, *value_end;
        if (delimiter == '"' || delimiter == '\'') {
            value = ++p;
            value_end = static_cast<const char *>(memchr(p, delimiter, static_cast<size_t>(end - p)));
            if (!value_end) value_end = end;
            p = value_end < end ? value_end + 1 : end;
        } else {
            value = p;
            while (p < end && *p != ' ' && *p != '\t' && *p != '/') ++p;
            value_end = p;
        }
        if (key_size == wanted && !strncasecmp(key, name, wanted)) {
            size_t used = 0;
            DecodeRun(value, value_end, out, capacity, &used);
            out[used] = 0;
            return true;
        }
    }
    return false;
}

bool XmlGetAttribute(XmlSlice scope, const char *path, const char *name, char *out, size_t capacity) {
    XmlSlice inner, attrs;
    if (out && capacity) out[0] = 0;
    if (!XmlPath(scope, path, &inner, &attrs)) return false;
    return XmlAttribute(attrs, name, out, capacity);
}
} // namespace satori
