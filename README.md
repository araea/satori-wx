# 知言（satori-wx）

微信 `com.tencent.mm` 的实验性 Zygisk 模块。v0.5.0 在 **纯 native Satori v1 服务端**
（C++ + POSIX socket，无 DEX、Java 助手、APK、ArtMethod 偏移或 hook 引擎）之上，
加入一个**只读的微信账号身份适配层**。

账号身份来自微信自己持久化的 SharedPreferences（同 uid 直接读文件，不 hook、不改写、
不访问数据库）。已登录时 `READY` / `/v1/meta` 会带真实 `logins`；退出登录或清除身份后
快照随事件总线更新。**消息接收、发送以及其余 37 个业务方法尚未接入**：账号快照的
当前账号 `features` 已声明 8 个真正实现的方法（见下表）；未实现的方法返回 404，不会伪造成功。
旧 v0.2.x 的 `native/wx.cpp`、`native/jni_helpers.h`、`src/`、`AndroidManifest.xml` 和 `libs/`
保留作研究资料，不参与任何当前构建。版本来源改为根目录 `module.prop`。

## 构建与验证

在 arm64 Termux 中，需要 clang、Python 3、readelf、patchelf、zip；不需要 JDK、Android SDK 或 D8。

```sh
./build.sh               # 默认 native 服务端 + 账号只读探针
./tests/run.sh           # HTTP/WebSocket socket 测试 + 账号解析/事件测试 + 探针资源/还原测试
./build.sh probe         # 独立、可选的 JNI 边界探针
```

产物：

- `build/satori-wx-server-v0.5.0.zip`，模块 ID `satori_wx`。
- `build/module-server/`，服务端模块目录。
- `build/satori-wx-account`，读取某个微信数据目录并打印推导出的登录事件（诊断用，不联网）。
- `build/satori-wx-wcdb`，只读 SQLCipher/SQLite 客户端，用微信自己的 libWCDB 读导出数据库（诊断用）。
- `build/satori-wx-probe-v0.5.0.zip`，模块 ID `satori_wx_probe`。

构建检查 AArch64、Zygisk 导出入口、动态依赖白名单及 DEX/旧引导标记。
构建会移除 Termux RUNPATH，运行时不依赖 Termux 库目录。C++ 不链接共享 STL；JSON 解析器为静态编译的 cJSON 1.7.19（MIT，许可证随包附带）。
构建与测试都不安装模块、不重启设备、不启动或操作微信。

## 安装与连接

通过 Magisk / KernelSU 管理器安装 server ZIP，并在需要时重启设备使模块生效。
要求已有可用的 Zygisk 实现。使用 Zygisk Next 时，按其官方要求配置。
安装器首次生成 256 位随机 token，升级保留现有配置：

```text
/data/adb/modules/satori_wx/satori-wx.conf
```

格式（端口可改，token 使用 32–128 位字母、数字、`-` 或 `_`）：

```ini
port=5601
token=<安装器生成的令牌>
```

配置权限为 `0600`；缺失、过长、重复字段或无效 token 时服务端不启动。
配置在 `preAppSpecialize` 读取，文件描述符立即关闭；服务在 `postAppSpecialize` 启动。
只在精确匹配的微信主进程内运行。非目标进程和 system_server 请求卸载模块。
服务端只监听 **127.0.0.1**，随微信主进程结束而退出；配置更改在下一次进程启动生效。

Satori 客户端填写：

- API：`http://127.0.0.1:5601/v1`
- Events：`ws://127.0.0.1:5601/v1/events`
- Token：配置文件的 `token` 值

可用接口：

| 接口 | 当前行为 |
| --- | --- |
| `POST /v1/meta` | 已登录时返回只读身份快照 `{"logins":[...],"proxy_urls":[]}`；无账号时 `logins` 为空 |
| `POST /v1/meta/webhook.create` / `webhook.delete` | 注册/注销 WebHook（`url` 必填、`token` 可选）；标准可选功能 |
| `POST /v1/internal/status` | 实验版版本、native 状态、backend unavailable；项目自定义诊断接口 |
| `POST /v1/login.get` | 返回已登记账号快照；未登录或身份不匹配时返回 403 |
| `POST /v1/{resource}.{method}` | 37 个标准方法的参数校验及 native 后端分发；已实现 `message.get/list`、`user.get`、`friend.list`、`guild.get/list`、`channel.get/list`（读只读库），其余返回 404 |
| `GET /v1/events` | WebSocket upgrade；10 秒内 IDENTIFY；READY、登录事件与 PING/PONG |

HTTP 使用 `Authorization: Bearer <token>`。缺失 token 返回 401，错误 token 返回 403。
账号 API 要求 `Satori-Platform: wechat` 和 `Satori-User-ID`。
未知接口返回 404，已有 RPC 接口错误方法返回 405。
`login.list` 不属于这里实现的 Satori v1 API；登录集合从 meta / READY 获取。

WebSocket 按 Satori 的 `op=3` + `body.token` 鉴权；成功后 `op=4`，`op=1` 心跳回复 `op=2`。
每 10 秒发送一次 Satori PING；连续 30 秒没有收到则断开。
支持掩码、文本分片、控制帧交错、TCP 分包/合包和关闭握手。
已实现 native 事件生产队列、EVENT/META 和 64 条有界历史；登录事件不参与回放。窗口外序号用 4009 拒绝恢复。
账号适配层每 3 秒只读扫描一次偏好文件，身份出现/变化/消失会广播对应的 `login-added` /
`login-updated` / `login-removed`，并同步 meta 与 READY。
消息等业务事件源尚未实现；广播、状态同步和断线回放已通过独立测试后端与账号端到端测试验证。
401/403 是 HTTP 状态；4000/4004/4009 是本项目的 WebSocket 应用关闭码。

资源限制：8 个并发连接、8 KiB HTTP 头、16 KiB 请求体/WS 消息、有界发送缓冲。
单个 `poll` 线程处理读写、背压和单调时钟超时；空闲时阻塞等待。
HTTP 每次响应后关闭连接；暂不提供 TLS、chunked 请求体、资源代理或真实微信消息事件。

## WebHook（标准可选）

除 WebSocket 外，服务端实现 Satori 的可选 WebHook 推送：应用通过已鉴权的
`POST /v1/meta/webhook.create`（`{"url":"http://...", "token":"..."}`）登记地址，
最多 4 个；每个 EVENT / META 信号都会以 POST 推送到所有登记地址，请求头带
`Satori-Opcode`（EVENT=0，META=5），有 token 时带 `Authorization: Bearer <token>`，
请求体就是信号 body（Event / Meta 对象），与标准一致。`webhook.delete` 按 `url` 注销。
推送在独立线程、有界队列上进行，事件循环永不阻塞；队列满或推送失败只计数不重试。
没有 TLS 客户端，因此只接受 `http://`，`https://` 在登记时返回 400；只有持有服务端 token
的应用才能登记。`/v1/internal/capabilities` 的 `webhook` 为 true，并报告当前登记数。

停用或卸载模块后，结束已有微信进程并按模块管理器要求重启；已有进程中的线程不会因删除模块目录自行退出。

## 只读账号身份

适配层在微信主进程内以微信自己的 uid 读取（只读、不解析数据库）:

- `shared_prefs/com.tencent.mm_preferences.xml`：`login_weixin_username`（wxid）、`last_login_uin`、
  `isLogin`、`last_login_alias`、`last_login_nick_name`、`last_login_bind_mobile`、`login_user_name`。
- `shared_prefs/auth_info_key_prefs.xml`：`_auth_uin`，作为 uin 缺失时的回退。

推导出的 Satori 登录以 wxid 作为 `user.id`，昵称与别名作为 `user.nick` / `user.name`。
`sn` 在账号出现时分配、更新时复用，账号切换会先 `login-removed` 再以新 `sn`
`login-added`，避免把两个身份折叠成一个。

用途与边界：

- `status=1` 仅表示微信持久化的 `isLogin=true`；网络层的 CONNECT / DISCONNECT 未区分，
  崩溃后的短暂过期标记可能造成误报上线。`status=0` 表示已登出。
- 这是**尽力而为的观测**，依赖微信未公开的偏好格式；格式变化时字段会失效或回退为空，
  不会让服务崩溃或阻塞。解析有界（单文件 128 KiB、160 条键值），不做 XML 外部实体展开。
- 不含头像 URL、好友、群、消息等数据；这些需要后续的数据库或网络层适配。
- 诊断：`build/satori-wx-account <数据目录>` 打印推导结果与将发布的登录事件，不写入、不联网。

## 可选 native 探针

`./build.sh probe` 构建独立探针，不是服务端的运行依赖。
它观察成功的 `RegisterNatives` 注册，原调用结果和异常原样保留；仅在可读、非执行数据页上
比较并交换 JNI 表中的一个函数指针。仍然属于实验性全局表修改，并非“零侵入”。
不替换微信 native 方法，不采集消息正文或数据库密钥。

worker attach 成功后才启用观测，避免主线程等待 Java 初始化；因此可能漏掉最早的一部分注册。
观察窗口 90 秒、最多 20000 次成功注册调用，1024 条有界队列；满时丢弃并计数。
到期停止生产、还原自有槽位、排空并释放所有全局引用。遇到外部槽位改动不覆盖；
`mprotect` 失败不写页面。模块保留映射，保障已缓存 wrapper 的调用仍可转发。

日志位于 Zygisk 提供的应用数据目录下 `files/satori-wx-probe/`：
`boundary.log`、`maps.log`、`meta.log`。每次运行覆盖旧日志，单文件最多 16 MiB。
`boundary.log` 的时间为 monotonic 毫秒，库偏移为 backing-file offset。
还原结果、保护位恢复、丢弃数及 I/O 状态写入 meta；失败必须结合 logcat 判断。
JNI 表来自主线程；CheckJNI 或其他模块使用不同表时，不保证覆盖所有注册。

## 参考与下一步

- [ZygiskNext](https://github.com/LSPosed/ZygiskNext)：运行环境与公开接口参考。
- [标准 Zygisk 模块接口](https://github.com/topjohnwu/zygisk-module-sample)：当前沿用本地 API v4 头。
- [Satori HTTP API](https://satori.chat/zh-CN/protocol/api.html)、[事件](https://satori.chat/zh-CN/protocol/events.html)、[元信息](https://satori.chat/zh-CN/advanced/meta.html)。
- [研究记录与已知边界](docs/native-server.md)。
- [只读账号身份说明](docs/wechat-account.md)。
- [消息后端设计（native、低特征）](docs/wechat-store.md)。
- [v0.5.0 协议覆盖矩阵](docs/satori-conformance.md)。
- [v0.4.0 安装与重启验收记录](docs/deployment-v0.4.0.md)。

下一步是在账号身份之上实现微信 native 业务适配层。设计（消息库加密、只用微信自己的 libWCDB
做只读客户端的干净路线、密钥的单点捕获、分步计划）见 [消息后端设计](docs/wechat-store.md)；
每个方法只有在后端真正实现后才写入账号 `features`。
