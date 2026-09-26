#include "wcdb.h"
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>

namespace satori {
// SQLite open flags and result codes; values are fixed by the public SQLite C API.
constexpr int kOpenReadOnly = 0x00000001;
constexpr int kOpenReadWrite = 0x00000002;
constexpr int kRow = 100;
constexpr int kDone = 101;

struct Wcdb {
    void *library;
    void *connection;
    int (*open_v2)(const char *, void **, int, const char *);
    int (*close_v2)(void *);
    int (*key)(void *, const void *, int);
    int (*exec)(void *, const char *, int (*)(void *, int, char **, char **), void *, char **);
    void (*free_memory)(void *);
    int (*prepare_v2)(void *, const char *, int, void **, const char **);
    int (*step)(void *);
    int (*finalize)(void *);
    int (*column_count)(void *);
    const char *(*column_name)(void *, int);
    const char *(*column_text)(void *, int);
    long long (*column_int64)(void *, int);
    int (*column_type)(void *, int);
    char error[256];
};

namespace {
template <typename T>
bool Resolve(void *library, const char *name, T *target) {
    void *symbol = dlsym(library, name);
    if (!symbol) return false;
    *target = reinterpret_cast<T>(symbol);
    return true;
}
void Fail(Wcdb *db, const char *message) { snprintf(db->error, sizeof(db->error), "%s", message ? message : "error"); }
void Release(Wcdb *db) {
    if (db->connection) db->close_v2(db->connection);
    if (db->library) dlclose(db->library);
    free(db);
}
} // namespace

Wcdb *WcdbOpen(const char *library, const char *path, const void *key, int key_size, int read_only) {
    if (!library || !path) return nullptr;
    auto *db = static_cast<Wcdb *>(calloc(1, sizeof(Wcdb)));
    if (!db) return nullptr;
    db->library = dlopen(library, RTLD_NOW | RTLD_LOCAL);
    if (!db->library) { Fail(db, dlerror()); free(db); return nullptr; }
    if (!Resolve(db->library, "sqlite3_open_v2", &db->open_v2) ||
        !Resolve(db->library, "sqlite3_close_v2", &db->close_v2) ||
        !Resolve(db->library, "sqlite3_exec", &db->exec) ||
        !Resolve(db->library, "sqlite3_free", &db->free_memory) ||
        !Resolve(db->library, "sqlite3_prepare_v2", &db->prepare_v2) ||
        !Resolve(db->library, "sqlite3_step", &db->step) ||
        !Resolve(db->library, "sqlite3_finalize", &db->finalize) ||
        !Resolve(db->library, "sqlite3_column_count", &db->column_count) ||
        !Resolve(db->library, "sqlite3_column_name", &db->column_name) ||
        !Resolve(db->library, "sqlite3_column_text", &db->column_text) ||
        !Resolve(db->library, "sqlite3_column_int64", &db->column_int64) ||
        !Resolve(db->library, "sqlite3_column_type", &db->column_type)) {
        Fail(db, "missing sqlite3 symbol");
        Release(db);
        return nullptr;
    }
    // sqlite3_key is optional: plain SQLite (without SQLCipher) has no encryption.
    Resolve(db->library, "sqlite3_key", &db->key);
    const int flags = read_only ? kOpenReadOnly : kOpenReadWrite;
    if (db->open_v2(path, &db->connection, flags, nullptr) != 0 || !db->connection) {
        Fail(db, "sqlite3_open_v2 failed");
        Release(db);
        return nullptr;
    }
    if (key && key_size > 0) {
        if (!db->key) {
            Fail(db, "database is encrypted but the library has no sqlite3_key");
            Release(db);
            return nullptr;
        }
        if (db->key(db->connection, key, key_size) != 0) {
            Fail(db, "sqlite3_key failed");
            Release(db);
            return nullptr;
        }
    }
    return db;
}

void WcdbClose(Wcdb *db) { if (db) Release(db); }

bool WcdbQuery(Wcdb *db, const char *sql, WcdbRow callback, void *context) {
    if (!db || !sql || !callback) return false;
    void *stmt = nullptr;
    if (db->prepare_v2(db->connection, sql, -1, &stmt, nullptr) != 0 || !stmt) {
        Fail(db, "prepare failed");
        return false;
    }
    bool ok = true;
    for (;;) {
        const int rc = db->step(stmt);
        if (rc == kRow) {
            if (!callback(db, stmt, context)) break;
        } else if (rc == kDone) {
            break;
        } else {
            Fail(db, "step failed");
            ok = false;
            break;
        }
    }
    db->finalize(stmt);
    return ok;
}

bool WcdbExec(Wcdb *db, const char *sql) {
    if (!db || !sql) return false;
    char *message = nullptr;
    if (db->exec(db->connection, sql, nullptr, nullptr, &message) != 0) {
        Fail(db, message ? message : "exec failed");
        if (message) db->free_memory(message);
        return false;
    }
    return true;
}

const char *WcdbError(Wcdb *db) { return db ? db->error : "no database"; }
int WcdbColumns(Wcdb *db, void *stmt) { return db && stmt ? db->column_count(stmt) : 0; }
const char *WcdbName(Wcdb *db, void *stmt, int column) { return db && stmt ? db->column_name(stmt, column) : nullptr; }
const char *WcdbText(Wcdb *db, void *stmt, int column) { return db && stmt ? db->column_text(stmt, column) : nullptr; }
long long WcdbInt(Wcdb *db, void *stmt, int column) { return db && stmt ? db->column_int64(stmt, column) : 0; }
bool WcdbIsNull(Wcdb *db, void *stmt, int column) { return !(db && stmt) || db->column_type(stmt, column) == 5 /* SQLITE_NULL */; }
} // namespace satori
