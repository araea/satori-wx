// 用服务端真正的 satori::ReadConfig 判定一批配置文件，供 tests/ConfTest 与 Java 的 Conf.parse 逐条比对。
// 用法：conf-parity <文件>…；每个文件输出一行：ok <port> <send> <allow> 或 bad。
#include "server.h"
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>

int main(int argc, char **argv) {
    for (int i = 1; i < argc; ++i) {
        const int fd = open(argv[i], O_RDONLY | O_CLOEXEC);
        satori::Config config;
        const bool ok = fd >= 0 && satori::ReadConfig(fd, &config);
        if (fd >= 0) close(fd);
        if (ok) printf("ok %u %s %s\n", config.port, config.send ? "on" : "off", config.send_allow[0] ? config.send_allow : "-");
        else printf("bad\n");
    }
    return 0;
}
