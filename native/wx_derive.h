#pragma once
#include "wx_names.h"

#include <stddef.h>

// Derives a name set for a WeChat build from its APK: the on-device counterpart of tools/wxnames.py.
//
// WeChat is re-obfuscated on every release, so class names move. What stays is what the code says:
// log tags, cgi paths, the shape of constructors and methods, and who refers to whom. The rules start
// at those anchors, walk the relations to the classes around them, and read member names off the dex.
// The previous build table is used only to choose between candidates of the same shape, never to
// invent a name.
//
// A key that no rule can derive is carried over from the previous set: MsgInfoIsSendField is a field
// of a library class (not obfuscated), and ThreadLooper is a behaviour switch that the next build
// inherits (8.0.78 needs a prepared Looper, 8.0.79 is hurt by one; see wx_send.cpp EnsureLooper).
namespace satori {

// The longest value is the video cross-object constructor (about 120 characters).
constexpr size_t kWxValueSize = 192;

struct DerivedNames {
    char value[kWxKeyCount][kWxValueSize];
    // Why a key is empty, for the host tool; the module never reads it.
    char reason[kWxKeyCount][96];
    const char *Get(WxKey key) const { return value[key]; }
    // Every key has a value: a set with holes is not installed (see WxNamesInstall).
    bool Complete() const;
};

// Reads `apk_path` and fills `out` using the built-in set labelled `hints_label` (null: the newest).
// Returns false and fills `error` when the APK cannot be read at all.
bool DeriveNames(const char *apk_path, const char *hints_label, DerivedNames *out, char *error, size_t size);

} // namespace satori
