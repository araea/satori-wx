// One-off inspection tool: run a read-only SELECT over WeChat's database.
// Usage: wxq <libWCDB.so> <db path> <key hex (raw bytes hex, "hex:" prefix)> <sql>
#include "wcdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
bool Row(satori::Wcdb *db, void *stmt, void *context) {
    (void)context;
    const int columns = satori::WcdbColumns(db, stmt);
    for (int i = 0; i < columns; ++i) {
        if (i) printf("\x1f");
        const char *text = satori::WcdbText(db, stmt, i);
        printf("%s", text ? text : "");
    }
    printf("\x1e\n");
    return true;
}
} // namespace

int main(int argc, char **argv) {
    if (argc != 5) {
        fprintf(stderr, "usage: %s <lib> <db> <keyhex|-> <sql>\n", argv[0]);
        return 2;
    }
    const char *lib = argv[1], *path = argv[2], *keyhex = argv[3], *sql = argv[4];
    satori::Wcdb *db = nullptr;
    if (!strcmp(keyhex, "-")) {
        db = satori::WcdbOpen(lib, path, nullptr, 0, 1);
    } else {
        if (strncmp(keyhex, "hex:", 4)) {
            fprintf(stderr, "key must be hex:...\n");
            return 2;
        }
        const char *hex = keyhex + 4;
        const size_t n = strlen(hex) / 2;
        unsigned char *key = static_cast<unsigned char *>(malloc(n ? n : 1));
        for (size_t i = 0; i < n; ++i) {
            unsigned byte = 0;
            if (sscanf(hex + 2 * i, "%2x", &byte) != 1) {
                fprintf(stderr, "bad hex\n");
                return 2;
            }
            key[i] = static_cast<unsigned char>(byte);
        }
        db = satori::WcdbOpenEx(lib, path, key, static_cast<int>(n), 1024, 1, 0, 1);
        free(key);
    }
    if (!db) {
        fprintf(stderr, "open failed: %s\n", satori::WcdbError(nullptr));
        return 1;
    }
    if (!satori::WcdbExec(db, "PRAGMA query_only=1")) fprintf(stderr, "query_only: %s\n", satori::WcdbError(db));
    if (!satori::WcdbQuery(db, sql, Row, nullptr)) {
        fprintf(stderr, "query failed: %s\n", satori::WcdbError(db));
        return 1;
    }
    satori::WcdbClose(db);
    return 0;
}
