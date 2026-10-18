// Host-side tests for the name discovery engine: the failure paths (no APK needed) and, when
// SATORI_WX_APK points at a WeChat base.apk, a full derivation checked against the built-in table.
#include "wx_derive.h"
#include "wx_names.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

using namespace satori;

static int failures = 0;

static void Check(bool ok, const char *what) {
    printf("%s - %s\n", ok ? "ok  " : "FAIL", what);
    if (!ok) ++failures;
}

static void WriteFile(const char *path, const void *data, size_t size) {
    FILE *file = fopen(path, "wb");
    if (!file) return;
    fwrite(data, 1, size, file);
    fclose(file);
}

int main() {
    const char *tmp = getenv("SATORI_TMPROOT");
    char root[400];
    snprintf(root, sizeof(root), "%s", tmp && *tmp ? tmp : "/tmp");
    char junk[400], zip[400];
    snprintf(junk, sizeof(junk), "%s/derive-junk.bin", root);
    snprintf(zip, sizeof(zip), "%s/derive-empty.zip", root);
    const char text[] = "this is not an apk";
    WriteFile(junk, text, sizeof(text));
    // An empty zip: the end of central directory record, no entries.
    const unsigned char empty_zip[22] = {0x50, 0x4b, 0x05, 0x06, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};
    WriteFile(zip, empty_zip, sizeof(empty_zip));

    DerivedNames *names = static_cast<DerivedNames *>(calloc(1, sizeof(DerivedNames)));
    char error[160] = {};
    Check(!DeriveNames("/nonexistent/base.apk", nullptr, names, error, sizeof(error)),
          "a missing APK is reported, not guessed");
    Check(error[0] != 0, "the failure says why");
    Check(!DeriveNames(junk, nullptr, names, error, sizeof(error)), "a non-zip file is refused");
    Check(!DeriveNames(zip, nullptr, names, error, sizeof(error)), "a zip without dex files is refused");
    Check(strstr(error, "no dex") != nullptr, "and says there are no dex files");

    const WxNames &newest = WxNamesNewestBuiltin();
    Check(newest.label && *newest.label, "the newest built-in set has a label");
    Check(WxNamesByLabel(newest.label) == &newest, "it can be looked up by label");
    Check(WxNamesByLabel("0.0.0") == nullptr, "an unknown label is null");

    const char *apk = getenv("SATORI_WX_APK");
    if (!apk || !*apk) {
        printf("skip - SATORI_WX_APK not set (no APK to derive from)\n");
    } else {
        memset(names, 0, sizeof(DerivedNames));
        error[0] = 0;
        const bool derived = DeriveNames(apk, nullptr, names, error, sizeof(error));
        Check(derived, "a real APK derives a set");
        if (derived) {
            Check(names->Complete(), "every key has a value");
            int different = 0;
            for (int i = 0; i < kWxKeyCount; ++i)
                if (strcmp(names->value[i], newest.value[i])) ++different;
            // ThreadLooper is carried from the newest set, so a derivation of that same build must
            // match it exactly.
            Check(different == 0, "the derived set equals the newest built-in set");
        } else {
            printf("     error: %s\n", error);
        }
    }
    free(names);
    remove(junk);
    remove(zip);
    printf("%s\n", failures ? "derive tests: FAIL" : "derive tests: PASS");
    return failures ? 1 : 0;
}
