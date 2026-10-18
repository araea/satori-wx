# 知言应用

知言模块的原生管理界面（`com.satori.wx`）：查看服务是否连通、断在哪一环、下一步做什么；管理端口与令牌；开关保活看守；重新启动微信让配置生效。状态是订阅来的（root 侧 inotify），微信被冻结时沿用冻结前的状态，不会整页变空。同时是微信常驻通知上「唤醒锁」按钮的落点（`keepalive.WakeToggleReceiver` 把切换转给模块的 `POST /v1/internal/wakelock`）。设计规范见 [知言应用设计规范](../docs/app-design.md)。

零依赖：只用 Android 框架 API，不引入 androidx / Material Components。视图直接构造，不解析 XML 布局。

## 使用前提

- 已安装并启用知言模块（`satori_wx`），且重启过手机。
- 在 KernelSU / Magisk 里允许「知言」使用 Root。配置文件在 `/data/adb/modules/satori_wx/`，读写都要 Root。没有授权时首页停在「需要 Root 授权」，授权后点「重试」。

## 构建与测试（arm64 Termux）

需要 `openjdk-17`、`aapt`、`zipalign`、`apksigner`、`clang`、Python 3（`materialyoucolor` 与 `Pillow` 只在改色板或看截图时需要），以及 `libs/r8.jar`、`libs/json.jar`（被 gitignore，下载命令见 `build.sh` 顶部）。

```sh
./build.sh                 # → build/Zhiyan.apk（先自检令牌与图标生成物是否过期）
./test.sh                  # JVM：配置与服务端 ReadConfig 一致性、状态推导、对比度合约；有 su 时加跑 root 脚本与订阅测试
bash tests/ui/run.sh       # 真机：离屏渲染各状态页面并检查，截图在 build/design-tests/design-review/；测试包跑完自动卸载

python3 tools/make-tokens.py   # 改种子色后重新生成 res/values*/tokens.xml
python3 tools/make-icons.py    # 增删图标后重新生成 res/drawable/ms_*.xml 与启动图标
```

装机：

```sh
su -c "cp build/Zhiyan.apk /data/local/tmp/ && pm install -r /data/local/tmp/Zhiyan.apk"
```

签名密钥 `keystore/zhiyan.keystore` 被 gitignore，只在缺失时生成。换密钥后已安装的应用只能卸载重装。

## 结构

| 路径 | 职责 |
| --- | --- |
| `src/com/satori/wx/core/Conf.java` | 配置解析与写出，规则逐条对齐 `native/server.cpp` 的 `ReadConfig` |
| `src/com/satori/wx/core/Root.java` | `su -M`（全局挂载命名空间）：探测模块、微信进程、冻结状态、模块发布的状态文件与保活看守，原子写配置，开关看守，重新启动微信 |
| `src/com/satori/wx/core/Watcher.java` | root 侧的变化订阅：一条 `inotifyd` 盯微信 cgroup、发布的状态文件、看守与模块目录，变了就通知应用重新检查 |
| `src/com/satori/wx/core/Api.java` | 回环 HTTP：状态与元信息（只读） |
| `src/com/satori/wx/core/Status.java` | 事实 → 结论、链路、生效与否、诊断报告（纯函数）；暂停时沿用冻结前的状态 |
| `src/com/satori/wx/keepalive/WakeToggleReceiver.java` | 微信常驻通知唤醒锁按钮的落点，转发到 `internal/wakelock` |
| `src/com/satori/wx/ui/` | 令牌读口、M3E 组件与两个页面；`MainActivity` 负责导航、线程与系统集成 |
| `tools/` | 颜色令牌与图标的生成脚本 |
| `tests/` | JVM 测试、`conf_parity.cpp`（服务端判定）、`ui/` 真机设计冒烟 |
