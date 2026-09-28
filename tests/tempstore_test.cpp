// Host-side tests for the upload store and the `internal:` link the media sender follows.
// No device, no Java, no cJSON: a real temp directory is used so TempStorePut/Get/Resolve
// run their actual path handling (this is what caught a sizeof() on a pointer instead of a
// string literal, which made every image send answer media_unavailable).
#include "tempstore.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {
int failures = 0;

void Check(bool ok, const char *what) {
    if (!ok) { fprintf(stderr, "FAIL: %s\n", what); ++failures; }
}

// Resolves `url` and compares either the resolution result or the stored file name.
void Link(const char *url, bool expect_ok, const char *expect_suffix, const char *what) {
    char out[1200] = {};
    const bool ok = satori::TempStoreResolveLink(url, out, sizeof(out));
    if (ok != expect_ok) {
        fprintf(stderr, "FAIL: %s\n  url: %s\n  got ok=%d want ok=%d\n", what, url, ok ? 1 : 0, expect_ok ? 1 : 0);
        ++failures;
        return;
    }
    if (ok && expect_suffix && !strstr(out, expect_suffix)) {
        fprintf(stderr, "FAIL: %s\n  url: %s\n  got: %s\n  want suffix: %s\n", what, url, out, expect_suffix);
        ++failures;
    }
}

bool ReadFile(const char *path, char *out, size_t capacity) {
    FILE *file = fopen(path, "rb");
    if (!file) return false;
    const size_t got = fread(out, 1, capacity - 1, file);
    out[got] = 0;
    fclose(file);
    return true;
}
} // namespace

int main() {
    // /tmp is not writable on this device; follow store_test and build the directory under
    // the caller's temp root (tests/run.sh points SATORI_ACCOUNT_TMP at build/tests/tmp).
    const char *base = getenv("SATORI_TMPROOT");
    if (!base || !*base) base = getenv("TMPDIR");
    if (!base || !*base) base = ".";
    char directory[512];
    snprintf(directory, sizeof(directory), "%s/satori-tempstore-XXXXXX", base);
    if (!mkdtemp(directory)) { fprintf(stderr, "mkdtemp failed\n"); return 2; }
    satori::TempStoreSetDir(directory);
    Check(satori::TempStoreAvailable(), "store directory is usable");

    char name[128] = {};
    Check(satori::TempStorePut("pic.png", "image/png", "hello", 5, name, sizeof(name)), "put");
    Check(name[0] && strstr(name, ".png"), "stored name keeps the extension");

    const satori::TempFile *file = satori::TempStoreGet(name);
    Check(file != nullptr, "get by name");
    char body[16] = {};
    Check(file && ReadFile(file->path, body, sizeof(body)) && !strcmp(body, "hello"), "stored bytes are readable");
    Check(file && !strcmp(file->content_type, "image/png"), "content type is kept");

    char link[256];
    snprintf(link, sizeof(link), "internal:wechat/wxid_self/_tmp/%s", name);
    Link(link, true, name, "the link upload.create hands out resolves");

    // Everything else must be refused rather than guessed at.
    Link("", false, nullptr, "empty url");
    Link("https://example.invalid/x.png", false, nullptr, "external url");
    Link("internal:wechat/wxid_self/_tmp/missing.png", false, nullptr, "unknown name");
    Link("internal:wechat/_tmp/noname.png", false, nullptr, "empty user segment");
    Link("internal:qq/wxid_self/_tmp/pic.png", false, nullptr, "other platform");
    Link("internal:wechat/wxid_self/pic.png", false, nullptr, "outside _tmp");
    Link("internal:wechat/wxid_self/_tmp/../pic.png", false, nullptr, "path escape");

    // A short output buffer fails instead of truncating the path.
    char small[4];
    char small_link[256];
    snprintf(small_link, sizeof(small_link), "internal:wechat/wxid_self/_tmp/%s", name);
    Check(!satori::TempStoreResolveLink(small_link, small, sizeof(small)), "short buffer is refused");

    // The store only knows names it wrote: a file placed in the directory by hand is not one.
    char stray[1200];
    snprintf(stray, sizeof(stray), "%s/stray.png", directory);
    FILE *hand = fopen(stray, "wb");
    if (hand) { fputs("x", hand); fclose(hand); }
    Link("internal:wechat/wxid_self/_tmp/stray.png", false, nullptr, "hand-placed file is not a link");

    unlink(stray);
    char kept[1200];
    snprintf(kept, sizeof(kept), "%s/%s", directory, name);
    unlink(kept);
    rmdir(directory);

    if (failures) { fprintf(stderr, "%d tempstore test(s) failed\n", failures); return 1; }
    printf("tempstore tests: PASS\n");
    return 0;
}
