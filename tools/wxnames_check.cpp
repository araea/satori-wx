// Prints the name set the on-device engine derives from an APK, and optionally checks it against a
// built-in set. The same source the module runs; used to compare with tools/wxnames.py.
//
// usage: wxnames_check <base.apk> [--hints=8.0.79] [--check]
#include "wx_derive.h"
#include "wx_names.h"

#include <stdio.h>
#include <string.h>

using namespace satori;

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "usage: %s <base.apk> [--hints=LABEL] [--check]\n", argv[0]);
        return 2;
    }
    const char *hints = nullptr;
    const char *against = nullptr;
    bool check = false;
    for (int i = 2; i < argc; ++i) {
        if (!strncmp(argv[i], "--hints=", 8))
            hints = argv[i] + 8;
        else if (!strncmp(argv[i], "--against=", 10))
            against = argv[i] + 10;
        else if (!strcmp(argv[i], "--check"))
            check = true;
    }
    DerivedNames names{};
    char error[160] = {};
    if (!DeriveNames(argv[1], hints, &names, error, sizeof(error))) {
        fprintf(stderr, "error: %s\n", error);
        return 1;
    }
    const char *label = against ? against : hints;
    const WxNames *reference = check ? (label ? WxNamesByLabel(label) : &WxNamesNewestBuiltin()) : nullptr;
    int missing = 0;
    int mismatched = 0;
    for (int i = 0; i < kWxKeyCount; ++i) {
        const char *value = names.value[i];
        if (!value[0]) {
            printf("%-26s -   MISSING%s%s\n", WxKeyId(static_cast<WxKey>(i)), names.reason[i][0] ? " " : "",
                   names.reason[i]);
            ++missing;
            continue;
        }
        if (reference) {
            const bool same = !strcmp(value, reference->value[i]);
            if (!same) ++mismatched;
            printf("%-26s %-70s %s\n", WxKeyId(static_cast<WxKey>(i)), value, same ? "ok" : "MISMATCH");
        } else {
            printf("%-26s %s\n", WxKeyId(static_cast<WxKey>(i)), value);
        }
    }
    if (reference)
        printf("\n%s: %d missing, %d different from %s\n", names.Complete() ? "complete" : "incomplete", missing,
               mismatched, reference->label);
    return missing || mismatched ? 2 : 0;
}
