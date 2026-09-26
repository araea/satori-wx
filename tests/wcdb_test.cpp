// Host tests for the read-only WCDB/SQLCipher client.
// They never touch a real WeChat database: a plaintext SQLite file is created in a temp dir
// and read back through the same dlopen/dlsym path the module uses. Set SATORI_WCDB_LIB to a
// library exporting the SQLite C API (libWCDB.so on device, libsqlite3.so in Termux).
#include "wcdb.h"
#include <fcntl.h>
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
struct Rows {
    int count = 0;
    long long sum = 0;
    char first[64] = {};
    char second[64] = {};
};
bool Collect(satori::Wcdb *db, void *stmt, void *context) {
    auto *rows = static_cast<Rows *>(context);
    Check(strcmp(satori::WcdbName(db, stmt, 0), "a") == 0, "column name a");
    Check(strcmp(satori::WcdbName(db, stmt, 1), "b") == 0, "column name b");
    const char *text = satori::WcdbText(db, stmt, 0);
    if (rows->count == 0) snprintf(rows->first, sizeof(rows->first), "%s", text ? text : "");
    if (rows->count == 1) snprintf(rows->second, sizeof(rows->second), "%s", text ? text : "");
    rows->sum += satori::WcdbInt(db, stmt, 1);
    ++rows->count;
    return true;
}
bool StopAtFirst(satori::Wcdb *db, void *stmt, void *context) {
    (void)db; (void)stmt;
    ++*static_cast<int *>(context);
    return false;
}
} // namespace

int main() {
    const char *library = getenv("SATORI_WCDB_LIB");
    if (!library || !*library) { printf("wcdb tests: SKIP (set SATORI_WCDB_LIB)\n"); return 0; }
    const char *base = getenv("SATORI_ACCOUNT_TMP");
    if (!base || !*base) base = getenv("TMPDIR");
    if (!base || !*base) base = ".";
    char directory[512];
    snprintf(directory, sizeof(directory), "%s/satori-wcdb-XXXXXX", base);
    if (!mkdtemp(directory)) { fprintf(stderr, "mkdtemp failed\n"); return 2; }
    char path[640];
    snprintf(path, sizeof(path), "%s/test.db", directory);
    const int created = open(path, O_CREAT | O_WRONLY | O_CLOEXEC, 0600);
    if (created >= 0) close(created);

    satori::Wcdb *db = satori::WcdbOpen(library, path, nullptr, 0, 0);
    Check(db != nullptr, "open plaintext database");
    if (!db) { fprintf(stderr, "wcdb error: %s\n", satori::WcdbError(nullptr)); return 1; }
    Check(satori::WcdbExec(db, "CREATE TABLE t(a TEXT, b INTEGER)"), "create table");
    Check(satori::WcdbExec(db, "INSERT INTO t VALUES('hello', 42)"), "insert 1");
    Check(satori::WcdbExec(db, "INSERT INTO t VALUES('世界', 7)"), "insert 2");
    Rows rows;
    Check(satori::WcdbQuery(db, "SELECT a, b FROM t ORDER BY b", Collect, &rows), "query rows");
    Check(rows.count == 2, "row count");
    Check(rows.sum == 49, "integer values");
    Check(strcmp(rows.first, "世界") == 0, "first text");
    Check(strcmp(rows.second, "hello") == 0, "utf-8 text");
    int visited = 0;
    Check(satori::WcdbQuery(db, "SELECT a FROM t", StopAtFirst, &visited), "early stop is not an error");
    Check(visited == 1, "early stop visited one row");
    Check(!satori::WcdbQuery(db, "SELECT * FROM missing_table", Collect, &rows), "missing table fails");
    Check(satori::WcdbError(db)[0] != 0, "error text set");
    // An empty statement set must still be a clean success.
    Check(satori::WcdbQuery(db, "SELECT 1 WHERE 0", Collect, &rows), "empty result is ok");
    satori::WcdbClose(db);

    // Wrong library / missing file are reported, not crashed on.
    Check(satori::WcdbOpen("/nonexistent/libsqlite.so", path, nullptr, 0, 0) == nullptr, "missing library rejected");
    Check(satori::WcdbOpen(library, "/nonexistent/dir/x.db", nullptr, 0, 0) == nullptr, "unopenable path rejected");

    char cleanup[640];
    snprintf(cleanup, sizeof(cleanup), "%s", path);
    unlink(cleanup);
    rmdir(directory);
    if (failures) { fprintf(stderr, "%d wcdb test(s) failed\n", failures); return 1; }
    printf("wcdb tests: PASS\n");
    return 0;
}
