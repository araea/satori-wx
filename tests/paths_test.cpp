// 痕迹收敛的落盘迁移：只删自己的旧条目，只搬开关的值，别的文件一个都不许碰。
// 这段逻辑在启动时对宿主数据目录动手，坏掉的代价是「删错东西」，所以单独测。
#include "wx_paths.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

namespace {

int failures = 0;

void Check(bool ok, const char *what) {
    if (!ok) {
        printf("FAIL: %s\n", what);
        ++failures;
    }
}

bool Exists(const char *path) {
    struct stat info{};
    return stat(path, &info) == 0;
}

void Touch(const char *path, const char *text, const char *mode = "w") {
    FILE *file = fopen(path, mode);
    if (!file) return;
    fputs(text, file);
    fclose(file);
}

char g_root[256];

void Path(char *out, size_t size, const char *rest) { snprintf(out, size, "%s/files/%s", g_root, rest); }

void RemoveTree(const char *path) {
    struct stat info{};
    if (lstat(path, &info) != 0) return;
    if (!S_ISDIR(info.st_mode)) {
        unlink(path);
        return;
    }
    DIR *dir = opendir(path);
    if (dir) {
        for (dirent *entry; (entry = readdir(dir));) {
            if (!strcmp(entry->d_name, ".") || !strcmp(entry->d_name, "..")) continue;
            char child[700];
            snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
            RemoveTree(child);
        }
        closedir(dir);
    }
    rmdir(path);
}

void Reset() {
    RemoveTree(g_root);
    char files[400];
    snprintf(files, sizeof(files), "%s/files", g_root);
    mkdir(g_root, 0700);
    mkdir(files, 0700);
}

void MakeLegacyTree() {
    char path[600], child[700];
    Path(path, sizeof(path), "satori-wx");
    mkdir(path, 0700);
    snprintf(child, sizeof(child), "%s/enabled", path);
    Touch(child, "0");
    snprintf(child, sizeof(child), "%s/state.json", path);
    Touch(child, "{}");
    Path(path, sizeof(path), "satori-wx-tmp");
    mkdir(path, 0700);
    snprintf(child, sizeof(child), "%s/upload.bin", path);
    Touch(child, "partial");

    char buffer[600];
    const char *leaves[] = {"satori-wx-store.log", "satori-wx-dev-hello.out"};
    for (const char *leaf : leaves) {
        Path(buffer, sizeof(buffer), leaf);
        Touch(buffer, "x");
    }

    // 宿主的文件与另一个模块的目录：必须原样留着。
    Path(path, sizeof(path), "mmkv");
    mkdir(path, 0700);
    snprintf(child, sizeof(child), "%s/cache.dat", path);
    Touch(child, "keep");
    const char *others[] = {"KeyInfo.bin", "xlog", "satori-qq"};
    for (const char *leaf : others) {
        Path(buffer, sizeof(buffer), leaf);
        if (!strcmp(leaf, "xlog") || !strcmp(leaf, "satori-qq")) {
            mkdir(buffer, 0700);
        } else {
            Touch(buffer, "keep");
        }
    }
}

void TestMigration() {
    Reset();
    MakeLegacyTree();
    const int removed = satori::MigrateLegacy(g_root);
    Check(removed == 4, "four legacy entries removed (dir, tmp dir, two files)");

    char path[600];
    Path(path, sizeof(path), ".svc/enabled");
    Check(Exists(path), "switch moved into .svc");
    FILE *file = fopen(path, "r");
    Check(file != nullptr && fgetc(file) == '0', "switch keeps its value");
    if (file) fclose(file);

    const char *gone[] = {"satori-wx", "satori-wx-tmp", "satori-wx-store.log", "satori-wx-dev-hello.out"};
    for (const char *leaf : gone) {
        Path(path, sizeof(path), leaf);
        Check(!Exists(path), leaf);
    }
    const char *kept[] = {"mmkv", "KeyInfo.bin", "xlog", "satori-qq"};
    for (const char *leaf : kept) {
        Path(path, sizeof(path), leaf);
        Check(Exists(path), leaf);
    }

    struct stat info{};
    Path(path, sizeof(path), ".svc");
    Check(stat(path, &info) == 0 && (info.st_mode & 0777) == 0700, ".svc is 0700");

    Check(satori::MigrateLegacy(g_root) == 0, "second pass removes nothing");
}

void TestNewSwitchWins() {
    Reset();
    MakeLegacyTree();
    char path[600];
    Path(path, sizeof(path), ".svc");
    mkdir(path, 0700);
    Path(path, sizeof(path), ".svc/enabled");
    Touch(path, "1");
    satori::MigrateLegacy(g_root);
    FILE *file = fopen(path, "r");
    Check(file != nullptr && fgetc(file) == '1', "existing switch is not overwritten by the legacy one");
    if (file) fclose(file);
    Check(Exists(path), "switch still there");
}

void TestNoLegacy() {
    Reset();
    Check(satori::MigrateLegacy(g_root) == 0, "nothing to remove on a clean install");
    char path[600];
    Path(path, sizeof(path), ".svc");
    Check(Exists(path), ".svc is created anyway");
}

} // namespace

int main() {
    const char *root = getenv("SATORI_TMPROOT");
    snprintf(g_root, sizeof(g_root), "%s/paths-test-%d", root ? root : "/tmp", static_cast<int>(getpid()));
    TestMigration();
    TestNewSwitchWins();
    TestNoLegacy();
    if (failures != 0) {
        printf("paths tests: %d failure(s)\n", failures);
        return 1;
    }
    printf("paths tests: PASS\n");
    return 0;
}
