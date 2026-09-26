// Standalone read-only SQLCipher/SQLite reader used for research and diagnostics.
// It opens a database with WeChat's own libWCDB.so and dumps rows. It never writes.
//
// usage: satori-wx-wcdb <library> <db> [key] [sql]
//   key: "hex:<hex>" or "text:<text>" (default: no key)
//   sql: defaults to a bounded dump of the WeChat message table
#include "wcdb.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

namespace {
bool PrintRow(satori::Wcdb *db, void *stmt, void *context) {
    auto *header = static_cast<bool *>(context);
    const int columns = satori::WcdbColumns(db, stmt);
    if (!*header) {
        *header = true;
        for (int i = 0; i < columns; ++i) printf("%s%s", i ? " | " : "", satori::WcdbName(db, stmt, i));
        printf("\n");
    }
    for (int i = 0; i < columns; ++i) {
        if (i) printf(" | ");
        if (satori::WcdbIsNull(db, stmt, i)) printf("NULL");
        else printf("%s", satori::WcdbText(db, stmt, i));
    }
    printf("\n");
    return true;
}
} // namespace

int main(int argc, char **argv) {
    if (argc < 3) {
        fprintf(stderr, "usage: %s <library> <db> [key] [sql]\n", argv[0]);
        return 2;
    }
    const char *key = argc > 3 ? argv[3] : nullptr;
    char decoded[128];
    int key_size = 0;
    if (key && !strncmp(key, "hex:", 4)) {
        const char *hex = key + 4;
        const size_t length = strlen(hex);
        if (length % 2 || length / 2 >= sizeof(decoded)) { fprintf(stderr, "bad hex key\n"); return 2; }
        for (size_t i = 0; i < length; i += 2) {
            unsigned value = 0;
            if (sscanf(hex + i, "%2x", &value) != 1) { fprintf(stderr, "bad hex key\n"); return 2; }
            decoded[i / 2] = static_cast<char>(value);
        }
        key_size = static_cast<int>(length / 2);
    } else if (key && !strncmp(key, "text:", 5)) {
        key = key + 5;
        key_size = static_cast<int>(strlen(key));
        if (key_size >= static_cast<int>(sizeof(decoded))) { fprintf(stderr, "key too long\n"); return 2; }
        memcpy(decoded, key, static_cast<size_t>(key_size));
    } else if (key) {
        key_size = static_cast<int>(strlen(key));
        if (key_size >= static_cast<int>(sizeof(decoded))) { fprintf(stderr, "key too long\n"); return 2; }
        memcpy(decoded, key, static_cast<size_t>(key_size));
    }
    satori::Wcdb *db = satori::WcdbOpen(argv[1], argv[2], key_size ? decoded : nullptr, key_size, 1);
    if (!db) { fprintf(stderr, "open failed (wrong key, wrong library, or file not readable)\n"); return 1; }
    bool header = false;
    const char *sql = argc > 4 ? argv[4] : "SELECT * FROM message ORDER BY createTime DESC LIMIT 20";
    printf("== %s ==\n", sql);
    if (!satori::WcdbQuery(db, sql, PrintRow, &header)) fprintf(stderr, "query failed: %s\n", satori::WcdbError(db));
    satori::WcdbClose(db);
    return 0;
}
