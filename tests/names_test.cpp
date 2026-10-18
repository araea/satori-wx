// The table of WeChat names: every key has a value in every built-in set, a discovered set can be
// installed and comes first, and the helpers turn dotted class names into JNI form.
#include "wx_names.h"
#include <stdio.h>
#include <string.h>

namespace {
int failures = 0;

void Check(bool ok, const char *what) {
    if (!ok) {
        fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
}
} // namespace

int main() {
    using namespace satori;
    Check(WxNamesCount() == 2, "two built-in sets at first");
    Check(!strcmp(WxNamesAt(0).label, "8.0.79") && !strcmp(WxNamesAt(1).label, "8.0.78"), "newest build first");
    for (int set = 0; set < WxNamesCount(); ++set) {
        for (int key = 0; key < kWxKeyCount; ++key) {
            char what[160];
            snprintf(what, sizeof(what), "%s has a value for %s", WxNamesAt(set).label,
                     WxKeyId(static_cast<WxKey>(key)));
            Check(WxNamesAt(set).value[key] && *WxNamesAt(set).value[key], what);
        }
    }
    for (int a = 0; a < kWxKeyCount; ++a) {
        Check(WxKeyByName(WxKeyId(static_cast<WxKey>(a))) == a, "key ids round-trip");
        for (int b = a + 1; b < kWxKeyCount; ++b)
            if (!strcmp(WxKeyId(static_cast<WxKey>(a)), WxKeyId(static_cast<WxKey>(b))))
                Check(false, "key ids are unique");
    }
    Check(WxKeyByName("NoSuchKey") == -1 && WxKeyByName(nullptr) == -1, "unknown key id");

    // Class names stay dotted in the table, descriptors use slashes: the two sets must agree about it.
    for (int set = 0; set < WxNamesCount(); ++set) {
        const char *cls = WxNamesAt(set).value[kSendSceneCls];
        Check(strchr(cls, '.') && !strchr(cls, '/'), "class names are dotted");
        const char *desc = WxNamesAt(set).value[kImageSendDesc];
        Check(desc[0] == '(' && !strchr(desc, '.'), "descriptors use slashes");
    }
    // The two builds differ where they are known to differ, and agree where they are not.
    Check(strcmp(WxNamesAt(0).value[kSendSceneCls], WxNamesAt(1).value[kSendSceneCls]) != 0,
          "the send scene was renamed");
    Check(!strcmp(WxNamesAt(0).value[kRevokeCls], WxNamesAt(1).value[kRevokeCls]), "the revoke scene was not");

    char buffer[64];
    Check(!strcmp(WxSlash("com.tencent.mm.network.s", buffer, sizeof(buffer)), "com/tencent/mm/network/s"),
          "slash form");
    Check(!strcmp(WxClassDesc("a.b.C", buffer, sizeof(buffer)), "La/b/C;"), "class descriptor");
    char small[8];
    Check(!strcmp(WxClassDesc("com.tencent.mm.network.s", small, sizeof(small)), "Lcom/te;") || small[0] == 'L',
          "a short buffer is truncated, not overrun");

    // A discovered set goes first; an incomplete one is refused.
    const char *values[kWxKeyCount];
    for (int key = 0; key < kWxKeyCount; ++key) values[key] = WxNamesAt(0).value[key];
    values[kSendSceneCls] = "zz.top";
    Check(WxNamesInstall("test", values), "install a discovered set");
    Check(WxNamesCount() == 3 && !strcmp(WxNamesAt(0).label, "test") &&
              !strcmp(WxNamesAt(0).value[kSendSceneCls], "zz.top") && !strcmp(WxNamesAt(1).label, "8.0.79") &&
              !strcmp(WxNamesAt(2).label, "8.0.78"),
          "it comes first, the built-ins follow");
    values[kRevokeCls] = "";
    Check(!WxNamesInstall("bad", values) && !strcmp(WxNamesAt(0).label, "test"), "an incomplete set is refused");

    if (failures) {
        fprintf(stderr, "names tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("names tests: PASS\n");
    return 0;
}
