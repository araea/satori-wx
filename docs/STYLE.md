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

## 管理页（`webroot/`）

- 原生 ES 模块，不打包、不引第三方库；`kit.css`、`kit.js`、`icons.js` 与 satori-qq 逐字相同，改一边就同步另一边。
- 页面不拼 shell 脚本：所有 root 动作都是 `ctl.sh` 的子命令，参数逐个单引号转义后传入，`ctl.sh` 自己再校验。
- 视觉只用 Material 3 Expressive 的令牌与形状；颜色只在 `index.html` 的 `:root` 里定义。

## Python（`tools/`、`tests/`）

4 空格，标准库优先，脚本顶部写清用途与用法。
