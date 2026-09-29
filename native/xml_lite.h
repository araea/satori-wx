#pragma once
#include <stddef.h>

// A deliberately small XML reader for WeChat's message bodies (<msg><appmsg>..., <sysmsg ...>).
//
// It is a scanner, not a parser: it never builds a tree and never allocates. It walks tags
// with a depth counter so a path like "msg/appmsg/refermsg/svrid" only ever matches *direct*
// children (the first <title> inside a quoted message is not the reply's own title), skips
// CDATA sections, comments and the <?xml?> prolog, and treats malformed input by simply not
// finding things. All entry points are bounded by the slice they are given.
namespace satori {
struct XmlSlice {
    const char *begin = nullptr, *end = nullptr;
    bool Valid() const { return begin != nullptr; }
    size_t Size() const { return static_cast<size_t>(end - begin); }
};
// The whole NUL-terminated document as a scope.
XmlSlice XmlDocument(const char *xml);
// First direct child element `name` of `scope`. `inner` receives the content between its start
// and end tag (empty for <name/>); `attrs`, when given, the text inside the start tag after the
// element name (what XmlAttribute reads).
bool XmlChild(XmlSlice scope, const char *name, XmlSlice *inner, XmlSlice *attrs = nullptr);
// The `index`-th (0-based) direct child element `name` of `scope`, for repeated children.
bool XmlChildAt(XmlSlice scope, const char *name, int index, XmlSlice *inner, XmlSlice *attrs = nullptr);
// Follows a '/'-separated chain of direct children from `scope`.
bool XmlPath(XmlSlice scope, const char *path, XmlSlice *inner, XmlSlice *attrs = nullptr);
// Element text: CDATA unwrapped, character references decoded, surrounding whitespace trimmed.
// Writes a NUL-terminated string, cutting on a UTF-8 boundary; returns the bytes written.
size_t XmlText(XmlSlice slice, char *out, size_t capacity);
// XmlPath + XmlText. False when the element does not exist (an empty element yields "").
bool XmlGetText(XmlSlice scope, const char *path, char *out, size_t capacity);
// Attribute `name` inside a start tag's attribute text; the value is entity-decoded.
bool XmlAttribute(XmlSlice attrs, const char *name, char *out, size_t capacity);
// XmlPath + XmlAttribute.
bool XmlGetAttribute(XmlSlice scope, const char *path, const char *name, char *out, size_t capacity);
} // namespace satori
