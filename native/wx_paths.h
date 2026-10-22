#pragma once
// 模块在宿主私有目录里落盘的位置。
//
// 宿主随时能看见自己的 files/：模块名出现在文件名里，等于把模块写在宿主的目录上
// （检测器按 satori / xposed / zygisk 之类的名字扫一遍就能命中）。所以全部状态收进一个
// 不以模块命名的隐藏目录，文件名也只用宿主自己的命名风格。
//
// 只用 <files>：cache 会被系统清掉（key 与名字缓存不能丢），外部存储需要额外权限。
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <sys/stat.h>

namespace satori {

constexpr const char *kStoreDir = ".svc";

inline void StoreDir(char *out, size_t size, const char *data_dir) {
    snprintf(out, size, "%s/files/%s", data_dir, kStoreDir);
}

inline void StorePath(char *out, size_t size, const char *data_dir, const char *leaf) {
    snprintf(out, size, "%s/files/%s/%s", data_dir, kStoreDir, leaf);
}

// 0700：同 uid 的宿主进程本来就读得到，但目录不该对沙箱外的任何东西敞开。
inline bool EnsureStoreDir(const char *data_dir) {
    char dir[560];
    StoreDir(dir, sizeof(dir), data_dir);
    if (mkdir(dir, 0700) && errno != EEXIST) return false;
    struct stat info{};
    return !stat(dir, &info) && S_ISDIR(info.st_mode);
}

// 收拢旧版本的痕迹：建 .svc，把 files/satori-wx/enabled 的值搬进 .svc/enabled，
// 再删掉 files/ 下所有以 satori-wx 开头的条目。返回删掉的条目数；建不出目录时返回 -1。
//
// 只认自己的名字，所以对宿主的其他文件是只读的；失败也不阻塞启动——调用方只记一行。
int MigrateLegacy(const char *data_dir);

} // namespace satori
