// 旧版本把状态直接写在 files/ 下，名字里带模块名。升级后统一挪进 .svc，旧条目逐个删掉，
// 否则宿主目录里会一直留着「这里装过什么」。
//
// 只碰以 satori-wx 开头的条目：这些名字按定义都是模块自己留下的（开关、状态、名字缓存、
// 临时目录、日志，以及开发探针的 -dev / -probe / -observe）。
#include "wx_paths.h"

#include <dirent.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

namespace satori {
namespace {

constexpr const char *kLegacyPrefix = "satori-wx";

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
            char child[1200];
            snprintf(child, sizeof(child), "%s/%s", path, entry->d_name);
            RemoveTree(child);
        }
        closedir(dir);
    }
    rmdir(path);
}

// 开关是唯一有状态的东西：先搬值，再删目录。新位置已经有值时以新的为准（用户可能在管理页改过）。
void MigrateFlag(const char *data_dir) {
    char legacy[600];
    snprintf(legacy, sizeof(legacy), "%s/files/%s/enabled", data_dir, kLegacyPrefix);
    char flag[600];
    StorePath(flag, sizeof(flag), data_dir, "enabled");
    if (access(flag, F_OK) == 0) return;
    FILE *file = fopen(legacy, "re");
    if (!file) return;
    const int value = fgetc(file);
    fclose(file);
    if (value != '0' && value != '1') return;
    const int fd = open(flag, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    const char byte = static_cast<char>(value);
    // 单字节写失败只可能是磁盘满或目录被删，调用方下一次读不到时按「开启」处理。
    ssize_t written = write(fd, &byte, 1);
    (void)written;
    close(fd);
}

} // namespace

int MigrateLegacy(const char *data_dir) {
    if (!data_dir || !*data_dir) return -1;
    if (!EnsureStoreDir(data_dir)) return -1;
    MigrateFlag(data_dir);
    char files[520];
    snprintf(files, sizeof(files), "%s/files", data_dir);
    DIR *dir = opendir(files);
    if (!dir) return 0;
    int removed = 0;
    for (dirent *entry; (entry = readdir(dir));) {
        if (strncmp(entry->d_name, kLegacyPrefix, strlen(kLegacyPrefix)) != 0) continue;
        char path[1200];
        snprintf(path, sizeof(path), "%s/%s", files, entry->d_name);
        RemoveTree(path);
        ++removed;
    }
    closedir(dir);
    return removed;
}

} // namespace satori
