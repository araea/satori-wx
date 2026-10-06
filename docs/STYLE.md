# 代码规范

知言、知弦（satori-qq）与 acumen 三个仓库共用同一套底线，再按语言落到各自的工具上。

## 通用

- `.editorconfig`：UTF-8、LF、文件末尾换行、去行尾空白、4 空格（Markdown、JSON、CSS 为 2 空格）。三个仓库同一份。
- 提交信息写成 `类型(范围): 一句话`，类型取 feat / fix / refactor / style / docs / test / chore / release，正文讲为什么。
- 不留死代码：不参与构建的文件、没人调用的函数直接删，历史在 git 里。
- Shell 脚本里写 `CDPATH='' cd -- …`，不写 `CDPATH= cd`；脚本用 `shellcheck` 过一遍。

## C++（`native/`、`tests/`、`tools/`）

- 标准 C++20，不带标准库（`-nostdinc++ -nostdlib++`，`-Wall -Wextra -Werror`）：模块注入微信进程，不能再带一份 libc++。
  所以字符串、缓冲、容器用 `textbuf.h` 这类自带的小工具，不用 STL。
- 版式交给 `.clang-format`（4 空格、120 列、右对齐指针）：`clang-format -i <文件>`，`tests/run.sh` 开头会检查。
  `vendor/` 与 `zygisk.hpp` 不改。
- 一个函数一件事。路由、请求头解析、业务处理各是各的函数（`server.cpp` 的 `Http()` 就是按这个拆的），
  反复出现的「401/403」「登录头与登录查找」「打印并回 200/500」收成一个辅助函数。
- 源文件清单在 `build.sh` 的两张表里（`SERVER_CORE`、`MODULE_ONLY`），新增文件要登记；测试在 `tests/run.sh` 里一行一个。

## Java（`app/`）

- 语言级别 Java 17：`javac --release 17`，D8 脱糖到 min-api 26。record、switch 表达式、`instanceof` 模式匹配可用，
  `switch` 上的模式匹配（Java 21）不用。
- 只用 Android 26 有的类库，或 D8 能回填的方法。`javac` 对着 JDK 的类库编译而不是 `android.jar`，较新的 API
  编译期查不出来。
- import 排成一块：静态在前，其余按 ASCII 序；不写行内全限定名。`tools/java-imports.py --check` 检查，`app/test.sh` 会跑它。

## Python（`tools/`、`tests/`）

4 空格，标准库优先，脚本顶部写清用途与用法。
